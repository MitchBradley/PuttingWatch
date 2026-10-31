//! AXP2101 power-management IC: battery voltage readout.
//!
//! The chip's own fuel-gauge percentage is unusable on this board (the C++ app saw 30% at
//! 4.19 V), so percent is derived from the terminal voltage with a rested-LiPo curve.

use esp_idf_svc::hal::delay::BLOCK;
use esp_idf_svc::hal::i2c::I2cDriver;
use esp_idf_svc::sys::EspError;

const ADDR: u8 = 0x34;
const REG_STATUS1: u8 = 0x00; // bit 3: battery connected
const REG_ADC_CTRL: u8 = 0x30; // bit 0: battery voltage ADC enable
const REG_BAT_DET: u8 = 0x68; // bit 0: battery detection enable
const REG_VBAT_H: u8 = 0x34; // 5 high bits
const REG_VBAT_L: u8 = 0x35; // 8 low bits

fn rd(i2c: &mut I2cDriver, reg: u8) -> Result<u8, EspError> {
    let mut b = [0u8];
    i2c.write_read(ADDR, &[reg], &mut b, BLOCK)?;
    Ok(b[0])
}

fn set_bit(i2c: &mut I2cDriver, reg: u8, bit: u8) -> Result<(), EspError> {
    let v = rd(i2c, reg)? | (1 << bit);
    i2c.write(ADDR, &[reg, v], BLOCK)
}

pub fn init(i2c: &mut I2cDriver) -> Result<(), EspError> {
    set_bit(i2c, REG_BAT_DET, 0)?;
    set_bit(i2c, REG_ADC_CTRL, 0)
}

/// Battery terminal voltage in mV, or None if no battery is connected or the read fails.
/// Does I2C traffic: call it every few seconds, not every frame.
pub fn millivolts(i2c: &mut I2cDriver) -> Option<i32> {
    if rd(i2c, REG_STATUS1).ok()? & (1 << 3) == 0 {
        return None;
    }
    let h = rd(i2c, REG_VBAT_H).ok()?;
    let l = rd(i2c, REG_VBAT_L).ok()?;
    Some((((h & 0x1F) as i32) << 8) | l as i32)
}

/// Cut every rail but VRTC, the ESP32's own supply included. The PMU's PWRON pin (the PWR key,
/// wired to it in hardware) is the only thing that brings the rails back, as a normal cold boot.
/// Does not return in practice: the MCU loses power before the caller's next line.
pub fn shutdown(i2c: &mut I2cDriver) -> Result<(), EspError> {
    set_bit(i2c, 0x10, 0)
}

/// (mV, %) knots of a rested single-cell LiPo, as in the C++ app. The top knot is 4115, not the
/// nominal 4200: this board's charger tops out there in practice.
const CURVE: [(i32, i32); 11] = [
    (4115, 100), (4100, 90), (4000, 80), (3900, 65), (3800, 45), (3750, 35),
    (3700, 25), (3650, 15), (3550, 8), (3450, 3), (3300, 0),
];

/// Rough state of charge, 0..=100, or -1 for mv <= 0. Terminal voltage sags under load, so this
/// reads a few points low while the display is drawing.
pub fn percent_for_millivolts(mv: i32) -> i32 {
    if mv <= 0 {
        return -1;
    }
    if mv >= CURVE[0].0 {
        return 100;
    }
    for w in CURVE.windows(2) {
        let (hi, lo) = (w[0], w[1]);
        if mv >= lo.0 {
            return lo.1 + (hi.1 - lo.1) * (mv - lo.0) / (hi.0 - lo.0);
        }
    }
    0
}
