mod audio;
mod co5300;
mod gfx;
mod pmu;
mod touch;
mod ui;
mod voice_index;

use std::sync::{Arc, Mutex};

use esp_idf_svc::hal::delay::{FreeRtos, BLOCK};
use esp_idf_svc::hal::i2c::{I2cConfig, I2cDriver};
use esp_idf_svc::hal::peripherals::Peripherals;
use esp_idf_svc::hal::units::Hertz;
use putting_core::{angle_between_degrees, Event, GravityFilter, MotionSample, State, SwingTracker, Vec3};

use ui::{Btn, Screen, UiState};

// Waveshare ESP32-C6-Touch-AMOLED-2.06 shared I2C bus (see PuttingWatch/include/pin_config.h).
const QMI8658_ADDR: u8 = 0x6B;

/// A touch ends when no report has arrived for this long (the FT3168 only reports while a finger
/// is down; reports repeat every ~10 ms).
const TOUCH_RELEASE_MS: u32 = 120;

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

    // The sampling loop (this task) must preempt the display thread: a screen redraw takes
    // tens of ms of CPU on this single core and would otherwise stall IMU sampling.
    unsafe { esp_idf_svc::sys::vTaskPrioritySet(core::ptr::null_mut(), 6) };

    let p = Peripherals::take().unwrap();
    let cfg = I2cConfig::new().baudrate(Hertz(400_000));
    let mut i2c = I2cDriver::new(p.i2c0, p.pins.gpio8, p.pins.gpio7, &cfg).unwrap();

    let lcd = co5300::Co5300::new(p.pins.gpio11.into(), co5300::WIDTH * gfx::STRIP_LINES * 2);

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
    }));
    {
        let ui_state = ui_state.clone();
        std::thread::Builder::new()
            .stack_size(16 * 1024)
            .spawn(move || ui::display_thread(lcd, ui_state))
            .unwrap();
    }

    let now_ms = || (unsafe { esp_idf_svc::sys::esp_timer_get_time() } / 1000) as u32;
    let mut prev_ms = now_ms();
    let mut tick = 0u32;
    let mut last_state = tracker.state();

    let mut bye = false; // Stop was pressed
    let (mut touching, mut last_touch_ms, mut pressed) = (false, 0u32, None::<Btn>);
    let (mut batt_mv_ema, mut batt_pct, mut batt_next_ms) = (0i32, -1i32, 0u32);

    loop {
        let now = now_ms();

        // --- touch: one click at touch-down, pressed look until release -------------------------
        let screen = current_screen(bye, &tracker);
        if let Some((x, y)) = touch.read(&mut i2c) {
            last_touch_ms = now;
            if !touching {
                touching = true;
                match ui::hit(screen, x as i32, y as i32) {
                    Some(b) => {
                        pressed = Some(b);
                        match b {
                            Btn::Recal => tracker.recal(),
                            Btn::Stop => bye = true,
                            Btn::Minus | Btn::Plus => {
                                let delta = if b == Btn::Plus { 1 } else { -1 };
                                let level = (level_for_degrees(tracker.click_threshold_degrees()) + delta).clamp(1, LEVEL_COUNT);
                                tracker.set_click_threshold_degrees(degrees_for_level(level));
                                audio.play(audio::Sound::Number(level as u8));
                            }
                        }
                    }
                    None if bye => {
                        // Any touch wakes from "Bye" back to a fresh calibration.
                        bye = false;
                        tracker.recal();
                    }
                    None => tracker.tap(), // starts calibration; no-op outside the waiting screen
                }
            }
        }
        if touching && now.wrapping_sub(last_touch_ms) > TOUCH_RELEASE_MS {
            touching = false;
            pressed = None;
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

        // --- publish to the display thread ---------------------------------------------------------
        let screen = current_screen(bye, &tracker);
        *ui_state.lock().unwrap() = UiState {
            screen,
            ready: tracker.is_ready_for_swing(),
            angle10: if screen == Screen::Run { (tracker.swing_angle_degrees() * 10.0).round() as i32 } else { 0 },
            level: level_for_degrees(tracker.click_threshold_degrees()),
            batt_pct,
            batt_mv: batt_mv_ema,
            pressed,
        };

        tick = tick.wrapping_add(1);
        FreeRtos::delay_ms(10);
    }
}

fn current_screen(bye: bool, tracker: &SwingTracker) -> Screen {
    if bye {
        return Screen::Bye;
    }
    match tracker.state() {
        State::WaitingForCalibration => Screen::Waiting,
        State::Accumulating => Screen::Hold,
        // Done is a brief pass-through to Monitoring; draw it as Monitoring.
        State::Done | State::Monitoring => Screen::Run,
    }
}
