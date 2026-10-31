//! Update mode: a separate boot that runs only the updater (no IMU, tracking or audio), so
//! Wi-Fi and TLS have the RAM to themselves. The Bye screen's Update button sets a flag in RTC
//! memory and restarts; the next boot sees the flag plus a software-reset reason and comes here.
//! BOOT cancels, restarting into the normal app.
//!
//! Flow: join Wi-Fi from saved credentials -> NTP (TLS needs a sane clock) -> fetch and verify
//! manifest.json over HTTPS -> decide (up to date / available / needs a USB install) -> on the
//! Install button, stream the image into the inactive OTA slot, checking its SHA-256 against the
//! manifest, then switch the boot slot and restart. The running firmware is only replaced when the
//! very last step (`complete`) succeeds, so cancelling at any moment by restarting is safe.

use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU16, Ordering};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use esp_idf_svc::eventloop::EspSystemEventLoop;
use esp_idf_svc::hal::delay::FreeRtos;
use esp_idf_svc::hal::gpio::{AnyIOPin, AnyOutputPin, PinDriver, Pull};
use esp_idf_svc::hal::i2c::I2cDriver;
use esp_idf_svc::hal::modem::Modem;
use esp_idf_svc::hal::task::thread::ThreadSpawnConfiguration;
use esp_idf_svc::http::client::{Configuration as HttpConfig, EspHttpConnection};
use esp_idf_svc::http::Method;
use esp_idf_svc::nvs::{EspDefaultNvsPartition, EspNvs};
use esp_idf_svc::ota::EspOta;
use esp_idf_svc::sntp::EspSntp;
use esp_idf_svc::wifi::{AuthMethod, BlockingWifi, ClientConfiguration, Configuration, EspWifi, WifiEvent};
use putting_core::update::{battery_allows_update, decide, parse_manifest, Decision, Manifest, ManifestError};
use putting_core::update_view::{Phase, Status};
use sha2::{Digest, Sha256};

use crate::co5300::Co5300;
use crate::provision;
use crate::update_ui::{self, Hit, Shared};

/// Where the watch looks for updates. Must match where the release workflow publishes.
pub const SITE_URL: &str = match option_env!("OTA_SITE_URL") {
    Some(u) => u,
    None => "https://mitchbradley.github.io/PuttingWatch/",
};
const RUNNING_VERSION: &str = env!("CARGO_PKG_VERSION");

const HTTP_TIMEOUT: Duration = Duration::from_secs(10);
const JOIN_TIMEOUT: Duration = Duration::from_secs(20);
const STALL_LIMIT: Duration = Duration::from_secs(15); // no download bytes for this long = stalled
const TOUCH_RELEASE_MS: u32 = 120;

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

// --- commands from the screen's controls ----------------------------------------------------------

/// What an on-screen control asks the engine to do (the others just restart into the normal app).
#[derive(Clone, Copy, PartialEq, Debug)]
enum Cmd {
    Install,
    Retry,
    ChangeWifi,
}

// --- the engine ----------------------------------------------------------------------------------

/// Battery state, fed by the main thread (which owns the I2C bus).
struct Power {
    pct: AtomicI32,
    external: AtomicBool,
}

fn set(shared: &Shared, f: impl FnOnce(&mut Status)) {
    f(&mut shared.status.lock().unwrap());
}

fn phase(shared: &Shared, p: Phase) {
    log::info!("[update] phase {p:?}");
    set(shared, |s| s.phase = p);
}

