//! FT3168 capacitive touch controller on the shared I2C bus.
//!
//! Same bring-up as the C++ app's Arduino_FT3x68: pulse the reset line, then put the chip in
//! "monitor" power mode (reg 0xA5 = 1). INT (active low) is asserted while a finger is down.

use esp_idf_svc::hal::delay::{FreeRtos, BLOCK};
use esp_idf_svc::hal::gpio::{AnyIOPin, AnyOutputPin, Input, Pull, PinDriver};
use esp_idf_svc::hal::i2c::I2cDriver;

const ADDR: u8 = 0x38;

pub struct Touch<'d> {
    int: PinDriver<'d, Input>,
}

impl<'d> Touch<'d> {
    pub fn new(i2c: &mut I2cDriver, rst: AnyOutputPin<'_>, int: AnyIOPin<'d>) -> Self {
        let mut rst = esp_idf_svc::hal::gpio::PinDriver::output(rst).unwrap();
        rst.set_high().unwrap();
        FreeRtos::delay_ms(1);
        rst.set_low().unwrap();
        FreeRtos::delay_ms(20);
        rst.set_high().unwrap();
        FreeRtos::delay_ms(50);
        core::mem::forget(rst);

        let id = {
            let mut b = [0u8];
            i2c.write_read(ADDR, &[0xA0], &mut b, BLOCK).map(|_| b[0])
        };
        log::info!("FT3168 device id reg 0xA0: {id:?} (0x03 = FT3168)");
        i2c.write(ADDR, &[0xA5, 0x01], BLOCK).ok();
        FreeRtos::delay_ms(20);

        Touch { int: PinDriver::input(int, Pull::Floating).unwrap() }
    }

    /// First finger's position if a finger is down: reads only while INT is asserted.
    pub fn read(&mut self, i2c: &mut I2cDriver) -> Option<(u16, u16)> {
        if self.int.is_high() {
            return None;
        }
        // 0x02 = finger count, then X high/low and Y high/low (high bytes carry event flags in the top nibble).
        let mut r = [0u8; 5];
        i2c.write_read(ADDR, &[0x02], &mut r, BLOCK).ok()?;
        if r[0] == 0 {
            return None;
        }
        Some((((r[1] & 0x0F) as u16) << 8 | r[2] as u16, ((r[3] & 0x0F) as u16) << 8 | r[4] as u16))
    }
}
