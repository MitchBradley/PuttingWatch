//! The update-mode screens: layout, drawing, hit-testing and the repaint policy. What each screen
//! says comes from `putting_core::update_view::view_for`; this module only turns a `View` into
//! pixels. Layout and styling follow the C++ updater (PuttingWatch src/update_ui.cpp): one centred
//! column of whichever items the phase shows, stacked and centred vertically.

use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Instant;

use embedded_graphics::pixelcolor::Rgb565;
use embedded_graphics::prelude::*;
use embedded_graphics::primitives::{PrimitiveStyle, Rectangle, RoundedRectangle};
use esp_idf_svc::hal::delay::FreeRtos;
use putting_core::update_view::{view_for, wrap_by, Status, View};
use qrcodegen::{QrCode, QrCodeEcc};
use u8g2_fonts::types::{FontColor, HorizontalAlignment, VerticalPosition};
use u8g2_fonts::{fonts, FontRenderer};

use crate::co5300::{Co5300, HEIGHT, WIDTH};
use crate::gfx::{self, Strip};

const W: i32 = WIDTH as i32;
const H: i32 = HEIGHT as i32;

const CONTENT_W: i32 = 350;
const PAD_TOP: i32 = 26;
const PAD_BOTTOM: i32 = 22;
const ROW_GAP: i32 = 9;
const LINK_MARGIN_TOP: i32 = 28; // extra gap above "Change Wi-Fi": keeps it from being hit by accident

fn rgb(c: u32) -> Rgb565 {
    Rgb565::new(((c >> 19) & 0x1F) as u8, ((c >> 10) & 0x3F) as u8, ((c >> 3) & 0x1F) as u8)
}

const TEXT_MAIN: u32 = 0xFFFFFF;
const TEXT_BODY: u32 = 0xD0D0D0;
const TEXT_DIM: u32 = 0x9A9A9A;
const TEXT_WARN: u32 = 0xFF8A7A;
const LINK: u32 = 0x6CB4FF;
const BTN_GREEN: u32 = 0x1B7F3B;
const BTN_GREEN_PRESSED: u32 = 0x145F2C;
const BTN_GREY: u32 = 0x3A3A3A;
const BTN_GREY_PRESSED: u32 = 0x5A5A5A;
const BTN_DISABLED: u32 = 0x262626;

// --- fonts (the same u8g2 families the C++ updater uses) -------------------------------------------

#[derive(Clone, Copy, PartialEq, Debug)]
enum Font {
    Title,      // logisoso24
    TitleSmall, // logisoso22
    Button,     // logisoso22
    Body,       // helvR18
    Link,       // helvR14
    Note,       // helvR12
}

fn renderer(f: Font) -> FontRenderer {
    match f {
        Font::Title => FontRenderer::new::<fonts::u8g2_font_logisoso24_tf>(),
        Font::TitleSmall | Font::Button => FontRenderer::new::<fonts::u8g2_font_logisoso22_tf>(),
        Font::Body => FontRenderer::new::<fonts::u8g2_font_helvR18_tf>(),
        Font::Link => FontRenderer::new::<fonts::u8g2_font_helvR14_tf>(),
        Font::Note => FontRenderer::new::<fonts::u8g2_font_helvR12_tf>(),
    }
}

/// Line pitch, as the C++ MiniUi computes it: ascent + descent + 6.
fn line_height(f: Font) -> i32 {
    let r = renderer(f);
    r.get_ascent() as i32 - r.get_descent() as i32 + 6
}

fn text_width(f: Font, s: &str) -> i32 {
    renderer(f)
        .get_rendered_dimensions(s, Point::zero(), VerticalPosition::Baseline)
        .ok()
        .and_then(|d| d.bounding_box)
        .map_or(0, |b| b.size.width as i32)
}

fn wrap(f: Font, s: &str) -> String {
    wrap_by(s, CONTENT_W, |t| text_width(f, t))
}

fn lines(s: &str) -> i32 {
    if s.is_empty() {
        0
    } else {
        s.split('\n').count() as i32
    }
}

// --- layout -----------------------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Hit {
    Primary,
    Secondary,
    Link,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Kind {
    Title,
    Busy,
    QrBox,
    Bar,
    Pct,
    Body,
    Note,
    Primary,
    Secondary,
    Link,
}

/// One placed item: its kind, bounds (x, y, w, h) and wrapped text.
#[derive(Clone, PartialEq, Debug)]
struct Placed {
    kind: Kind,
    r: (i32, i32, i32, i32),
    text: String,
}