fn fail(shared: &Shared, msg: &str) {
    log::warn!("[update] error: {msg}");
    set(shared, |s| {
        s.phase = Phase::Error;
        s.error = msg.to_string();
    });
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

type Wifi = BlockingWifi<EspWifi<'static>>;

/// Why a join failed, in the words shown on the setup screen.
fn describe_join_failure(reason: u16, ssid: &str) -> String {
    match reason {
        // NO_AP_FOUND (and its compatible-security variants)
        201 | 210..=213 => format!("Couldn't find \"{ssid}\"."),
        // AUTH_EXPIRE, 4WAY_HANDSHAKE_TIMEOUT, MIC_FAILURE, AUTH_FAIL, HANDSHAKE_TIMEOUT, ...
        2 | 14 | 15 | 16 | 202 | 204 | 205 => format!("Couldn't join \"{ssid}\". Wrong password?"),
        _ => format!("Couldn't connect to \"{ssid}\"."),
    }
}

/// Join a network: one attempt with a bounded wait, classifying the failure from the driver's
/// disconnect reason (`why`, filled in by an event subscription).
fn join(shared: &Shared, wifi: &mut Wifi, why: &AtomicU16, ssid: &str, pass: &str) -> Result<(), String> {
    log::info!("[update] joining '{ssid}'");
    set(shared, |s| {
        s.phase = Phase::Connecting;
        s.ssid = ssid.to_string();
        s.error.clear();
    });
    let cfg = Configuration::Client(ClientConfiguration {
        ssid: ssid.try_into().map_err(|_| "That network name is too long.".to_string())?,
        password: pass.try_into().map_err(|_| "That password is too long.".to_string())?,
        auth_method: if pass.is_empty() { AuthMethod::None } else { AuthMethod::WPA2Personal },
        ..Default::default()
    });
    let _ = wifi.wifi_mut().disconnect();
    let _ = wifi.stop();
    wifi.set_configuration(&cfg).map_err(|e| format!("Wi-Fi setup failed: {e}"))?;
    wifi.start().map_err(|e| format!("Wi-Fi start failed: {e}"))?;
    why.store(0, Ordering::Relaxed);
    wifi.wifi_mut().connect().map_err(|e| format!("Couldn't join \"{ssid}\": {e}"))?;

    let t0 = Instant::now();
    loop {
        if wifi.is_connected().unwrap_or(false) {
            break;
        }
        let reason = why.load(Ordering::Relaxed);
        if reason != 0 {
            log::warn!("[update] join failed, disconnect reason {reason}");
            let _ = wifi.wifi_mut().disconnect();
            return Err(describe_join_failure(reason, ssid));
        }
        if t0.elapsed() > JOIN_TIMEOUT {
            let _ = wifi.wifi_mut().disconnect();
            return Err(format!("Couldn't connect to \"{ssid}\"."));
        }
        FreeRtos::delay_ms(100);
    }
    wifi.wait_netif_up().map_err(|e| format!("Joined \"{ssid}\" but got no address: {e}"))?;
    if let Ok(ip) = wifi.wifi().sta_netif().get_ip_info() {
        log::info!("[update] joined, ip {}", ip.ip);
    }
    Ok(())
}

/// Any real clock reads later than this (Nov 2023).
const CLOCK_SANE: u64 = 1_700_000_000;

fn clock_is_sane() -> bool {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) >= CLOCK_SANE
}

/// TLS needs a real clock to validate certificates.
///
/// Judged by the clock itself, never by the SNTP sync status: `sntp_get_sync_status()` reports
/// Completed only once and then resets, so after a first successful check (or any later pass
/// through here, e.g. after changing Wi-Fi) the status would never say Completed again even though
/// the time is right.
fn sync_clock(sntp: &mut Option<EspSntp<'static>>) -> Result<(), String> {
    if clock_is_sane() {
        return Ok(());
    }
    if sntp.is_none() {
        *sntp = Some(EspSntp::new_default().map_err(|e| format!("SNTP failed: {e}"))?);
    }
    for _ in 0..75 {
        if clock_is_sane() {
            return Ok(());
        }
        FreeRtos::delay_ms(200);
    }
    // Start over next time: the client may have been started before the network was usable.
    *sntp = None;
    Err("Couldn't get the time from the internet. Is this Wi-Fi connected to the internet?".into())
}

