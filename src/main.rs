mod co5300;
mod gfx;

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
use embedded_graphics::primitives::{Circle, PrimitiveStyle, PrimitiveStyleBuilder, Rectangle, RoundedRectangle};
use u8g2_fonts::types::{FontColor, HorizontalAlignment, VerticalPosition};
use u8g2_fonts::{fonts, FontRenderer};

fn text<F: u8g2_fonts::Font>(t: &mut gfx::Strip, s: &str, x: i32, y: i32, c: Rgb565) {
    let _ = FontRenderer::new::<F>().render_aligned(
        s,
        Point::new(x, y),
        VerticalPosition::Baseline,
        HorizontalAlignment::Center,
        FontColor::Transparent(c),
        t,
    );
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
    let _ = Circle::with_center(Point::new(205, 250), 150)
        .into_styled(PrimitiveStyleBuilder::new().fill_color(Rgb565::CSS_SEA_GREEN).stroke_color(Rgb565::WHITE).stroke_width(4).build())
        .draw(t);
    text::<fonts::u8g2_font_logisoso38_tf>(t, "12.5", 205, 265, Rgb565::WHITE);
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

fn draw_screen(rst: esp_idf_svc::hal::gpio::AnyOutputPin) {
    let mut buf = vec![0u8; co5300::WIDTH * gfx::STRIP_LINES * 2];
    let lcd = co5300::Co5300::new(rst, buf.len());
    let t0 = std::time::Instant::now();
    gfx::render(&lcd, &mut buf, scene);
    log::info!("full-screen render: {} ms", t0.elapsed().as_millis());
}

fn main() {
    esp_idf_svc::sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();

    let p = Peripherals::take().unwrap();
    let cfg = I2cConfig::new().baudrate(Hertz(400_000));
    let mut i2c = I2cDriver::new(p.i2c0, p.pins.gpio8, p.pins.gpio7, &cfg).unwrap();

    draw_screen(p.pins.gpio11.into());

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

    // Accel +-4 g @ ~250 Hz, gyro +-512 dps @ ~250 Hz, both enabled.
    for (reg, val) in [(0x03u8, 0x15u8), (0x04, 0x55), (0x08, 0x03)] {
        i2c.write(QMI8658_ADDR, &[reg, val], BLOCK).unwrap();
    }

    loop {
        // 12 bytes from AX_L (0x35): accel x,y,z then gyro x,y,z, little-endian i16.
        let mut raw = [0u8; 12];
        if i2c.write_read(QMI8658_ADDR, &[0x35], &mut raw, BLOCK).is_ok() {
            let v = |i: usize| i16::from_le_bytes([raw[2 * i], raw[2 * i + 1]]) as f32;
            log::info!(
                "a[{:+.3} {:+.3} {:+.3}] g   w[{:+.1} {:+.1} {:+.1}] dps",
                v(0) / 8192.0, v(1) / 8192.0, v(2) / 8192.0,
                v(3) / 64.0, v(4) / 64.0, v(5) / 64.0,
            );
        }
        FreeRtos::delay_ms(200);
    }
}
