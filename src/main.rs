mod audio;
mod co5300;
mod gfx;
mod pmu;
mod provision;
mod touch;
mod ui;
mod update_mode;
mod update_ui;
mod voice_index;

use std::sync::atomic::AtomicU8;
use std::sync::{Arc, Mutex};

use esp_idf_svc::hal::delay::{FreeRtos, BLOCK};
use esp_idf_svc::hal::gpio::{PinDriver, Pull};
use esp_idf_svc::hal::i2c::{I2cConfig, I2cDriver};
use esp_idf_svc::hal::peripherals::Peripherals;
use esp_idf_svc::hal::units::Hertz;
use putting_core::{angle_between_degrees, Event, GravityFilter, MotionSample, State, SwingTracker, Vec3};

use ui::{Btn, Panel, Screen, UiState};

// Waveshare ESP32-C6-Touch-AMOLED-2.06 shared I2C bus (see include/pin_config.h on the old-cpp-version branch).
const QMI8658_ADDR: u8 = 0x6B;

/// A touch ends when no report has arrived for this long (the FT3168 only reports while a finger
/// is down; reports repeat every ~10 ms).
const TOUCH_RELEASE_MS: u32 = 120;

/// Power behaviour, as in the C++ app.
const BRIGHT_HOLD_MS: u32 = 5000; // run screen blanks this long after the last touch...
const ANGLE_WAKE_DEG: f32 = 45.0; // ...and this long after a swing angle past this (looking at the watch)
const ANGLE_WAKE_HOLD_MS: u32 = 10_000;
const BYE_MS: u32 = 3000; // "Bye" screen before the PMU cuts power
const WAKE_SWALLOW_MS: u32 = 400; // a wake tap must not also press a button
const DOUBLE_TAP_MS: u32 = 400; // two background taps on the run screen -> light sleep
const BOOT_DEBOUNCE_MS: u32 = 500;
const QR_TIMEOUT_MS: u32 = 60_000; // back to Bye if nobody dismisses a QR page
const TP_INT_GPIO: i32 = 15;
const BOOT_GPIO: i32 = 9;

/// Click sensitivity levels: level 1 = 3.0 deg, +0.9 deg per level, 16 levels (as in the C++ app).
const LEVEL_BASE_DEGREES: f32 = 3.0;
const LEVEL_STEP_DEGREES: f32 = 0.9;
const LEVEL_COUNT: i32 = 16;

fn level_for_degrees(deg: f32) -> i32 {
    ((deg - LEVEL_BASE_DEGREES) / LEVEL_STEP_DEGREES).round() as i32 + 1
}

fn degrees_for_level(level: i32) -> f32 {
    LEVEL_BASE_DEGREES + (level - 1) as f32 * LEVEL_STEP_DEGREES
}

fn name_for(addr: u8) -> &'static str {
    match addr {
        0x34 => "AXP2101 PMU",
        0x38 => "FT3168 touch",
        0x18 | 0x19 => "ES8311 codec",
        0x40 => "ES7210 mic ADC?",
        0x51 => "PCF85063 RTC",
        0x6A | 0x6B => "QMI8658 IMU",
        _ => "?",
    }
}