/// Start a verified HTTPS GET and return the open connection with the response headers read.
fn https_get(url: &str) -> Result<EspHttpConnection, String> {
    let cfg = HttpConfig {
        crt_bundle_attach: Some(esp_idf_svc::sys::esp_crt_bundle_attach),
        timeout: Some(HTTP_TIMEOUT),
        ..Default::default()
    };
    let mut conn = EspHttpConnection::new(&cfg).map_err(|e| format!("HTTP init failed: {e}"))?;
    let ua = format!("HonuWatch-rs/{RUNNING_VERSION}");
    conn.initiate_request(Method::Get, url, &[("User-Agent", ua.as_str())])
        .map_err(|e| format!("Couldn't reach the update server: {e}"))?;
    conn.initiate_response().map_err(|e| format!("Couldn't reach the update server: {e}"))?;
    if conn.status() != 200 {
        return Err(format!("The update server returned error {}.", conn.status()));
    }
    Ok(conn)
}

/// Fetch and validate manifest.json, and decide what to do about it.
fn check(shared: &Shared) -> Result<(Decision, Manifest), String> {
    phase(shared, Phase::Checking);
    let url = format!("{SITE_URL}manifest.json");
    log::info!("[update] GET {url}");
    let mut conn = https_get(&url)?;
    let mut body = Vec::new();
    let mut chunk = [0u8; 512];
    loop {
        let n = conn.read(&mut chunk).map_err(|e| format!("The download was interrupted: {e}"))?;
        if n == 0 {
            break;
        }
        if body.len() + n > 4096 {
            return Err("Unexpected reply from the update server.".into());
        }
        body.extend_from_slice(&chunk[..n]);
    }
    let manifest = match parse_manifest(&String::from_utf8_lossy(&body)) {
        Ok(m) => m,
        Err(ManifestError::UnsupportedSchema) => {
            return Err("This update needs a newer updater. Please install it with a USB cable.".into())
        }
        Err(e) => {
            log::error!("[update] manifest error: {e:?}");
            return Err("The update information on the server is unreadable. Please try again later.".into());
        }
    };
    log::info!("[update] manifest: version {} size {} (running {RUNNING_VERSION})", manifest.version, manifest.size);
    Ok((decide(RUNNING_VERSION, &manifest), manifest))
}

/// Stream the image into the inactive OTA slot, verify, switch the boot slot and restart.
/// Returns only on failure.
fn download(shared: &Shared, power: &Power, m: &Manifest) -> Result<(), String> {
    if !battery_allows_update(power.pct.load(Ordering::Relaxed), power.external.load(Ordering::Relaxed)) {
        return Err("The battery is too low to update safely. Plug in a charger and try again.".into());
    }
    set(shared, |s| {
        s.phase = Phase::Downloading;
        s.progress_pct = 0;
    });
    log::info!("[update] downloading {} ({} bytes)", m.url, m.size);

    let mut conn = https_get(&m.url)?;
    if conn.header("Content-Length").and_then(|v| v.trim().parse::<u32>().ok()) != Some(m.size) {
        return Err("The update file isn't the size the server promised.".into());
    }

    let mut ota = EspOta::new().map_err(|e| format!("OTA init failed: {e}"))?;
    let mut update = ota
        .initiate_update_with_known_size(m.size as usize)
        .map_err(|e| format!("Couldn't prepare the update slot: {e}"))?;

    let mut sha = Sha256::new();
    let mut buf = vec![0u8; 4096];
    let (mut written, mut last_pct, mut last_data) = (0u32, 0u32, Instant::now());
    let failure = loop {
        if written >= m.size {
            break None;
        }
        match conn.read(&mut buf) {
            Ok(0) => break Some("The download was interrupted.".to_string()),
            Ok(n) => {
                let n = n.min((m.size - written) as usize);
                if let Err(e) = update.write(&buf[..n]) {
                    break Some(format!("Couldn't write the update: {e}"));
                }
                sha.update(&buf[..n]);
                written += n as u32;
                last_data = Instant::now();
                let pct = (written as u64 * 100 / m.size as u64) as u32;
                if pct != last_pct {
                    last_pct = pct;
                    set(shared, |s| s.progress_pct = pct as u8);
                }
            }
            Err(e) if last_data.elapsed() > STALL_LIMIT => {
                break Some(format!("The download stalled. Check the Wi-Fi signal. ({e})"))
            }
            Err(e) => break Some(format!("The download was interrupted: {e}")),
        }
    };
    drop(conn);
    if let Some(msg) = failure {
        let _ = update.abort();
        return Err(msg);
    }

    let digest = format!("{:x}", sha.finalize());
    if !digest.eq_ignore_ascii_case(&m.sha256) {
        log::error!("[update] sha256 mismatch: got {digest}, manifest says {}", m.sha256);
        let _ = update.abort();
        return Err("The downloaded update was corrupted. Please try again.".into());
    }
    // Checks the image's own header, then selects it for the next boot.
    update.complete().map_err(|e| format!("The update didn't install: {e}"))?;

    phase(shared, Phase::Installed);
    log::info!("[update] installed {} - restarting", m.version);
    FreeRtos::delay_ms(3000);
    unsafe { esp_idf_svc::sys::esp_restart() };
}

