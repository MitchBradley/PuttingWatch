//! Update mode: a separate boot that runs only the updater (no IMU, tracking or audio), so
//! Wi-Fi and TLS have the RAM to themselves. The Bye screen's Update button sets a flag in RTC
//! memory and restarts; the next boot sees the flag plus a software-reset reason and comes here.
//! BOOT cancels, restarting into the normal app.
//!
//! Flow: join Wi-Fi from saved credentials -> NTP (TLS needs a sane clock) -> fetch and verify
//! manifest.json over HTTPS -> decide (up to date / available / needs a USB install).

use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use embedded_graphics::pixelcolor::Rgb565;
use embedded_graphics::prelude::*;
use esp_idf_svc::eventloop::EspSystemEventLoop;
use esp_idf_svc::hal::delay::FreeRtos;
use esp_idf_svc::hal::gpio::{AnyIOPin, PinDriver, Pull};
use esp_idf_svc::hal::modem::Modem;
use esp_idf_svc::http::client::{Configuration as HttpConfig, EspHttpConnection};
use esp_idf_svc::http::Method;
use esp_idf_svc::nvs::{EspDefaultNvsPartition, EspNvs};
use esp_idf_svc::sntp::{EspSntp, SyncStatus};
use esp_idf_svc::wifi::{AuthMethod, BlockingWifi, ClientConfiguration, Configuration, EspWifi};
use putting_core::update::{battery_allows_update, decide, parse_manifest, Decision, ManifestError};
use u8g2_fonts::types::{FontColor, HorizontalAlignment, VerticalPosition};
use u8g2_fonts::{fonts, FontRenderer};

use crate::co5300::Co5300;
use crate::gfx::{self, Strip};

/// Where the watch looks for updates. Must match where the release workflow publishes.
const SITE_URL: &str = match option_env!("OTA_SITE_URL") {
    Some(u) => u,
    None => "https://mitchbradley.github.io/PuttingWatch/",
};
const RUNNING_VERSION: &str = env!("CARGO_PKG_VERSION");

// --- the RTC flag that selects update mode on the next boot --------------------------------------

const MAGIC: u32 = 0x5550_4454; // "UPDT"

#[link_section = ".rtc_noinit"]
static mut REQUEST: u32 = 0;

/// Restart into update mode.
pub fn request_and_restart() -> ! {
    unsafe {
        core::ptr::write_volatile(core::ptr::addr_of_mut!(REQUEST), MAGIC);
        esp_idf_svc::sys::esp_restart();
    }
}

/// True if this boot was requested as update mode (one-shot: the flag is cleared). RTC memory
/// holds garbage after a power-up, so the flag only counts after a software reset.
pub fn requested() -> bool {
    unsafe {
        let magic = core::ptr::read_volatile(core::ptr::addr_of!(REQUEST));
        core::ptr::write_volatile(core::ptr::addr_of_mut!(REQUEST), 0);
        magic == MAGIC && esp_idf_svc::sys::esp_reset_reason() == esp_idf_svc::sys::esp_reset_reason_t_ESP_RST_SW
    }
}

// --- the (minimal, for now) status screen --------------------------------------------------------

#[derive(Clone, PartialEq, Default)]
struct View {
    title: String,
    lines: Vec<String>,
}

/// Greedy word wrap to `width` characters.
fn wrap(s: &str, width: usize) -> Vec<String> {
    let mut out = Vec::new();
    for para in s.split('\n') {
        let mut line = String::new();
        for word in para.split_whitespace() {
            if !line.is_empty() && line.len() + 1 + word.len() > width {
                out.push(core::mem::take(&mut line));
            }
            if !line.is_empty() {
                line.push(' ');
            }
            line.push_str(word);
        }
        out.push(line);
    }
    out
}

fn draw_view(t: &mut Strip, v: &View) {
    let _ = t.clear(Rgb565::BLACK);
    let centred = |t: &mut Strip, font: &FontRenderer, s: &str, y: i32, c: Rgb565| {
        let _ = font.render_aligned(s, Point::new(205, y), VerticalPosition::Center, HorizontalAlignment::Center, FontColor::Transparent(c), t);
    };
    centred(t, &FontRenderer::new::<fonts::u8g2_font_logisoso34_tf>(), &v.title, 70, Rgb565::WHITE);
    let body = FontRenderer::new::<fonts::u8g2_font_helvR18_tf>();
    for (i, l) in v.lines.iter().take(10).enumerate() {
        centred(t, &body, l, 150 + 36 * i as i32, Rgb565::CSS_LIGHT_GRAY);
    }
}