fn main() {
    esp_idf_svc::sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();

    // Rollback test image: crash before anything can confirm this image as good (see Cargo.toml).
    if cfg!(feature = "crash_on_boot") {
        panic!("crash_on_boot: deliberate crash for the OTA rollback test");
    }

    // The sampling loop (this task) must preempt the display thread: a screen redraw takes
    // tens of ms of CPU on this single core and would otherwise stall IMU sampling.
    unsafe { esp_idf_svc::sys::vTaskPrioritySet(core::ptr::null_mut(), 6) };

    let p = Peripherals::take().unwrap();
    let cfg = I2cConfig::new().baudrate(Hertz(400_000));
    let mut i2c = I2cDriver::new(p.i2c0, p.pins.gpio8, p.pins.gpio7, &cfg).unwrap();

    let lcd = co5300::Co5300::new(p.pins.gpio11.into(), co5300::WIDTH * gfx::STRIP_LINES * 2);

    // Update mode is its own boot: no IMU, tracking or audio, so Wi-Fi and TLS get the RAM.
    if update_mode::requested() {
        update_mode::run(lcd, i2c, p.modem, p.pins.gpio9.into(), p.pins.gpio10.into(), p.pins.gpio15.into());
    }

    FreeRtos::delay_ms(400);
    log::info!("I2C devices (SDA=8 SCL=7):");
    for addr in 0x08u8..0x78 {
        if i2c.write(addr, &[], BLOCK).is_ok() {
            log::info!("  0x{addr:02X}  {}", name_for(addr));
        }
    }

    let mut who = [0u8];
    match i2c.write_read(QMI8658_ADDR, &[0x00], &mut who, BLOCK) {
        Ok(()) => log::info!("QMI8658 WHO_AM_I = 0x{:02X} (expect 0x05)", who[0]),
        Err(e) => {
            log::error!("QMI8658 WHO_AM_I read failed: {e:?}");
            confirm_firmware(false); // reverts to the previous firmware if this image is an unconfirmed OTA
            return;
        }
    }

    // CTRL1 = 0x40: register address auto-increment ON (little-endian data). Without it a burst
    // read returns the same register for every byte. Don't rely on state left by earlier firmware.
    // Then accel +-4 g @ ~250 Hz, gyro +-512 dps @ ~250 Hz, both enabled.
    for (reg, val) in [(0x02u8, 0x40u8), (0x03, 0x15), (0x04, 0x55), (0x08, 0x03)] {
        i2c.write(QMI8658_ADDR, &[reg, val], BLOCK).unwrap();
    }

    if let Err(e) = pmu::init(&mut i2c) {
        log::error!("AXP2101 init failed: {e:?}");
    }

    let audio = audio::Audio::start(
        &mut i2c,
        p.i2s0,
        p.pins.gpio20.into(),
        p.pins.gpio23.into(),
        p.pins.gpio19.into(),
        p.pins.gpio22.into(),
        p.pins.gpio6.into(),
    )
    .expect("audio init");

    let mut touch = touch::Touch::new(&mut i2c, p.pins.gpio10.into(), p.pins.gpio15.into());
    // The IMU, codec and display all came up: this image is healthy.
    confirm_firmware(true);
    // BOOT (GPIO9): idle high, low while pressed. Doubles as the shutdown button.
    let boot = PinDriver::input(p.pins.gpio9, Pull::Up).unwrap();

    let mut filter = GravityFilter::default();
    let mut tracker = SwingTracker::default();

    // Shared with the display thread, which repaints whenever this changes.
    let ui_state = Arc::new(Mutex::new(UiState {
        screen: Screen::Waiting,
        ready: false,
        angle10: 0,
        level: level_for_degrees(tracker.click_threshold_degrees()),
        batt_pct: -1,
        batt_mv: 0,
        pressed: None,
        panel: Panel::On,
    }));
    let panel_applied = Arc::new(AtomicU8::new(Panel::On as u8));
    {
        let (ui_state, applied) = (ui_state.clone(), panel_applied.clone());
        std::thread::Builder::new()
            .stack_size(16 * 1024)
            .spawn(move || ui::display_thread(lcd, ui_state, applied))
            .unwrap();
    }

    let now_ms = || (unsafe { esp_idf_svc::sys::esp_timer_get_time() } / 1000) as u32;
    let mut prev_ms = now_ms();
    let mut tick = 0u32;
    let mut last_state = tracker.state();

    // Power state.
    let mut qr_page: Option<ui::QrPage> = None; // Help / Honu QR page shown from the Bye screen
    let mut qr_since = 0u32;
    let mut bye_until: Option<u32> = None; // Stop/BOOT pressed: show "Bye" until then, then power off
    let mut panel = Panel::On;
    let mut last_tap_ms = now_ms(); // any touch: holds the run screen on
    let mut last_bg_tap_ms = 0u32; // background taps, for double-tap -> light sleep
    let mut last_angle_wake_ms = 0u32;
    let mut angle_over = false;
    let mut ignore_touch_until = 0u32; // swallow touches until then (wake tap, Stop tap)
    let mut want_sleep = false;
    let mut boot_prev = boot.is_low();
    let mut boot_last_trigger = 0u32;

    let (mut touching, mut last_touch_ms, mut pressed) = (false, 0u32, None::<Btn>);
    let (mut batt_mv_ema, mut batt_pct, mut batt_next_ms) = (0i32, -1i32, 0u32);

    // Signed "has `t` passed" for wrapping millisecond timestamps.
    let passed = |now: u32, t: u32| (now.wrapping_sub(t) as i32) >= 0;

    loop {
        let now = now_ms();

        // --- BOOT button: the same graceful shutdown as Stop ------------------------------------
        let boot_low = boot.is_low();
        if boot_low && !boot_prev && now.wrapping_sub(boot_last_trigger) > BOOT_DEBOUNCE_MS {
            boot_last_trigger = now;
            if qr_page.is_some() {
                qr_page = None; // the upper button goes back from a QR page to the Bye screen
            } else if bye_until.is_none() {
                log::info!("BOOT pressed -> shutdown");
                bye_until = Some(now.wrapping_add(BYE_MS));
                ignore_touch_until = now.wrapping_add(500);
                panel = Panel::On;
            }
        }
        boot_prev = boot_low;

        // A QR page stays up until dismissed or for a minute; the power-off countdown restarts
        // when we get back to the Bye screen.
        if qr_page.is_some() {
            if now.wrapping_sub(qr_since) >= QR_TIMEOUT_MS {
                qr_page = None;
            }
            bye_until = Some(now.wrapping_add(BYE_MS));
        }

        // --- power off once the Bye screen has been shown long enough ---------------------------
        if let Some(t) = bye_until {
            if passed(now, t) {
                log::info!("shutdown: PMU power off (press PWR to wake)");
                audio.set_amp(false);
                panel = Panel::Asleep;
                ui_state.lock().unwrap().panel = panel;
                ui::wait_for_panel(&panel_applied, Panel::Asleep, 1500);
                match pmu::shutdown(&mut i2c) {
                    Ok(()) => loop {
                        FreeRtos::delay_ms(1000); // rails drop before this returns
                    },
                    Err(e) => {
                        // Don't strand the user with a dead Stop button: sleep instead.
                        log::error!("PMU shutdown failed ({e:?}); falling back to light sleep");
                        bye_until = None;
                        want_sleep = true;
                    }
                }
            }
        }

        // --- touch: one click at touch-down, pressed look until release -------------------------
        let screen = current_screen(bye_until.is_some(), qr_page, &tracker);
        if let Some((x, y)) = touch.read(&mut i2c) {
            if !passed(now, ignore_touch_until) {
                // swallowed (wake tap / the Stop tap): don't start a touch
            } else if panel != Panel::On {
                // First touch on a dark screen only wakes it.
                panel = Panel::On;
                ignore_touch_until = now.wrapping_add(WAKE_SWALLOW_MS);
                last_tap_ms = now;
            } else {
                last_tap_ms = now;
                last_touch_ms = now;
                if !touching {
                    touching = true;
                    match ui::hit(screen, x as i32, y as i32) {
                        Some(b) => {
                            pressed = Some(b);
                            match b {
                                Btn::Recal => tracker.recal(),
                                Btn::Help | Btn::Honu => {
                                    qr_page = Some(if b == Btn::Help { ui::QrPage::Help } else { ui::QrPage::Honu });
                                    qr_since = now;
                                }
                                Btn::Update => {
                                    // Let "Starting..." reach the screen, then restart into update mode.
                                    ui_state.lock().unwrap().pressed = pressed;
                                    FreeRtos::delay_ms(400);
                                    update_mode::request_and_restart();
                                }
                                Btn::Stop => {
                                    bye_until = Some(now.wrapping_add(BYE_MS));
                                    ignore_touch_until = now.wrapping_add(500);
                                }
                                Btn::Minus | Btn::Plus => {
                                    let delta = if b == Btn::Plus { 1 } else { -1 };
                                    let level = (level_for_degrees(tracker.click_threshold_degrees()) + delta).clamp(1, LEVEL_COUNT);
                                    tracker.set_click_threshold_degrees(degrees_for_level(level));
                                    audio.play(audio::Sound::Number(level as u8));
                                }
                            }
                        }
                        None => {
                            tracker.tap(); // starts calibration; no-op outside the waiting screen
                            if screen == Screen::Run {
                                if now.wrapping_sub(last_bg_tap_ms) < DOUBLE_TAP_MS {
                                    want_sleep = true;
                                }
                                last_bg_tap_ms = now;
                            }
                        }
                    }
                }
            }
        }
        if touching && now.wrapping_sub(last_touch_ms) > TOUCH_RELEASE_MS {
            touching = false;
            pressed = None;
        }

        // --- light sleep (double-tap on the run screen) -----------------------------------------
        if want_sleep {
            want_sleep = false;
            light_sleep(&mut i2c, &mut touch, &audio, &ui_state, &panel_applied);
            // Resume exactly where we left off; the wake tap must not hit a button.
            let t = now_ms();
            panel = Panel::On;
            ignore_touch_until = t.wrapping_add(WAKE_SWALLOW_MS);
            last_tap_ms = t;
            prev_ms = t;
            touching = false;
            pressed = None;
            boot_prev = boot.is_low(); // BOOT woke us: don't also read that as a shutdown press
            continue;
        }

        // --- IMU -> filter -> tracker -------------------------------------------------------------
        // 12 bytes from AX_L (0x35): accel x,y,z then gyro x,y,z, little-endian i16.
        let mut raw = [0u8; 12];
        if i2c.write_read(QMI8658_ADDR, &[0x35], &mut raw, BLOCK).is_ok() {
            let v = |i: usize| i16::from_le_bytes([raw[2 * i], raw[2 * i + 1]]) as f32;
            let accel = Vec3::new(v(0) / 8192.0, v(1) / 8192.0, v(2) / 8192.0);
            let gyro_rps = Vec3::new(v(3), v(4), v(5)) * (1.0 / 64.0 * core::f32::consts::PI / 180.0);

            let dt = now.wrapping_sub(prev_ms) as f32 * 0.001;
            prev_ms = now;
            filter.update(accel, gyro_rps, dt);

            let sample = MotionSample { t_ms: now, gravity: filter.gravity(), rotation_rate: gyro_rps, accel };
            for e in tracker.update(&sample) {
                match e {
                    Event::Click => {
                        audio.play(audio::Sound::Click);
                        log::info!("CLICK #{}", tracker.click_count())
                    }
                    Event::CalibrationComplete => {
                        audio.play(audio::Sound::Beep);
                        log::info!("calibrated")
                    }
                    Event::Ready => {
                        audio.play(audio::Sound::Ready);
                        log::info!("READY")
                    }
                    Event::SwingComplete(p) => {
                        // Speak the peak as a sensitivity level, same scale as the click levels.
                        let n = level_for_degrees(p).clamp(1, audio::MAX_SPOKEN_NUMBER as i32);
                        audio.play(audio::Sound::Number(n as u8));
                        log::info!("swing complete, peak {p:.1} deg")
                    }
                    Event::DirectionLocked(a) => log::info!("direction locked at {a:.1} deg"),
                }
            }
            if tracker.state() != last_state {
                log::info!("state {:?} -> {:?}", last_state, tracker.state());
                last_state = tracker.state();
            }

            if tick % 100 == 0 {
                let g = filter.gravity();
                log::info!(
                    "{:?}/{:?} angle {:.1} deg  from-cal {:.1}  g[{:+.3} {:+.3} {:+.3}] batt {}% {} mV",
                    tracker.state(),
                    tracker.swing_phase(),
                    tracker.swing_angle_degrees(),
                    angle_between_degrees(g, tracker.initial_rest_gravity()),
                    g.x,
                    g.y,
                    g.z,
                    batt_pct,
                    batt_mv_ema
                );
            }
        }

        // --- battery: every 5 s, EMA-smoothed (terminal voltage sags under load) --------------------
        if now.wrapping_sub(batt_next_ms) < u32::MAX / 2 {
            batt_next_ms = now.wrapping_add(5000);
            if let Some(mv) = pmu::millivolts(&mut i2c) {
                batt_mv_ema = if batt_mv_ema == 0 { mv } else { batt_mv_ema + (mv - batt_mv_ema) / 3 };
                batt_pct = pmu::percent_for_millivolts(batt_mv_ema);
            }
        }

        // --- screen blanking on the run screen --------------------------------------------------
        let screen = current_screen(bye_until.is_some(), qr_page, &tracker);
        let angle_now = tracker.swing_angle_degrees();
        let over = screen == Screen::Run && angle_now > ANGLE_WAKE_DEG;
        if over && !angle_over {
            // A big angle isn't putting motion: someone is looking at the watch. Light it up.
            last_angle_wake_ms = now;
            panel = Panel::On;
        }
        angle_over = over;
        if screen != Screen::Run && bye_until.is_none() {
            panel = Panel::On; // only the run screen ever blanks
        } else if panel == Panel::On
            && screen == Screen::Run
            && now.wrapping_sub(last_tap_ms) >= BRIGHT_HOLD_MS
            && now.wrapping_sub(last_angle_wake_ms) >= ANGLE_WAKE_HOLD_MS
        {
            panel = Panel::Blanked;
        }

        // The speaker amp only needs power while sounds can play.
        audio.set_amp(bye_until.is_none() && tracker.state() != State::WaitingForCalibration);

        // --- publish to the display thread ---------------------------------------------------------
        *ui_state.lock().unwrap() = UiState {
            screen,
            ready: tracker.is_ready_for_swing(),
            angle10: if screen == Screen::Run { (angle_now * 10.0).round() as i32 } else { 0 },
            level: level_for_degrees(tracker.click_threshold_degrees()),
            batt_pct,
            batt_mv: batt_mv_ema,
            pressed,
            panel,
        };

        tick = tick.wrapping_add(1);
        FreeRtos::delay_ms(10);
    }
}