/// Block until one of `want` is pressed; returns which.
fn wait_for(rx: &Receiver<Cmd>, want: &[Cmd]) -> Cmd {
    while let Ok(c) = rx.recv() {
        if want.contains(&c) {
            return c;
        }
    }
    loop {
        FreeRtos::delay_ms(1000);
    }
}

#[derive(Clone, Copy)]
enum Next {
    Connect,
    Provision,
    Check,
}

/// Where a "Try again" / "Change Wi-Fi" pair of controls leads.
fn retry_or_change(rx: &Receiver<Cmd>, note: &mut String) -> Next {
    match wait_for(rx, &[Cmd::Retry, Cmd::ChangeWifi]) {
        Cmd::ChangeWifi => {
            note.clear();
            Next::Provision
        }
        _ => Next::Connect,
    }
}

fn engine(shared: Arc<Shared>, power: Arc<Power>, rx: Receiver<Cmd>, modem: Modem<'static>, sysloop: EspSystemEventLoop, nvs: EspDefaultNvsPartition) {
    // No NVS handed to the driver: it would keep its own copy of the credentials.
    let esp = match EspWifi::new(modem, sysloop.clone(), None) {
        Ok(e) => e,
        Err(e) => return fail(&shared, &format!("Wi-Fi init failed: {e}")),
    };
    // The driver's disconnect reasons tell "wrong password" from "no such network".
    let why = Arc::new(AtomicU16::new(0));
    let _subscription = {
        let why = why.clone();
        sysloop.subscribe::<WifiEvent, _>(move |e| {
            if let WifiEvent::StaDisconnected(d) = e {
                why.store(d.reason(), Ordering::Relaxed);
            }
        })
    };
    let mut wifi = match BlockingWifi::wrap(esp, sysloop) {
        Ok(w) => w,
        Err(e) => return fail(&shared, &format!("Wi-Fi init failed: {e}")),
    };
    let mut sntp: Option<EspSntp<'static>> = None;
    let mut note = String::new(); // why the last join failed, shown on the setup screen
    let mut next = Next::Connect;

    loop {
        next = match next {
            Next::Connect => match credentials(&nvs) {
                None => {
                    note.clear();
                    Next::Provision
                }
                Some((ssid, pass)) => match join(&shared, &mut wifi, &why, &ssid, &pass) {
                    Ok(()) => Next::Check,
                    Err(e) => {
                        note = e;
                        Next::Provision
                    }
                },
            },

            Next::Provision => {
                let result = provision::run(&mut wifi, |st| match st {
                    provision::Status::Scanning => phase(&shared, Phase::Scanning),
                    provision::Status::Waiting { ap_ssid, ap_pass, ip, phone_joined } => set(&shared, |s| {
                        s.phase = Phase::Provisioning;
                        s.ap_ssid = ap_ssid.to_string();
                        s.ap_pass = ap_pass.to_string();
                        s.ap_ip = ip.to_string();
                        s.portal_client = phone_joined;
                        s.error = note.clone();
                    }),
                });
                match result {
                    Ok((ssid, pass)) => match join(&shared, &mut wifi, &why, &ssid, &pass) {
                        Ok(()) => {
                            save_creds(&nvs, &ssid, &pass);
                            note.clear();
                            Next::Check
                        }
                        Err(e) => {
                            note = e; // back to a fresh setup screen, with the reason shown
                            Next::Provision
                        }
                    },
                    Err(e) => {
                        fail(&shared, &e);
                        retry_or_change(&rx, &mut note)
                    }
                }
            }

            Next::Check => {
                phase(&shared, Phase::Checking);
                match sync_clock(&mut sntp).and_then(|_| check(&shared)) {
                    Ok((Decision::UpToDate, _)) => {
                        phase(&shared, Phase::UpToDate);
                        // "Done" restarts into the normal app; only Change Wi-Fi comes back here.
                        wait_for(&rx, &[Cmd::ChangeWifi]);
                        note.clear();
                        Next::Provision
                    }
                    Ok((Decision::NeedsUsbInstall, m)) => {
                        set(&shared, |s| {
                            s.phase = Phase::NeedsUsbInstall;
                            s.version = m.version.clone();
                            s.notes = m.notes.clone();
                        });
                        wait_for(&rx, &[Cmd::ChangeWifi]);
                        note.clear();
                        Next::Provision
                    }
                    Ok((Decision::Available, m)) => {
                        set(&shared, |s| {
                            s.phase = Phase::Available;
                            s.version = m.version.clone();
                            s.notes = m.notes.clone();
                        });
                        match wait_for(&rx, &[Cmd::Install, Cmd::ChangeWifi]) {
                            Cmd::Install => match download(&shared, &power, &m) {
                                Ok(()) => Next::Check,
                                Err(e) => {
                                    fail(&shared, &e);
                                    retry_or_change(&rx, &mut note)
                                }
                            },
                            _ => {
                                note.clear();
                                Next::Provision
                            }
                        }
                    }
                    Err(e) => {
                        fail(&shared, &e);
                        retry_or_change(&rx, &mut note)
                    }
                }
            }
        };
    }
}

