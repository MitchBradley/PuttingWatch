//! embedded-graphics `DrawTarget` over a horizontal strip of the panel.
//!
//! The C6 has no PSRAM, so a full 410x502 RGB565 frame (411 KB) won't fit. `render`
//! instead runs the scene closure once per strip, each time clipped to that strip's
//! rows, and pushes the strip to the panel - the same approach as MiniUi's StripCanvas.

use core::convert::Infallible;

use embedded_graphics::pixelcolor::raw::RawU16;
use embedded_graphics::pixelcolor::Rgb565;
use embedded_graphics::prelude::*;
use embedded_graphics::primitives::Rectangle;

use crate::co5300::{Co5300, HEIGHT, WIDTH};

pub const STRIP_LINES: usize = 50;

pub struct Strip<'a> {
    buf: &'a mut [u8],
    y0: i32,
    lines: i32,
}

impl OriginDimensions for Strip<'_> {
    fn size(&self) -> Size {
        Size::new(WIDTH as u32, HEIGHT as u32)
    }
}

impl DrawTarget for Strip<'_> {
    type Color = Rgb565;
    type Error = Infallible;

    fn draw_iter<I: IntoIterator<Item = Pixel<Rgb565>>>(&mut self, pixels: I) -> Result<(), Infallible> {
        for Pixel(p, c) in pixels {
            let row = p.y - self.y0;
            if p.x >= 0 && p.x < WIDTH as i32 && row >= 0 && row < self.lines {
                let i = (row as usize * WIDTH + p.x as usize) * 2;
                self.buf[i..i + 2].copy_from_slice(&RawU16::from(c).into_inner().to_be_bytes());
            }
        }
        Ok(())
    }

    fn fill_solid(&mut self, area: &Rectangle, c: Rgb565) -> Result<(), Infallible> {
        let area = area.intersection(&Rectangle::new(
            Point::new(0, self.y0),
            Size::new(WIDTH as u32, self.lines as u32),
        ));
        let Some(br) = area.bottom_right() else { return Ok(()) };
        let px = RawU16::from(c).into_inner().to_be_bytes();
        for y in area.top_left.y..=br.y {
            let row = (y - self.y0) as usize * WIDTH;
            for x in area.top_left.x..=br.x {
                let i = (row + x as usize) * 2;
                self.buf[i..i + 2].copy_from_slice(&px);
            }
        }
        Ok(())
    }
}

/// Draw the whole screen by running `scene` against every strip in turn.
pub fn render(lcd: &Co5300, buf: &mut [u8], scene: impl Fn(&mut Strip)) {
    render_rows(lcd, buf, 0, HEIGHT, scene)
}

/// Like `render`, but only redraws rows `y_start..y_end` (y_start must be even).
pub fn render_rows(lcd: &Co5300, buf: &mut [u8], y_start: usize, y_end: usize, scene: impl Fn(&mut Strip)) {
    let mut y = y_start;
    while y < y_end {
        let lines = STRIP_LINES.min(y_end - y);
        let mut strip = Strip { buf: &mut buf[..lines * WIDTH * 2], y0: y as i32, lines: lines as i32 };
        scene(&mut strip);
        lcd.blit(0, y as u16, WIDTH as u16, lines as u16, &buf[..lines * WIDTH * 2]);
        y += lines;
    }
}
