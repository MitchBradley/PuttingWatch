mod co5300;
mod gfx;
mod touch;

use esp_idf_svc::hal::delay::{FreeRtos, BLOCK};
use esp_idf_svc::hal::i2c::{I2cConfig, I2cDriver};
use esp_idf_svc::hal::peripherals::Peripherals;
use esp_idf_svc::hal::units::Hertz;

// Waveshare ESP32-C6-Touch-AMOLED-2.06 shared I2C bus (see PuttingWatch/include/pin_config.h).
const QMI8658_ADDR: u8 = 0x6B;

fn name_for(addr: u8) -> &'static str {
    match addr {
        0x34 => "AXP2101 PMU",
        0x38 => "FT3168 touch",
        0x18 | 0x19 => "ES8311 codec / ES7210 mic ADC",
        0x51 => "PCF85063 RTC",
        0x6A | 0x6B => "QMI8658 IMU",
        _ => "?",
    }
}

use embedded_graphics::pixelcolor::Rgb565;
use embedded_graphics::prelude::*;
use embedded_graphics::primitives::{PrimitiveStyle, PrimitiveStyleBuilder, Rectangle, RoundedRectangle};
use u8g2_fonts::types::{FontColor, HorizontalAlignment, VerticalPosition};
use u8g2_fonts::{fonts, FontRenderer};

fn text_aligned<F: u8g2_fonts::Font>(t: &mut gfx::Strip, s: &str, x: i32, y: i32, a: HorizontalAlignment, c: Rgb565) {
    let _ = FontRenderer::new::<F>().render_aligned(s, Point::new(x, y), VerticalPosition::Baseline, a, FontColor::Transparent(c), t);
}

fn text<F: u8g2_fonts::Font>(t: &mut gfx::Strip, s: &str, x: i32, y: i32, c: Rgb565) {
    text_aligned::<F>(t, s, x, y, HorizontalAlignment::Center, c);
}

fn button(t: &mut gfx::Strip, r: Rectangle, label: &str, c: Rgb565) {
    let _ = RoundedRectangle::with_equal_corners(r, Size::new(14, 14))
        .into_styled(PrimitiveStyle::with_fill(c))
        .draw(t);
    text::<fonts::u8g2_font_logisoso22_tf>(t, label, r.center().x, r.center().y + 11, Rgb565::WHITE);
}

/// Roughly the app's main screen, to compare against the C++/MiniUi look.
fn scene(t: &mut gfx::Strip) {
    let blue = Rgb565::new(0x21 >> 3, 0x96 >> 2, 0xF3 >> 3);
    let grey = Rgb565::new(0x3A >> 3, 0x3A >> 2, 0x3A >> 3);
    let _ = t.clear(Rgb565::BLACK);

    text::<fonts::u8g2_font_logisoso34_tf>(t, "Honu Watch", 205, 90, Rgb565::WHITE);
    text::<fonts::u8g2_font_helvR14_tf>(t, "Tap when in putting position", 205, 130, Rgb565::CSS_LIGHT_GRAY);
    button(t, Rectangle::new(Point::new(30, 370), Size::new(170, 60)), "Recal", blue);
    button(t, Rectangle::new(Point::new(210, 370), Size::new(170, 60)), "Stop", blue);
    button(t, Rectangle::new(Point::new(30, 440), Size::new(60, 50)), "-", grey);
    button(t, Rectangle::new(Point::new(320, 440), Size::new(60, 50)), "+", grey);
    let _ = Rectangle::new(Point::new(100, 455), Size::new(210, 20))
        .into_styled(PrimitiveStyle::with_stroke(Rgb565::WHITE, 2))
        .draw(t);
    let _ = Rectangle::new(Point::new(102, 457), Size::new(130, 16))
        .into_styled(PrimitiveStyle::with_fill(Rgb565::CSS_LIME_GREEN))
        .draw(t);
    text::<fonts::u8g2_font_helvR12_tf>(t, "v0.1.5  87%", 205, 25, Rgb565::CSS_GRAY);
}

/// Diagnostic switch: set false to run without the display thread.
const USE_DISPLAY_THREAD: bool = true;

const STATUS_Y0: usize = 150;
const STATUS_Y1: usize = 360;

/// Snapshot of the tracker for the display thread.
#[derive(Clone, Copy, PartialEq)]
struct Status {
    state: putting_core::State,
    phase: putting_core::SwingPhase,
    clicks: i32,
    last_peak: Option<f32>,
    angle: f32,    // from the current rest
    from_cal: f32, // from the original calibration position
    z: f32,        // filtered gravity z (device frame); autocal wants it below 0.25
    still_ms: Option<u32>, // autocal still-window progress while waiting
}