/// Restart into the normal app (the update flag is already cleared, so the next boot is normal).
fn restart_to_normal() -> ! {
    FreeRtos::delay_ms(50);
    unsafe { esp_idf_svc::sys::esp_restart() }
}

/// Left alone on a result screen this long: go back to the normal app instead of sitting there
/// with the screen and the radio on.
const IDLE_RESTART: Duration = Duration::from_secs(5 * 60);

/// Run update mode. Never returns: leaving it restarts into the normal app.
pub fn run(
    lcd: Co5300,
    mut i2c: I2cDriver<'static>,
    modem: Modem<'static>,
    boot: AnyIOPin<'static>,
    touch_rst: AnyOutputPin<'static>,
    touch_int: AnyIOPin<'static>,
) -> ! {
    log::info!("UPDATE MODE (site {SITE_URL}, running {RUNNING_VERSION})");
    let shared = Shared::new();
    shared.status.lock().unwrap().phase = Phase::Starting;

    // A repaint is tens of ms of CPU; keep the display thread below the engine so it can't slow the
    // download.
    let _ = ThreadSpawnConfiguration { name: Some(c"display"), stack_size: 16 * 1024, priority: 2, ..Default::default() }.set();
    {
        let shared = shared.clone();
        std::thread::Builder::new()
            .stack_size(16 * 1024)
            .spawn(move || update_ui::display_thread(lcd, shared, RUNNING_VERSION, SITE_URL))
            .unwrap();
    }
    let _ = ThreadSpawnConfiguration::default().set();

    let power = Arc::new(Power { pct: AtomicI32::new(-1), external: AtomicBool::new(false) });
    let (tx, rx): (Sender<Cmd>, Receiver<Cmd>) = channel();
    let sysloop = EspSystemEventLoop::take().expect("event loop");
    let nvs = EspDefaultNvsPartition::take().expect("nvs");
    {
        let (shared, power) = (shared.clone(), power.clone());
        // 24 KB: the TLS handshake runs on this thread's stack and is the deepest thing it does.
        std::thread::Builder::new()
            .stack_size(24 * 1024)
            .spawn(move || engine(shared, power, rx, modem, sysloop, nvs))
            .unwrap();
    }

    // This thread owns the I2C bus: touch, battery, and the BOOT button.
    let _ = crate::pmu::init(&mut i2c);
    let mut touch = crate::touch::Touch::new(&mut i2c, touch_rst, touch_int);
    let boot = PinDriver::input(boot, Pull::Up).unwrap();
    let mut boot_was_low = boot.is_low();
    let (mut touching, mut last_touch) = (false, Instant::now());
    let mut last_activity = Instant::now();
    let mut next_battery = Instant::now();
    let mut batt_mv_ema = 0i32;
    loop {
        // BOOT cancels, anywhere: restarting is always safe.
        let low = boot.is_low();
        if low && !boot_was_low {
            log::info!("BOOT pressed: leaving update mode");
            restart_to_normal();
        }
        boot_was_low = low;

        if let Some((x, y)) = touch.read(&mut i2c) {
            last_touch = Instant::now();
            last_activity = last_touch;
            if !touching {
                touching = true;
                let view = update_ui::current_view(&shared, RUNNING_VERSION, SITE_URL);
                let phase = shared.status.lock().unwrap().phase;
                if let Some(h) = update_ui::hit(&view, x as i32, y as i32) {
                    shared.set_pressed(Some(h));
                    match (h, phase) {
                        (Hit::Primary, Phase::Available) => {
                            let _ = tx.send(Cmd::Install);
                        }
                        (Hit::Primary, Phase::Error) => {
                            let _ = tx.send(Cmd::Retry);
                        }
                        (Hit::Link, _) => {
                            let _ = tx.send(Cmd::ChangeWifi);
                        }
                        // Done, Cancel, Not now, Close: back to the normal app. Let the pressed look reach the screen.
                        (Hit::Primary, _) | (Hit::Secondary, _) => {
                            FreeRtos::delay_ms(150);
                            restart_to_normal();
                        }
                    }
                }
            }
        }
        if touching && last_touch.elapsed() > Duration::from_millis(TOUCH_RELEASE_MS as u64) {
            touching = false;
            shared.set_pressed(None);
        }

        if Instant::now() >= next_battery {
            next_battery = Instant::now() + Duration::from_secs(2);
            if let Some(mv) = crate::pmu::millivolts(&mut i2c) {
                batt_mv_ema = if batt_mv_ema == 0 { mv } else { batt_mv_ema + (mv - batt_mv_ema) / 3 };
                power.pct.store(crate::pmu::percent_for_millivolts(batt_mv_ema), Ordering::Relaxed);
            }
            power.external.store(crate::pmu::external_power(&mut i2c), Ordering::Relaxed);
            shared.battery_ok.store(
                battery_allows_update(power.pct.load(Ordering::Relaxed), power.external.load(Ordering::Relaxed)),
                Ordering::Relaxed,
            );
        }

        let phase = shared.status.lock().unwrap().phase;
        if matches!(phase, Phase::UpToDate | Phase::Available | Phase::NeedsUsbInstall | Phase::Error)
            && last_activity.elapsed() > IDLE_RESTART
        {
            log::info!("idle on a result screen: leaving update mode");
            restart_to_normal();
        }
        FreeRtos::delay_ms(20);
    }
}