/// Stack the visible items as a centred column.
fn layout(v: &View) -> Vec<Placed> {
    let title_font = if v.title_small { Font::TitleSmall } else { Font::Title };
    let title = wrap(title_font, &v.title);
    let body = wrap(Font::Body, &v.body);
    let note = wrap(Font::Note, &v.note);

    // (kind, text, width, height, extra space above)
    let mut items: Vec<(Kind, String, i32, i32, i32)> = Vec::new();
    if !title.is_empty() {
        let h = lines(&title) * line_height(title_font);
        items.push((Kind::Title, title, CONTENT_W, h, 0));
    }
    if v.busy {
        items.push((Kind::Busy, String::new(), 64, 40, 0));
    }
    if v.qr {
        items.push((Kind::QrBox, String::new(), 220, 220, 0));
    }
    if v.bar {
        items.push((Kind::Bar, String::new(), 300, 26, 0));
    }
    if v.pct {
        items.push((Kind::Pct, format!("{}%", v.progress), CONTENT_W, line_height(Font::Button), 0));
    }
    if !body.is_empty() {
        let h = lines(&body) * line_height(Font::Body);
        items.push((Kind::Body, body, CONTENT_W, h, 0));
    }
    if !note.is_empty() {
        let h = lines(&note) * line_height(Font::Note);
        items.push((Kind::Note, note, CONTENT_W, h, 0));
    }
    if !v.primary.is_empty() {
        items.push((Kind::Primary, v.primary.clone(), 300, 64, 0));
    }
    if !v.secondary.is_empty() {
        items.push((Kind::Secondary, v.secondary.clone(), 300, 56, 0));
    }
    if !v.link.is_empty() {
        items.push((Kind::Link, v.link.clone(), text_width(Font::Link, &v.link), line_height(Font::Link), LINK_MARGIN_TOP));
    }

    let total: i32 = items.iter().enumerate().map(|(i, (_, _, _, h, before))| h + before + if i > 0 { ROW_GAP } else { 0 }).sum();
    let mut y = PAD_TOP + ((H - PAD_TOP - PAD_BOTTOM) - total) / 2;
    let mut out = Vec::with_capacity(items.len());
    for (i, (kind, text, w, h, before)) in items.into_iter().enumerate() {
        if i > 0 {
            y += ROW_GAP;
        }
        y += before;
        out.push(Placed { kind, r: ((W - w) / 2, y, w, h), text });
        y += h;
    }
    out
}

/// A fingertip landing this far outside a button / the link still counts.
fn hit_margin(k: Kind) -> i32 {
    match k {
        Kind::Link => 6,
        _ => 8,
    }
}

/// Which control, if any, a touch lands on. A disabled primary button doesn't respond.
pub fn hit(view: &View, x: i32, y: i32) -> Option<Hit> {
    layout(view).iter().rev().find_map(|p| {
        let which = match p.kind {
            Kind::Primary if view.primary_enabled => Hit::Primary,
            Kind::Secondary => Hit::Secondary,
            Kind::Link => Hit::Link,
            _ => return None,
        };
        let (px, py, pw, ph) = p.r;
        let m = hit_margin(p.kind);
        (x >= px - m && x < px + pw + m && y >= py - m && y < py + ph + m).then_some(which)
    })
}

// --- drawing ------------------------------------------------------------------------------------------

fn draw_lines(t: &mut Strip, f: Font, text: &str, r: (i32, i32, i32, i32), colour: u32) {
    let rend = renderer(f);
    let lh = line_height(f);
    for (i, line) in text.split('\n').enumerate() {
        let _ = rend.render_aligned(
            line,
            Point::new(r.0 + r.2 / 2, r.1 + i as i32 * lh + 3),
            VerticalPosition::Top,
            HorizontalAlignment::Center,
            FontColor::Transparent(rgb(colour)),
            t,
        );
    }
}

fn rounded(t: &mut Strip, r: (i32, i32, i32, i32), radius: u32, colour: u32) {
    let _ = RoundedRectangle::with_equal_corners(Rectangle::new(Point::new(r.0, r.1), Size::new(r.2 as u32, r.3 as u32)), Size::new(radius, radius))
        .into_styled(PrimitiveStyle::with_fill(rgb(colour)))
        .draw(t);
}