/// Redraws the status block ~10x/s whenever the (rounded) values change. Runs on its own thread
/// because a redraw takes ~100 ms and the sampling loop must not stall.
fn display_thread(lcd: co5300::Co5300, status: std::sync::Arc<std::sync::Mutex<Status>>) {
    use putting_core::State;
    let mut buf = vec![0u8; co5300::WIDTH * gfx::STRIP_LINES * 2];
    let mut shown: Option<Vec<String>> = None;
    loop {
        FreeRtos::delay_ms(100);
        let s = *status.lock().unwrap();
        let title = match s.state {
            State::WaitingForCalibration => "Waiting".to_string(),
            State::Accumulating => "Calibrating...".to_string(),
            State::Done => "Calibrated".to_string(),
            State::Monitoring => format!("{:?}", s.phase),
        };
        // Two big rows: label (left, fixed) | value (right-aligned at a fixed x) | hint (left, fixed).
        // `ok` turns the hint green when the condition it describes is met.
        struct Row {
            label: &'static str,
            value: String,
            hint: String,
            ok: bool,
        }
        let rows: Vec<Row> = match s.state {
            State::WaitingForCalibration => vec![
                Row { label: "z", value: format!("{:+.2}", s.z), hint: "need < 0.25".into(), ok: s.z < 0.25 },
                Row {
                    label: "still",
                    value: s.still_ms.map_or("--".into(), |ms| format!("{:.1}", ms as f32 / 1000.0)),
                    hint: "of 3.0 s".into(),
                    ok: s.still_ms.is_some(),
                },
            ],
            State::Monitoring => vec![
                Row { label: "angle", value: format!("{:.1}", s.angle), hint: "deg".into(), ok: true },
                Row { label: "from cal", value: format!("{:.1}", s.from_cal), hint: "need < 10".into(), ok: s.from_cal < 10.0 },
            ],
            _ => vec![],
        };
        let footer = match (s.state, s.last_peak) {
            (State::WaitingForCalibration, _) => "hold still, or touch to calibrate".to_string(),
            (State::Monitoring, Some(p)) => format!("clicks {}     last peak {:.0} deg", s.clicks, p),
            (State::Monitoring, None) => format!("clicks {}", s.clicks),
            _ => "hold still".to_string(),
        };

        // Redraw only if some visible text changed.
        let key: Vec<String> = [title.clone(), footer.clone()]
            .into_iter()
            .chain(rows.iter().flat_map(|r| [r.value.clone(), r.ok.to_string()]))
            .collect();
        if shown.as_ref() == Some(&key) {
            continue;
        }
        shown = Some(key);

        gfx::render_rows(&lcd, &mut buf, STATUS_Y0, STATUS_Y1, |t| {
            let _ = Rectangle::new(Point::new(0, STATUS_Y0 as i32), Size::new(410, (STATUS_Y1 - STATUS_Y0) as u32))
                .into_styled(PrimitiveStyle::with_fill(Rgb565::BLACK))
                .draw(t);
            text::<fonts::u8g2_font_logisoso24_tf>(t, &title, 205, 185, Rgb565::WHITE);
            for (i, r) in rows.iter().enumerate() {
                let y = 245 + 60 * i as i32;
                let hint_c = if r.ok { Rgb565::CSS_LIME_GREEN } else { Rgb565::CSS_LIGHT_GRAY };
                text_aligned::<fonts::u8g2_font_helvR18_tf>(t, r.label, 20, y, HorizontalAlignment::Left, Rgb565::CSS_LIGHT_GRAY);
                text_aligned::<fonts::u8g2_font_logisoso38_tf>(t, &r.value, 255, y, HorizontalAlignment::Right, Rgb565::WHITE);
                text_aligned::<fonts::u8g2_font_helvR18_tf>(t, &r.hint, 270, y, HorizontalAlignment::Left, hint_c);
            }
            text::<fonts::u8g2_font_helvR14_tf>(t, &footer, 205, 345, Rgb565::CSS_LIGHT_GRAY);
        });
    }
}

fn draw_screen(rst: esp_idf_svc::hal::gpio::AnyOutputPin) -> co5300::Co5300 {
    let mut buf = vec![0u8; co5300::WIDTH * gfx::STRIP_LINES * 2];
    let lcd = co5300::Co5300::new(rst, buf.len());
    let t0 = std::time::Instant::now();
    gfx::render(&lcd, &mut buf, scene);
    log::info!("full-screen render: {} ms", t0.elapsed().as_millis());
    lcd
}

