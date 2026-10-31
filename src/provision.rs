//! Wi-Fi setup through the watch's own network, for a watch with no (or wrong) saved Wi-Fi.
//!
//! The watch scans for networks first (an access point can't scan without dropping its clients),
//! then becomes a WPA2 access point with a one-time password, and serves a small page on it. A
//! wildcard DNS makes every name resolve to the watch, so the phone's sign-in sheet opens by
//! itself; the page lists the scanned networks and takes the home network's name and password.
//! Pure parts (the pages, form handling, the DNS reply) live in `putting_core::portal`.

use std::net::{Ipv4Addr, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use esp_idf_svc::hal::delay::FreeRtos;
use esp_idf_svc::http::server::{Configuration as ServerConfig, EspHttpServer};
use esp_idf_svc::http::Method;
use esp_idf_svc::io::Write;
use esp_idf_svc::wifi::{AccessPointConfiguration, AuthMethod, BlockingWifi, ClientConfiguration, Configuration, EspWifi};
use putting_core::portal::{collect_networks, dns_reply, joining_page, message_page, parse_form, portal_page, validate_connect};
use putting_core::update::make_password;

type Wifi = BlockingWifi<EspWifi<'static>>;

/// Give up on setup after this long with nobody completing it.
const TIMEOUT: Duration = Duration::from_secs(10 * 60);
/// A reply needs a moment to reach the phone before the access point goes away.
const FLUSH: Duration = Duration::from_millis(1500);
const AP_CHANNEL: u8 = 6;

/// What the setup is doing, for the screen.
pub enum Status<'a> {
    Scanning,
    /// The access point is up and waiting for the phone.
    Waiting { ap_ssid: &'a str, ap_pass: &'a str, ip: Ipv4Addr, phone_joined: bool },
}

/// Scan, bring up the setup network, and wait for the phone to submit the home network's name and
/// password. Returns (ssid, password). The access point is torn down before returning, so the
/// caller can go straight to joining the network.
pub fn run(wifi: &mut Wifi, mut status: impl FnMut(Status)) -> Result<(String, String), String> {
    status(Status::Scanning);
    let nets = scan(wifi)?;
    log::info!("[provision] scan: {} networks", nets.len());

    // A one-time WPA2 password: the home Wi-Fi password is typed into a page served over this link,
    // so it must not be an open network.
    let mut rnd = [0u8; 64];
    unsafe { esp_idf_svc::sys::esp_fill_random(rnd.as_mut_ptr().cast(), rnd.len()) };
    let pass = make_password(&rnd);
    let mut mac = [0u8; 6];
    unsafe { esp_idf_svc::sys::esp_read_mac(mac.as_mut_ptr(), esp_idf_svc::sys::esp_mac_type_t_ESP_MAC_WIFI_SOFTAP) };
    let ap_ssid = format!("HonuWatch-{:02X}{:02X}", mac[4], mac[5]);

    let ap = Configuration::AccessPoint(AccessPointConfiguration {
        ssid: ap_ssid.as_str().try_into().map_err(|_| "Couldn't name the setup network.".to_string())?,
        password: pass.as_str().try_into().map_err(|_| "Couldn't set the setup password.".to_string())?,
        auth_method: AuthMethod::WPA2Personal,
        channel: AP_CHANNEL,
        max_connections: 2,
        ..Default::default()
    });
    let _ = wifi.stop();
    wifi.set_configuration(&ap).map_err(|e| format!("Couldn't start the watch's Wi-Fi setup network: {e}"))?;
    wifi.start().map_err(|e| format!("Couldn't start the watch's Wi-Fi setup network: {e}"))?;
    wifi.wait_netif_up().map_err(|e| format!("Couldn't start the watch's Wi-Fi setup network: {e}"))?;
    let ip = wifi.wifi().ap_netif().get_ip_info().map_err(|e| format!("No address on the setup network: {e}"))?.ip;
    log::info!("[provision] setup AP '{ap_ssid}' up, portal at http://{ip}/");

    // Wildcard DNS: every name -> the watch.
    let stop = Arc::new(AtomicBool::new(false));
    let dns = {
        let stop = stop.clone();
        std::thread::Builder::new().stack_size(4096).spawn(move || dns_thread(ip, stop)).map_err(|e| e.to_string())?
    };

    // The page.
    let submitted: Arc<Mutex<Option<(String, String)>>> = Arc::new(Mutex::new(None));
    let page = Arc::new(portal_page(&nets));
    let mut server = EspHttpServer::new(&ServerConfig { uri_match_wildcard: true, stack_size: 8192, ..Default::default() })
        .map_err(|e| format!("Couldn't start the setup page: {e}"))?;
    {
        let page = page.clone();
        server
            .fn_handler("/", Method::Get, move |req| {
                req.into_response(200, None, &[("Content-Type", "text/html"), ("Cache-Control", "no-store")])?
                    .write_all(page.as_bytes())
            })
            .map_err(|e| e.to_string())?;
    }
    {
        let submitted = submitted.clone();
        server
            .fn_handler("/connect", Method::Post, move |mut req| {
                let mut body = Vec::new();
                let mut chunk = [0u8; 256];
                while body.len() < 1024 {
                    let n = req.read(&mut chunk)?;
                    if n == 0 {
                        break;
                    }
                    body.extend_from_slice(&chunk[..n]);
                }
                let form = parse_form(&String::from_utf8_lossy(&body));
                let (status, html) = match validate_connect(&form) {
                    Ok((ssid, pass)) => {
                        let html = joining_page(&ssid);
                        *submitted.lock().unwrap() = Some((ssid, pass));
                        (200, html)
                    }
                    Err(msg) => (400, message_page("Please check", msg, true)),
                };
                req.into_response(status, None, &[("Content-Type", "text/html")])?.write_all(html.as_bytes())
            })
            .map_err(|e| e.to_string())?;
    }
    {
        // Connectivity probes (/generate_204, /hotspot-detect.html, /connecttest.txt, ...) and
        // anything else: send the phone to the page.
        let location = format!("http://{ip}/");
        server
            .fn_handler("/*", Method::Get, move |req| {
                req.into_response(302, None, &[("Location", location.as_str()), ("Content-Length", "0")])?.flush()
            })
            .map_err(|e| e.to_string())?;
    }

    // Wait for the phone.
    let started = Instant::now();
    let mut joined = false;
    status(Status::Waiting { ap_ssid: &ap_ssid, ap_pass: &pass, ip, phone_joined: false });
    let result = loop {
        if let Some(creds) = submitted.lock().unwrap().take() {
            break Ok(creds);
        }
        if started.elapsed() > TIMEOUT {
            break Err("Wi-Fi setup timed out.".to_string());
        }
        let now_joined = station_count() > 0;
        if now_joined != joined {
            joined = now_joined;
            status(Status::Waiting { ap_ssid: &ap_ssid, ap_pass: &pass, ip, phone_joined: joined });
        }
        FreeRtos::delay_ms(100);
    };
    if result.is_ok() {
        FreeRtos::delay_ms(FLUSH.as_millis() as u32); // let the "check your watch" reply reach the phone
    }

    stop.store(true, Ordering::Relaxed);
    let _ = dns.join();
    drop(server);
    let _ = wifi.stop();
    result
}