fn display_thread(lcd: Co5300, view: Arc<Mutex<View>>) {
    let mut buf = vec![0u8; crate::co5300::WIDTH * gfx::STRIP_LINES * 2];
    let mut drawn: Option<View> = None;
    loop {
        FreeRtos::delay_ms(100);
        let cur = view.lock().unwrap().clone();
        if drawn.as_ref() != Some(&cur) {
            gfx::render(&lcd, &mut buf, |t| draw_view(t, &cur));
            drawn = Some(cur);
        }
    }
}

// --- the engine ----------------------------------------------------------------------------------

fn show(view: &Mutex<View>, title: &str, lines: Vec<String>) {
    log::info!("[update] {title}: {}", lines.join(" | "));
    *view.lock().unwrap() = View { title: title.into(), lines };
}

fn fail(view: &Mutex<View>, msg: &str) {
    show(view, "Problem", wrap(msg, 30));
}

fn load_creds(nvs: &EspDefaultNvsPartition) -> Option<(String, String)> {
    let h = EspNvs::new(nvs.clone(), "wifi", false).ok()?;
    let (mut a, mut b) = ([0u8; 64], [0u8; 80]);
    let ssid = h.get_str("ssid", &mut a).ok()??.to_string();
    let pass = h.get_str("pass", &mut b).ok()?.unwrap_or("").to_string();
    (!ssid.is_empty()).then_some((ssid, pass))
}

fn save_creds(nvs: &EspDefaultNvsPartition, ssid: &str, pass: &str) {
    if let Ok(h) = EspNvs::new(nvs.clone(), "wifi", true) {
        let _ = h.set_str("ssid", ssid);
        let _ = h.set_str("pass", pass);
    }
}

/// Saved credentials, else the ones baked in at build time (WIFI_SSID / WIFI_PASS in the
/// environment of `cargo build`, never committed), which are then saved. Provisioning through the
/// watch's own setup network comes later.
fn credentials(nvs: &EspDefaultNvsPartition) -> Option<(String, String)> {
    if let Some(c) = load_creds(nvs) {
        return Some(c);
    }
    let (ssid, pass) = (option_env!("WIFI_SSID")?, option_env!("WIFI_PASS").unwrap_or(""));
    save_creds(nvs, ssid, pass);
    Some((ssid.to_string(), pass.to_string()))
}

fn connect(view: &Mutex<View>, modem: Modem<'static>, sysloop: EspSystemEventLoop, ssid: &str, pass: &str) -> Result<BlockingWifi<EspWifi<'static>>, String> {
    show(view, "Wi-Fi", vec!["Joining".into(), ssid.to_string()]);
    // No NVS handed to the driver: it would keep its own copy of the credentials.
    let esp = EspWifi::new(modem, sysloop.clone(), None).map_err(|e| format!("Wi-Fi init failed: {e}"))?;
    let mut wifi = BlockingWifi::wrap(esp, sysloop).map_err(|e| format!("Wi-Fi init failed: {e}"))?;
    let cfg = Configuration::Client(ClientConfiguration {
        ssid: ssid.try_into().map_err(|_| "That network name is too long.".to_string())?,
        password: pass.try_into().map_err(|_| "That password is too long.".to_string())?,
        auth_method: if pass.is_empty() { AuthMethod::None } else { AuthMethod::WPA2Personal },
        ..Default::default()
    });
    wifi.set_configuration(&cfg).map_err(|e| format!("Wi-Fi setup failed: {e}"))?;
    wifi.start().map_err(|e| format!("Wi-Fi start failed: {e}"))?;
    wifi.connect().map_err(|e| format!("Couldn't join \"{ssid}\": {e}"))?;
    wifi.wait_netif_up().map_err(|e| format!("Joined \"{ssid}\" but got no address: {e}"))?;
    if let Ok(ip) = wifi.wifi().sta_netif().get_ip_info() {
        log::info!("[update] joined, ip {}", ip.ip);
    }
    Ok(wifi)
}

/// TLS needs a real clock to validate certificates.
fn sync_clock() -> Result<EspSntp<'static>, String> {
    let sntp = EspSntp::new_default().map_err(|e| format!("SNTP failed: {e}"))?;
    for _ in 0..75 {
        let now = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        if sntp.get_sync_status() == SyncStatus::Completed && now > 1_700_000_000 {
            return Ok(sntp);
        }
        FreeRtos::delay_ms(200);
    }
    Err("Couldn't get the time from the internet. Is this Wi-Fi connected to the internet?".into())
}