fn main() {
    esp_idf_svc::sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();

    // The sampling loop (this task) must preempt the display thread: a status redraw takes
    // ~100 ms of CPU on this single core and would otherwise stall IMU sampling.
    unsafe { esp_idf_svc::sys::vTaskPrioritySet(core::ptr::null_mut(), 6) };

    let p = Peripherals::take().unwrap();
    let cfg = I2cConfig::new().baudrate(Hertz(400_000));
    let mut i2c = I2cDriver::new(p.i2c0, p.pins.gpio8, p.pins.gpio7, &cfg).unwrap();

    let lcd = draw_screen(p.pins.gpio11.into());

    FreeRtos::delay_ms(400);
    log::info!("===== I2C scan (SDA=8 SCL=7) =====");
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

    let mut touch = touch::Touch::new(&mut i2c, p.pins.gpio10.into(), p.pins.gpio15.into());
    log::info!("touch ready; touch the screen");

    use putting_core::{angle_between_degrees, Event, GravityFilter, MotionSample, State, SwingTracker, Vec3};
    let status = std::sync::Arc::new(std::sync::Mutex::new(Status {
        state: State::WaitingForCalibration,
        phase: putting_core::SwingPhase::Backswing,
        clicks: 0,
        last_peak: None,
        angle: 0.0,
        from_cal: 0.0,
        z: 1.0,
        still_ms: None,
    }));
    if USE_DISPLAY_THREAD {
        let status = status.clone();
        std::thread::Builder::new()
            .stack_size(16 * 1024)
            .spawn(move || display_thread(lcd, status))
            .unwrap();
    }

    let mut filter = GravityFilter::default();
    let mut tracker = SwingTracker::default();
    let mut last_peak: Option<f32> = None;
    let now_ms = || (unsafe { esp_idf_svc::sys::esp_timer_get_time() } / 1000) as u32;
    let mut prev_ms = now_ms();
    let mut tick = 0u32;
    let mut last_state = tracker.state();
    loop {
        if let Some((x, y)) = touch.read(&mut i2c) {
            log::info!("touch x={x} y={y}");
            tracker.tap(); // no-op unless waiting for calibration
        }

        // 12 bytes from AX_L (0x35): accel x,y,z then gyro x,y,z, little-endian i16.
        let mut raw = [0u8; 12];
        if i2c.write_read(QMI8658_ADDR, &[0x35], &mut raw, BLOCK).is_ok() {
            let v = |i: usize| i16::from_le_bytes([raw[2 * i], raw[2 * i + 1]]) as f32;
            let accel = Vec3::new(v(0) / 8192.0, v(1) / 8192.0, v(2) / 8192.0);
            let gyro_rps = Vec3::new(v(3), v(4), v(5)) * (1.0 / 64.0 * core::f32::consts::PI / 180.0);

            let now = now_ms();
            let dt = now.wrapping_sub(prev_ms) as f32 * 0.001;
            prev_ms = now;
            filter.update(accel, gyro_rps, dt);

            let sample = MotionSample { t_ms: now, gravity: filter.gravity(), rotation_rate: gyro_rps, accel };
            for e in tracker.update(&sample) {
                match e {
                    Event::Click => log::info!("CLICK #{}", tracker.click_count()),
                    Event::CalibrationComplete => log::info!("calibrated"),
                    Event::Ready => log::info!("READY"),
                    Event::SwingComplete(p) => {
                        last_peak = Some(p);
                        log::info!("swing complete, peak {p:.1} deg")
                    }
                    Event::DirectionLocked(a) => log::info!("direction locked at {a:.1} deg"),
                }
            }
            if tracker.state() != last_state {
                log::info!("state {:?} -> {:?}", last_state, tracker.state());
                last_state = tracker.state();
            }

            let g = filter.gravity();
            let monitoring = tracker.state() == State::Monitoring;
            *status.lock().unwrap() = Status {
                state: tracker.state(),
                phase: tracker.swing_phase(),
                clicks: tracker.click_count(),
                last_peak,
                angle: if monitoring { tracker.swing_angle_degrees() } else { 0.0 },
                z: g.z,
                still_ms: tracker.autocal_elapsed_ms(now),
                from_cal: if monitoring { angle_between_degrees(g, tracker.initial_rest_gravity()) } else { 0.0 },
            };
            if tick % 100 == 0 {
                log::info!(
                    "{:?} g[{:+.3} {:+.3} {:+.3}] acc[{:+.3} {:+.3} {:+.3}] |acc| {:.3}  gyro[{:+.1} {:+.1} {:+.1}] dps  filter-vs-acc {:.1} deg  dt {} ms raw {:02x?}",
                    tracker.state(), g.x, g.y, g.z, accel.x, accel.y, accel.z, accel.norm(),
                    v(3) / 64.0, v(4) / 64.0, v(5) / 64.0, angle_between_degrees(g, accel), (dt * 1000.0) as u32, raw
                );
            }
        }
        tick = tick.wrapping_add(1);
        FreeRtos::delay_ms(10);
    }
}