fn draw_button(t: &mut Strip, p: &Placed, bg: u32, text_colour: u32) {
    rounded(t, p.r, 18, bg);
    let _ = renderer(Font::Button).render_aligned(
        p.text.as_str(),
        Point::new(p.r.0 + p.r.2 / 2, p.r.1 + p.r.3 / 2),
        VerticalPosition::Center,
        HorizontalAlignment::Center,
        FontColor::Transparent(rgb(text_colour)),
        t,
    );
}

/// QR modules, scaled by whole pixels and centred in the 200x200 square inside the white box.
fn draw_qr(t: &mut Strip, qr: &QrCode, bx: i32, by: i32) {
    let n = qr.size();
    let scale = (200 / n).max(1);
    let off = (200 - scale * n) / 2;
    let black = rgb(0x000000);
    for y in 0..n {
        for x in 0..n {
            if qr.get_module(x, y) {
                let _ = Rectangle::new(Point::new(bx + 10 + off + x * scale, by + 10 + off + y * scale), Size::new(scale as u32, scale as u32))
                    .into_styled(PrimitiveStyle::with_fill(black))
                    .draw(t);
            }
        }
    }
}

/// Everything that varies between repaints of the same screen.
#[derive(Clone, PartialEq)]
struct Frame {
    view: View,
    placed: Vec<Placed>,
    pressed: Option<Hit>,
    dots: usize,
}

fn draw(t: &mut Strip, f: &Frame, qr: Option<&QrCode>) {
    let _ = t.clear(rgb(0x000000));
    let v = &f.view;
    for p in &f.placed {
        match p.kind {
            Kind::Title => draw_lines(t, if v.title_small { Font::TitleSmall } else { Font::Title }, &p.text, p.r, TEXT_MAIN),
            Kind::Busy => {
                let dots = [".", "..", "..."][f.dots % 3];
                let _ = renderer(Font::Title).render_aligned(
                    dots,
                    Point::new(p.r.0, p.r.1 + p.r.3 / 2),
                    VerticalPosition::Center,
                    HorizontalAlignment::Left,
                    FontColor::Transparent(rgb(TEXT_MAIN)),
                    t,
                );
            }
            Kind::QrBox => {
                rounded(t, p.r, 12, 0xFFFFFF);
                if let Some(qr) = qr {
                    draw_qr(t, qr, p.r.0, p.r.1);
                }
            }
            Kind::Bar => {
                let (x, y, w, h) = p.r;
                rounded(t, p.r, (h / 2) as u32, 0x333333);
                if v.progress > 0 {
                    let fill = (w * v.progress.min(100) as i32 / 100).max(h);
                    rounded(t, (x, y, fill, h), (h / 2) as u32, 0x33C04D);
                }
            }
            Kind::Pct => draw_lines(t, Font::Button, &p.text, p.r, TEXT_MAIN),
            Kind::Body => draw_lines(t, Font::Body, &p.text, p.r, TEXT_BODY),
            Kind::Note => draw_lines(t, Font::Note, &p.text, p.r, if v.warn { TEXT_WARN } else { TEXT_DIM }),
            Kind::Primary => {
                let pressed = f.pressed == Some(Hit::Primary);
                if v.primary_enabled {
                    draw_button(t, p, if pressed { BTN_GREEN_PRESSED } else { BTN_GREEN }, TEXT_MAIN);
                } else {
                    draw_button(t, p, BTN_DISABLED, 0x707070);
                }
            }
            Kind::Secondary => draw_button(t, p, if f.pressed == Some(Hit::Secondary) { BTN_GREY_PRESSED } else { BTN_GREY }, TEXT_MAIN),
            Kind::Link => {
                let (x, y, w, h) = p.r;
                let _ = renderer(Font::Link).render_aligned(
                    p.text.as_str(),
                    Point::new(x + w / 2, y + 3),
                    VerticalPosition::Top,
                    HorizontalAlignment::Center,
                    FontColor::Transparent(rgb(LINK)),
                    t,
                );
                // underline
                let _ = Rectangle::new(Point::new(x, y + h - 4), Size::new(w as u32, 1)).into_styled(PrimitiveStyle::with_fill(rgb(LINK))).draw(t);
            }
        }
    }
}

// --- repaint policy --------------------------------------------------------------------------------------

