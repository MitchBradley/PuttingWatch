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

use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use embedded_graphics::pixelcolor::Rgb565;
use embedded_graphics::prelude::*;
use embedded_graphics::primitives::{PrimitiveStyle, Rectangle, RoundedRectangle};
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
use esp_idf_svc::sntp::{EspSntp, SyncStatus};
use esp_idf_svc::wifi::{AuthMethod, BlockingWifi, ClientConfiguration, Configuration, EspWifi};
use putting_core::update::{battery_allows_update, decide, parse_manifest, Decision, Manifest, ManifestError};
use sha2::{Digest, Sha256};
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

const HTTP_TIMEOUT: Duration = Duration::from_secs(10);
const STALL_LIMIT: Duration = Duration::from_secs(15); // no download bytes for this long = stalled
const TOUCH_RELEASE_MS: u32 = 120;
const HIT_MARGIN: i32 = 24;

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

/// What a button does.
#[derive(Clone, Copy, PartialEq, Debug)]
enum Cmd {
    Install,
    Retry,
}

#[derive(Clone, PartialEq, Default)]
struct View {
    title: String,
    lines: Vec<String>,
    /// One big button under the text, if any.
    button: Option<(&'static str, Cmd)>,
}

/// (x, y, w, h) of the one button.
const BUTTON: (i32, i32, i32, i32) = (55, 400, 300, 64);

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
    for (i, l) in v.lines.iter().take(7).enumerate() {
        centred(t, &body, l, 150 + 36 * i as i32, Rgb565::CSS_LIGHT_GRAY);
    }
    if let Some((label, _)) = v.button {
        let (x, y, w, h) = BUTTON;
        let _ = RoundedRectangle::with_equal_corners(Rectangle::new(Point::new(x, y), Size::new(w as u32, h as u32)), Size::new(18, 18))
            .into_styled(PrimitiveStyle::with_fill(Rgb565::new(0x21 >> 3, 0x96 >> 2, 0xF3 >> 3)))
            .draw(t);
        centred(t, &FontRenderer::new::<fonts::u8g2_font_logisoso24_tf>(), label, y + h / 2, Rgb565::WHITE);
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

/// Battery state, fed by the main thread (which owns the I2C bus).
struct Power {
    pct: AtomicI32,
    external: AtomicBool,
}

fn show(view: &Mutex<View>, title: &str, lines: Vec<String>, button: Option<(&'static str, Cmd)>) {
    log::info!("[update] {title}: {}", lines.join(" | "));
    *view.lock().unwrap() = View { title: title.into(), lines, button };
}

fn fail(view: &Mutex<View>, msg: &str) {
    show(view, "Problem", wrap(msg, 30), Some(("Try again", Cmd::Retry)));
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

/// Join (or re-join) the network.
fn ensure_connected(view: &Mutex<View>, wifi: &mut Wifi, ssid: &str, pass: &str) -> Result<(), String> {
    if wifi.is_connected().unwrap_or(false) && wifi.is_up().unwrap_or(false) {
        return Ok(());
    }
    show(view, "Wi-Fi", vec!["Joining".into(), ssid.to_string()], None);
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
    Ok(())
}

/// TLS needs a real clock to validate certificates.
fn sync_clock(sntp: &mut Option<EspSntp<'static>>) -> Result<(), String> {
    if sntp.is_none() {
        *sntp = Some(EspSntp::new_default().map_err(|e| format!("SNTP failed: {e}"))?);
    }
    for _ in 0..75 {
        let now = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        if sntp.as_ref().is_some_and(|s| s.get_sync_status() == SyncStatus::Completed) && now > 1_700_000_000 {
            return Ok(());
        }
        FreeRtos::delay_ms(200);
    }
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
fn check(view: &Mutex<View>) -> Result<(Decision, Manifest), String> {
    show(view, "Checking", vec!["Looking for updates...".into()], None);
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
fn download(view: &Mutex<View>, power: &Power, m: &Manifest) -> Result<(), String> {
    if !battery_allows_update(power.pct.load(Ordering::Relaxed), power.external.load(Ordering::Relaxed)) {
        return Err("The battery is too low to update safely. Plug in a charger and try again.".into());
    }
    let label = format!("v{}", m.version);
    show(view, "Downloading", vec![label.clone(), "0%".into()], None);
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
                // Redraws compete with the download for the CPU, so report coarsely.
                let pct = (written as u64 * 100 / m.size as u64) as u32 / 5 * 5;
                if pct != last_pct {
                    last_pct = pct;
                    show(view, "Downloading", vec![label.clone(), format!("{pct}%")], None);
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

    show(view, "Installed", vec![label, "Restarting...".into()], None);
    log::info!("[update] installed {} - restarting", m.version);
    FreeRtos::delay_ms(3000);
    unsafe { esp_idf_svc::sys::esp_restart() };
}

/// Block until the button for `want` is pressed.
fn wait_for(rx: &Receiver<Cmd>, want: Cmd) {
    while let Ok(c) = rx.recv() {
        if c == want {
            return;
        }
    }
    loop {
        FreeRtos::delay_ms(1000);
    }
}

fn engine(view: Arc<Mutex<View>>, power: Arc<Power>, rx: Receiver<Cmd>, modem: Modem<'static>, sysloop: EspSystemEventLoop, nvs: EspDefaultNvsPartition) {
    let Some((ssid, pass)) = credentials(&nvs) else {
        show(&view, "Wi-Fi", wrap("No Wi-Fi network is saved yet. Setting one up from the watch isn't done yet.", 30), None);
        return;
    };
    // No NVS handed to the driver: it would keep its own copy of the credentials.
    let esp = match EspWifi::new(modem, sysloop.clone(), None) {
        Ok(e) => e,
        Err(e) => return fail(&view, &format!("Wi-Fi init failed: {e}")),
    };
    let mut wifi = match BlockingWifi::wrap(esp, sysloop) {
        Ok(w) => w,
        Err(e) => return fail(&view, &format!("Wi-Fi init failed: {e}")),
    };
    let mut sntp: Option<EspSntp<'static>> = None;

    loop {
        let result = ensure_connected(&view, &mut wifi, &ssid, &pass)
            .and_then(|_| {
                show(&view, "Checking", vec!["Setting the clock...".into()], None);
                sync_clock(&mut sntp)
            })
            .and_then(|_| check(&view));

        match result {
            Ok((Decision::UpToDate, _)) => {
                show(&view, "Up to date", vec![format!("v{RUNNING_VERSION}")], Some(("Check again", Cmd::Retry)));
                wait_for(&rx, Cmd::Retry);
            }
            Ok((Decision::NeedsUsbInstall, _)) => {
                show(&view, "USB update", wrap("A newer version needs a USB cable to install.", 30), None);
                return;
            }
            Ok((Decision::Available, m)) => {
                let mut lines = vec![format!("v{} is available", m.version)];
                lines.extend(wrap(&m.notes, 30));
                show(&view, "Update", lines, Some(("Install", Cmd::Install)));
                wait_for(&rx, Cmd::Install);
                match download(&view, &power, &m) {
                    Ok(()) => {}
                    Err(e) => {
                        fail(&view, &e);
                        wait_for(&rx, Cmd::Retry);
                    }
                }
            }
            Err(e) => {
                fail(&view, &e);
                wait_for(&rx, Cmd::Retry);
            }
        }
    }
}

/// Run update mode. Never returns: BOOT restarts into the normal app.
pub fn run(
    lcd: Co5300,
    mut i2c: I2cDriver<'static>,
    modem: Modem<'static>,
    boot: AnyIOPin<'static>,
    touch_rst: AnyOutputPin<'static>,
    touch_int: AnyIOPin<'static>,
) -> ! {
    log::info!("UPDATE MODE (site {SITE_URL}, running {RUNNING_VERSION})");
    let view = Arc::new(Mutex::new(View { title: "Update".into(), lines: vec!["Starting...".into()], button: None }));

    // A redraw takes ~100+ ms of CPU; keep it below the engine so it can't slow the download.
    let _ = ThreadSpawnConfiguration { name: Some(c"display"), stack_size: 16 * 1024, priority: 2, ..Default::default() }.set();
    {
        let view = view.clone();
        std::thread::Builder::new().stack_size(16 * 1024).spawn(move || display_thread(lcd, view)).unwrap();
    }
    let _ = ThreadSpawnConfiguration::default().set();

    let power = Arc::new(Power { pct: AtomicI32::new(-1), external: AtomicBool::new(false) });
    let (tx, rx): (Sender<Cmd>, Receiver<Cmd>) = channel();
    let sysloop = EspSystemEventLoop::take().expect("event loop");
    let nvs = EspDefaultNvsPartition::take().expect("nvs");
    {
        let (view, power) = (view.clone(), power.clone());
        // 24 KB: the TLS handshake runs on this thread's stack and is the deepest thing it does.
        std::thread::Builder::new()
            .stack_size(24 * 1024)
            .spawn(move || engine(view, power, rx, modem, sysloop, nvs))
            .unwrap();
    }

    // This thread owns the I2C bus: touch, battery, and the BOOT button.
    let _ = crate::pmu::init(&mut i2c);
    let mut touch = crate::touch::Touch::new(&mut i2c, touch_rst, touch_int);
    let boot = PinDriver::input(boot, Pull::Up).unwrap();
    let mut boot_was_low = boot.is_low();
    let (mut touching, mut last_touch) = (false, Instant::now());
    let mut next_battery = Instant::now();
    let mut batt_mv_ema = 0i32;
    loop {
        // BOOT cancels: restart into the normal app (the update flag is already cleared).
        let low = boot.is_low();
        if low && !boot_was_low {
            log::info!("BOOT pressed: leaving update mode");
            unsafe { esp_idf_svc::sys::esp_restart() };
        }
        boot_was_low = low;

        if let Some((x, y)) = touch.read(&mut i2c) {
            last_touch = Instant::now();
            if !touching {
                touching = true;
                let button = view.lock().unwrap().button;
                if let Some((_, cmd)) = button {
                    let (bx, by, bw, bh) = BUTTON;
                    let (x, y) = (x as i32, y as i32);
                    if x >= bx - HIT_MARGIN && x < bx + bw + HIT_MARGIN && y >= by - HIT_MARGIN && y < by + bh + HIT_MARGIN {
                        let _ = tx.send(cmd);
                    }
                }
            }
        }
        if touching && last_touch.elapsed() > Duration::from_millis(TOUCH_RELEASE_MS as u64) {
            touching = false;
        }

        if Instant::now() >= next_battery {
            next_battery = Instant::now() + Duration::from_secs(5);
            if let Some(mv) = crate::pmu::millivolts(&mut i2c) {
                batt_mv_ema = if batt_mv_ema == 0 { mv } else { batt_mv_ema + (mv - batt_mv_ema) / 3 };
                power.pct.store(crate::pmu::percent_for_millivolts(batt_mv_ema), Ordering::Relaxed);
            }
            power.external.store(crate::pmu::external_power(&mut i2c), Ordering::Relaxed);
        }
        FreeRtos::delay_ms(20);
    }
}
