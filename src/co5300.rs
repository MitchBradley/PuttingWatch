//! CO5300 AMOLED over QSPI, driven through ESP-IDF's esp_lcd SPI panel-IO layer.
//!
//! Wire protocol (same as Arduino_GFX's Arduino_ESP32QSPI): every transfer is an
//! 8-bit opcode (0x02 = register write, 0x32 = pixel write) sent on one line, a
//! 24-bit "address" holding the panel command as 0x00CC00, then data. esp_lcd's
//! quad_mode with 32-bit commands encodes exactly that as (opcode << 24) | (cmd << 8).

use esp_idf_svc::hal::delay::FreeRtos;
use esp_idf_svc::hal::gpio::{AnyOutputPin, PinDriver};
use esp_idf_svc::sys::*;

pub const WIDTH: usize = 410;
pub const HEIGHT: usize = 502;
const COL_OFFSET: u16 = 22; // panel gap, same as the C++ app

const HOST: spi_host_device_t = spi_host_device_t_SPI2_HOST;

pub struct Co5300 {
    io: esp_lcd_panel_io_handle_t,
}

// The esp_lcd panel-IO handle is only used by whichever thread owns the Co5300.
unsafe impl Send for Co5300 {}

fn check(what: &str, err: esp_err_t) {
    if err != ESP_OK {
        panic!("{what} failed: esp_err_t {err}");
    }
}

impl Co5300 {
    /// Pins: SDIO0..3 = 1,2,3,4, SCLK 0, CS 5 (see pin_config.h). Reset pin is toggled by the caller's `rst`.
    pub fn new(rst: AnyOutputPin, max_bytes: usize) -> Self {
        let mut rst = PinDriver::output(rst).unwrap();
        rst.set_high().unwrap();
        FreeRtos::delay_ms(10);
        rst.set_low().unwrap();
        FreeRtos::delay_ms(200);
        rst.set_high().unwrap();
        FreeRtos::delay_ms(200);
        core::mem::forget(rst); // keep the pin driven high

        let mut bus: spi_bus_config_t = unsafe { core::mem::zeroed() };
        bus.__bindgen_anon_1.data0_io_num = 1;
        bus.__bindgen_anon_2.data1_io_num = 2;
        bus.__bindgen_anon_3.data2_io_num = 3;
        bus.__bindgen_anon_4.data3_io_num = 4;
        bus.sclk_io_num = 0;
        bus.data4_io_num = -1;
        bus.data5_io_num = -1;
        bus.data6_io_num = -1;
        bus.data7_io_num = -1;
        bus.max_transfer_sz = max_bytes as i32;
        bus.flags = SPICOMMON_BUSFLAG_QUAD;
        check("spi_bus_initialize", unsafe {
            spi_bus_initialize(HOST, &bus, spi_common_dma_t_SPI_DMA_CH_AUTO)
        });

        let mut cfg: esp_lcd_panel_io_spi_config_t = unsafe { core::mem::zeroed() };
        cfg.cs_gpio_num = 5;
        cfg.dc_gpio_num = -1;
        cfg.spi_mode = 0;
        cfg.pclk_hz = 80_000_000;
        cfg.trans_queue_depth = 4;
        cfg.lcd_cmd_bits = 32;
        cfg.lcd_param_bits = 8;
        cfg.flags.set_quad_mode(1);
        let mut io: esp_lcd_panel_io_handle_t = core::ptr::null_mut();
        check("esp_lcd_new_panel_io_spi", unsafe {
            esp_lcd_new_panel_io_spi(HOST as usize as esp_lcd_spi_bus_handle_t, &cfg, &mut io)
        });

        let d = Co5300 { io };
        d.init();
        d
    }

    fn cmd(&self, c: u8, params: &[u8]) {
        let lcd_cmd = (0x02i32 << 24) | ((c as i32) << 8);
        check("tx_param", unsafe {
            esp_lcd_panel_io_tx_param(self.io, lcd_cmd, params.as_ptr().cast(), params.len())
        });
    }

    // Sequence from Arduino_CO5300::tftInit (co5300_init_operations).
    fn init(&self) {
        self.cmd(0x11, &[]); // sleep out
        FreeRtos::delay_ms(120);
        self.cmd(0xFE, &[0x00]);
        self.cmd(0xC4, &[0x80]); // SPI mode control
        self.cmd(0x3A, &[0x55]); // 16 bit/pixel
        self.cmd(0x53, &[0x20]);
        self.cmd(0x63, &[0xFF]);
        self.cmd(0x29, &[]); // display on
        self.cmd(0x51, &[0xD0]); // brightness
        self.cmd(0x58, &[0x00]); // contrast enhancement off
        FreeRtos::delay_ms(10);
        self.cmd(0x36, &[0x00]); // MADCTL: rotation 0, RGB
        self.cmd(0x20, &[]); // inversion off
    }

    pub fn set_brightness(&self, b: u8) {
        self.cmd(0x51, &[b]);
    }

    /// Stop emitting light (DISPOFF). GRAM is kept and the QSPI link stays usable, so a frame
    /// can be drawn while dark and then revealed with `display_on`.
    pub fn display_off(&self) {
        self.cmd(0x28, &[]);
    }

    pub fn display_on(&self) {
        self.cmd(0x29, &[]);
    }

    /// Deep panel sleep (SLPIN); leave it with `sleep_out`, which needs ~120 ms to settle.
    pub fn sleep_in(&self) {
        self.cmd(0x10, &[]);
        FreeRtos::delay_ms(120);
    }

    pub fn sleep_out(&self) {
        self.cmd(0x11, &[]);
        FreeRtos::delay_ms(120);
    }

    /// Push big-endian RGB565 `pixels` (w*h*2 bytes, DMA-capable) into the window at (x, y).
    /// x must be even and x+w odd (CO5300 column pairing); same for y.
    pub fn blit(&self, x: u16, y: u16, w: u16, h: u16, pixels: &[u8]) {
        let x0 = x + COL_OFFSET;
        let x1 = x0 + w - 1;
        self.cmd(0x2A, &[(x0 >> 8) as u8, x0 as u8, (x1 >> 8) as u8, x1 as u8]);
        let y1 = y + h - 1;
        self.cmd(0x2B, &[(y >> 8) as u8, y as u8, (y1 >> 8) as u8, y1 as u8]);
        let lcd_cmd = (0x32i32 << 24) | (0x2C << 8);
        check("tx_color", unsafe {
            esp_lcd_panel_io_tx_color(self.io, lcd_cmd, pixels.as_ptr().cast(), pixels.len())
        });
    }
}