fn current_screen(bye: bool, qr: Option<ui::QrPage>, tracker: &SwingTracker) -> Screen {
    if bye {
        return qr.map_or(Screen::Bye, Screen::Qr);
    }
    match tracker.state() {
        State::WaitingForCalibration => Screen::Waiting,
        State::Accumulating => Screen::Hold,
        // Done is a brief pass-through to Monitoring; draw it as Monitoring.
        State::Done | State::Monitoring => Screen::Run,
    }
}

/// Light sleep: panel and amp off, CPU halted; a touch (FT3168 INT low) or BOOT wakes it and
/// everything resumes where it left off (RAM is retained, so the tracker state survives). Note the
/// USB serial console drops while asleep.
fn light_sleep(
    i2c: &mut I2cDriver,
    touch: &mut touch::Touch,
    audio: &audio::Audio,
    ui_state: &Arc<Mutex<UiState>>,
    applied: &Arc<AtomicU8>,
) {
    log::info!("light sleep (touch the screen to wake)");

    // Let the touch that got us here lift and drain the controller so INT deasserts: a
    // level-triggered wake armed while INT is still low returns from sleep immediately.
    for _ in 0..120 {
        if !touch.int_is_low() {
            break;
        }
        let _ = touch.read(i2c);
        FreeRtos::delay_ms(25);
    }

    audio.set_amp(false);
    ui_state.lock().unwrap().panel = Panel::Asleep;
    ui::wait_for_panel(applied, Panel::Asleep, 1500);
    FreeRtos::delay_ms(20);

    unsafe {
        use esp_idf_svc::sys::*;
        esp_sleep_enable_gpio_wakeup();
        gpio_wakeup_enable(TP_INT_GPIO, gpio_int_type_t_GPIO_INTR_LOW_LEVEL);
        gpio_wakeup_enable(BOOT_GPIO, gpio_int_type_t_GPIO_INTR_LOW_LEVEL);
        esp_light_sleep_start();
        gpio_wakeup_disable(TP_INT_GPIO);
        gpio_wakeup_disable(BOOT_GPIO);
    }
    log::info!("woke from light sleep");
    ui_state.lock().unwrap().panel = Panel::On;
}

/// OTA rollback handshake. With rollback enabled in the bootloader, a freshly OTA'd image boots as
/// PENDING_VERIFY; if it never confirms itself, the bootloader reverts to the previous firmware at
/// the next reset. Confirm once the hardware has actually come up (a crash or reset before this
/// point rolls back on its own); `healthy == false` rolls back immediately. A USB-flashed or
/// already-confirmed image has nothing to decide.
fn confirm_firmware(healthy: bool) {
    use esp_idf_svc::sys::*;
    unsafe {
        let running = esp_ota_get_running_partition();
        let mut state: esp_ota_img_states_t = 0;
        if esp_ota_get_state_partition(running, &mut state) != ESP_OK
            || state != esp_ota_img_states_t_ESP_OTA_IMG_PENDING_VERIFY
        {
            return;
        }
        if healthy {
            esp_ota_mark_app_valid_cancel_rollback();
            log::info!("OTA image confirmed valid");
        } else {
            log::error!("OTA image failed its self-check - rolling back");
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }
}