/// Verified HTTPS GET of a small body (the manifest).
fn https_get(url: &str, max_len: usize) -> Result<Vec<u8>, String> {
    let cfg = HttpConfig {
        crt_bundle_attach: Some(esp_idf_svc::sys::esp_crt_bundle_attach),
        timeout: Some(Duration::from_secs(10)),
        ..Default::default()
    };
    let mut conn = EspHttpConnection::new(&cfg).map_err(|e| format!("HTTP init failed: {e}"))?;
    let ua = format!("HonuWatch-rs/{RUNNING_VERSION}");
    conn.initiate_request(Method::Get, url, &[("User-Agent", ua.as_str())])
        .map_err(|e| format!("Couldn't reach the update server: {e}"))?;
    conn.initiate_response().map_err(|e| format!("Couldn't reach the update server: {e}"))?;
    let status = conn.status();
    if status != 200 {
        return Err(format!("The update server returned error {status}."));
    }
    let mut body = Vec::new();
    let mut chunk = [0u8; 512];
    loop {
        let n = conn.read(&mut chunk).map_err(|e| format!("The download was interrupted: {e}"))?;
        if n == 0 {
            return Ok(body);
        }
        if body.len() + n > max_len {
            return Err("Unexpected reply from the update server.".into());
        }
        body.extend_from_slice(&chunk[..n]);
    }
}

fn engine(view: Arc<Mutex<View>>, modem: Modem<'static>, sysloop: EspSystemEventLoop, nvs: EspDefaultNvsPartition) {
    let Some((ssid, pass)) = credentials(&nvs) else {
        fail(&view, "No Wi-Fi network is saved yet. Setting one up from the watch isn't done yet.");
        return;
    };
    let _wifi = match connect(&view, modem, sysloop, &ssid, &pass) {
        Ok(w) => w,
        Err(e) => return fail(&view, &e),
    };

    show(&view, "Checking", vec!["Setting the clock...".into()]);
    let _sntp = match sync_clock() {
        Ok(s) => s,
        Err(e) => return fail(&view, &e),
    };

    show(&view, "Checking", vec!["Looking for updates...".into()]);
    let url = format!("{SITE_URL}manifest.json");
    log::info!("[update] GET {url}");
    let body = match https_get(&url, 4096) {
        Ok(b) => b,
        Err(e) => return fail(&view, &e),
    };
    let manifest = match parse_manifest(&String::from_utf8_lossy(&body)) {
        Ok(m) => m,
        Err(ManifestError::UnsupportedSchema) => {
            return fail(&view, "This update needs a newer updater. Please install it with a USB cable.")
        }
        Err(e) => {
            log::error!("[update] manifest error: {e:?}");
            return fail(&view, "The update information on the server is unreadable. Please try again later.");
        }
    };
    log::info!("[update] manifest: version {} size {} (running {RUNNING_VERSION})", manifest.version, manifest.size);

    match decide(RUNNING_VERSION, &manifest) {
        Decision::UpToDate => show(&view, "Up to date", vec![format!("v{RUNNING_VERSION}"), format!("latest is v{}", manifest.version)]),
        Decision::Available => {
            let mut lines = vec![format!("v{} is available", manifest.version)];
            lines.extend(wrap(&manifest.notes, 30));
            if !battery_allows_update(-1, true) {
                lines.push("Charge the battery first.".into());
            }
            show(&view, "Update", lines);
        }
        Decision::NeedsUsbInstall => show(&view, "USB update", wrap("A newer version needs a USB cable to install.", 30)),
    }
}

/// Run update mode. Never returns: BOOT restarts into the normal app.
pub fn run(lcd: Co5300, modem: Modem<'static>, boot: AnyIOPin<'static>) -> ! {
    log::info!("UPDATE MODE (site {SITE_URL}, running {RUNNING_VERSION})");
    let view = Arc::new(Mutex::new(View { title: "Update".into(), lines: vec!["Starting...".into()] }));
    {
        let view = view.clone();
        std::thread::Builder::new().stack_size(16 * 1024).spawn(move || display_thread(lcd, view)).unwrap();
    }

    let sysloop = EspSystemEventLoop::take().expect("event loop");
    let nvs = EspDefaultNvsPartition::take().expect("nvs");
    {
        let view = view.clone();
        // 24 KB: the TLS handshake runs on this thread's stack and is the deepest thing it does.
        std::thread::Builder::new()
            .stack_size(24 * 1024)
            .spawn(move || engine(view, modem, sysloop, nvs))
            .unwrap();
    }

    // BOOT cancels: restart into the normal app (the update flag is already cleared).
    let boot = PinDriver::input(boot, Pull::Up).unwrap();
    let mut was_low = boot.is_low();
    loop {
        let low = boot.is_low();
        if low && !was_low {
            log::info!("BOOT pressed: leaving update mode");
            unsafe { esp_idf_svc::sys::esp_restart() };
        }
        was_low = low;
        FreeRtos::delay_ms(20);
    }
}