/// Row bands (even-aligned) that changed between two frames of the same screen, or `None` if the
/// whole screen must be repainted.
fn dirty_bands(prev: &Frame, cur: &Frame) -> Option<Vec<(usize, usize)>> {
    // Same text, same items in the same places; only progress / pressed / dots may differ.
    let mut a = prev.view.clone();
    let mut b = cur.view.clone();
    a.progress = 0;
    b.progress = 0;
    if a != b || prev.placed.iter().map(|p| (p.kind, p.r)).ne(cur.placed.iter().map(|p| (p.kind, p.r))) {
        return None;
    }
    let mut rows: Vec<(i32, i32)> = Vec::new();
    for (pp, cp) in prev.placed.iter().zip(&cur.placed) {
        let changed = match cp.kind {
            Kind::Bar | Kind::Pct => prev.view.progress != cur.view.progress,
            Kind::Busy => prev.dots % 3 != cur.dots % 3,
            Kind::Primary => (prev.pressed == Some(Hit::Primary)) != (cur.pressed == Some(Hit::Primary)),
            Kind::Secondary => (prev.pressed == Some(Hit::Secondary)) != (cur.pressed == Some(Hit::Secondary)),
            _ => false,
        };
        if changed || pp.text != cp.text {
            rows.push((cp.r.1 - 4, cp.r.1 + cp.r.3 + 4));
        }
    }
    let mut out = Vec::new();
    for (y0, y1) in rows {
        let y0 = (y0.max(0) & !1) as usize;
        let y1 = ((y1 + 1) & !1).min(H) as usize;
        out.push((y0, y1));
    }
    Some(out)
}

/// What the display thread watches.
pub struct Shared {
    pub status: Mutex<Status>,
    /// Whether the battery allows an install (shown on the Available screen).
    pub battery_ok: AtomicBool,
    /// The control currently held down: 0 none, 1 primary, 2 secondary, 3 link.
    pub pressed: AtomicU8,
}

impl Shared {
    pub fn new() -> Arc<Self> {
        Arc::new(Shared { status: Mutex::new(Status::default()), battery_ok: AtomicBool::new(true), pressed: AtomicU8::new(0) })
    }

    pub fn set_pressed(&self, h: Option<Hit>) {
        self.pressed.store(
            match h {
                None => 0,
                Some(Hit::Primary) => 1,
                Some(Hit::Secondary) => 2,
                Some(Hit::Link) => 3,
            },
            Ordering::Relaxed,
        );
    }

    fn pressed(&self) -> Option<Hit> {
        match self.pressed.load(Ordering::Relaxed) {
            1 => Some(Hit::Primary),
            2 => Some(Hit::Secondary),
            3 => Some(Hit::Link),
            _ => None,
        }
    }
}

/// The view for the current status (also what touches are hit-tested against).
pub fn current_view(shared: &Shared, running: &str, site: &str) -> View {
    let st = shared.status.lock().unwrap().clone();
    view_for(&st, running, shared.battery_ok.load(Ordering::Relaxed), site)
}

/// Repaint whenever the status, battery gate, pressed control or busy dots change. Runs on its own
/// low-priority thread: a full repaint is tens of ms of CPU and must not slow the download.
pub fn display_thread(lcd: Co5300, shared: Arc<Shared>, running: &'static str, site: &'static str) {
    let mut buf = vec![0u8; WIDTH * gfx::STRIP_LINES * 2];
    let started = Instant::now();
    let mut drawn: Option<Frame> = None;
    let mut qr: Option<(String, QrCode)> = None;
    loop {
        FreeRtos::delay_ms(60);
        let view = current_view(&shared, running, site);
        let cur = Frame {
            placed: layout(&view),
            view,
            pressed: shared.pressed(),
            dots: (started.elapsed().as_millis() / 450) as usize % 3,
        };
        if drawn.as_ref() == Some(&cur) {
            continue;
        }
        if cur.view.qr {
            if qr.as_ref().map(|(d, _)| d.as_str()) != Some(cur.view.qr_data.as_str()) {
                qr = QrCode::encode_text(&cur.view.qr_data, QrCodeEcc::Medium).ok().map(|q| (cur.view.qr_data.clone(), q));
            }
        }
        let qr_ref = qr.as_ref().map(|(_, q)| q);
        match drawn.as_ref().and_then(|p| dirty_bands(p, &cur)) {
            Some(bands) => {
                for (y0, y1) in bands {
                    gfx::render_rows(&lcd, &mut buf, y0, y1, |t| draw(t, &cur, qr_ref));
                }
            }
            None => gfx::render(&lcd, &mut buf, |t| draw(t, &cur, qr_ref)),
        }
        drawn = Some(cur);
    }
}