/// Scan for networks (station mode) and tidy the list.
fn scan(wifi: &mut Wifi) -> Result<Vec<putting_core::portal::Network>, String> {
    let _ = wifi.stop();
    wifi.set_configuration(&Configuration::Client(ClientConfiguration::default()))
        .map_err(|e| format!("Wi-Fi setup failed: {e}"))?;
    wifi.start().map_err(|e| format!("Wi-Fi start failed: {e}"))?;
    FreeRtos::delay_ms(100);
    let aps = wifi.scan().map_err(|e| format!("Couldn't scan for networks: {e}"))?;
    let _ = wifi.stop();
    Ok(collect_networks(
        aps.into_iter().map(|a| (a.ssid.to_string(), a.signal_strength as i32, a.auth_method == Some(AuthMethod::None))),
    ))
}

/// Phones currently joined to the setup network.
fn station_count() -> u32 {
    let mut list: esp_idf_svc::sys::wifi_sta_list_t = unsafe { core::mem::zeroed() };
    if unsafe { esp_idf_svc::sys::esp_wifi_ap_get_sta_list(&mut list) } == 0 {
        list.num as u32
    } else {
        0
    }
}

fn dns_thread(ip: Ipv4Addr, stop: Arc<AtomicBool>) {
    let Ok(sock) = UdpSocket::bind("0.0.0.0:53") else {
        log::error!("[provision] couldn't open the DNS port");
        return;
    };
    let _ = sock.set_read_timeout(Some(Duration::from_millis(200)));
    let mut buf = [0u8; 512];
    while !stop.load(Ordering::Relaxed) {
        if let Ok((n, from)) = sock.recv_from(&mut buf) {
            if let Some(reply) = dns_reply(&buf[..n], ip.octets()) {
                let _ = sock.send_to(&reply, from);
            }
        }
    }
}
