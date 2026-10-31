mod co5300;

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

fn rgb565(r: u8, g: u8, b: u8) -> [u8; 2] {
    let v = ((r as u16 & 0xF8) << 8) | ((g as u16 & 0xFC) << 3) | (b as u16 >> 3);
    v.to_be_bytes()
}

/// Colour bars (top 400 lines) over a grey ramp, drawn in 50-line strips.
fn test_pattern(rst: esp_idf_svc::hal::gpio::AnyOutputPin) {
    use co5300::{HEIGHT, WIDTH};
    const STRIP: usize = 50;
    let mut strip = vec![0u8; WIDTH * STRIP * 2];
    let lcd = co5300::Co5300::new(rst, strip.len());
    let bars = [
        (255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
        (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0),
    ];
    let mut y = 0;
    while y < HEIGHT {
        let h = STRIP.min(HEIGHT - y);
        for row in 0..h {
            for x in 0..WIDTH {
                let (r, g, b) = if y + row < 400 {
                    bars[x * bars.len() / WIDTH]
                } else {
                    let v = (x * 255 / (WIDTH - 1)) as u8;
                    (v, v, v)
                };
                let i = (row * WIDTH + x) * 2;
                strip[i..i + 2].copy_from_slice(&rgb565(r, g, b));
            }
        }
        lcd.blit(0, y as u16, WIDTH as u16, h as u16, &strip[..h * WIDTH * 2]);
        y += h;
    }
    log::info!("test pattern drawn");
}

fn main() {
    esp_idf_svc::sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();

    let p = Peripherals::take().unwrap();
    let cfg = I2cConfig::new().baudrate(Hertz(400_000));
    let mut i2c = I2cDriver::new(p.i2c0, p.pins.gpio8, p.pins.gpio7, &cfg).unwrap();

    test_pattern(p.pins.gpio11.into());

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
