//! The watch's screens: layout, drawing and hit-testing. No swing logic here - `main` turns
//! tracker state into a `UiState`, and touches into `Btn` presses.
//!
//! Layout, colours and fonts follow the C++ app (src/ui.cpp on the old-cpp-version branch). Drawing is
//! immediate-mode: `scene` paints a whole screen for the current `UiState` into whichever strip
//! it is handed, and the display thread re-runs it only over the row bands whose inputs changed.

use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::{Arc, Mutex};

use embedded_graphics::pixelcolor::Rgb565;
use embedded_graphics::prelude::*;
use embedded_graphics::primitives::{PrimitiveStyle, Rectangle, RoundedRectangle};
use esp_idf_svc::hal::delay::FreeRtos;
use u8g2_fonts::types::{FontColor, HorizontalAlignment, VerticalPosition};
use u8g2_fonts::{fonts, FontRenderer};

use crate::co5300::{Co5300, HEIGHT, WIDTH};
use crate::gfx::{self, Strip};
use qrcodegen::{QrCode, QrCodeEcc};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Screen {
    /// "Tap when in putting position", Honu image.
    Waiting,
    /// "Hold still...", yellow, calibrating.
    Hold,
    /// Recal / Stop, battery, angle, click level, - / +. Green while armed, black otherwise.
    Run,
    /// Stop pressed.
    Bye,
    /// Bye screen's Help / Honu button: a full-screen QR code.
    Qr(QrPage),
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum QrPage {
    Help,
    Honu,
}

impl QrPage {
    fn title(self) -> &'static str {
        match self {
            QrPage::Help => "Help",
            QrPage::Honu => "Honu",
        }
    }

    /// Where the code leads: the guide the release site publishes, or the product's home page.
    fn url(self) -> String {
        match self {
            QrPage::Help => format!("{}guide/using.html", crate::update_mode::SITE_URL),
            QrPage::Honu => "https://honugolf.com".to_string(),
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Btn {
    Recal,
    Stop,
    Minus,
    Plus,
    /// Bye screen: restart into update mode.
    Update,
    /// Bye screen: QR code for the guide / for honugolf.com.
    Help,
    Honu,
}

/// What the panel itself is doing, independent of what is drawn on it.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Panel {
    On,
    /// DISPOFF: dark, but the frame is kept and the link stays usable (run-screen blanking).
    Blanked,
    /// DISPOFF + SLPIN: before light sleep or power-off.
    Asleep,
}

impl Panel {
    fn code(self) -> u8 {
        self as u8
    }
}

/// Everything the display depends on.
#[derive(Clone, Copy, PartialEq)]
pub struct UiState {
    pub screen: Screen,
    /// Armed for the next backswing (Run screen turns green).
    pub ready: bool,
    /// Swing angle in tenths of a degree.
    pub angle10: i32,
    /// Click sensitivity level, 1..=16.
    pub level: i32,
    /// Battery percent (-1 unknown) and millivolts.
    pub batt_pct: i32,
    pub batt_mv: i32,
    /// Button currently held down, drawn in its pressed colour.
    pub pressed: Option<Btn>,
    pub panel: Panel,
}

// --- layout (screen coordinates, 410 x 502) --------------------------------------------------

const W: i32 = WIDTH as i32;
const H: i32 = HEIGHT as i32;

/// (x, y, w, h)
type R = (i32, i32, i32, i32);

fn btn_rect(b: Btn) -> R {
    match b {
        Btn::Recal => ((W - 150) / 2 - 88, 12, 150, 82),
        Btn::Stop => ((W - 150) / 2 + 88, 12, 150, 82),
        Btn::Minus => ((W - 140) / 2 - 88, H - 104 - 16, 140, 104),
        Btn::Plus => ((W - 140) / 2 + 88, H - 104 - 16, 140, 104),
        Btn::Update => ((W - 240) / 2, (H - 76) / 2 + 110, 240, 76),
        // Across the top like Recal / Stop, which the Bye screen doesn't have.
        Btn::Help => ((W - 150) / 2 - 88, 12, 150, 82),
        Btn::Honu => ((W - 150) / 2 + 88, 12, 150, 82),
    }
}

/// A fingertip landing this far outside a button still presses it.
const HIT_MARGIN: i32 = 24;

/// Which button, if any, a touch at (x, y) lands on. Only the Run and Bye screens have buttons.
pub fn hit(screen: Screen, x: i32, y: i32) -> Option<Btn> {
    let buttons: &[Btn] = match screen {
        Screen::Run => &[Btn::Recal, Btn::Stop, Btn::Minus, Btn::Plus],
        Screen::Bye => &[Btn::Help, Btn::Honu, Btn::Update],
        _ => return None,
    };
    buttons.iter().copied().find(|&b| {
        let (bx, by, bw, bh) = btn_rect(b);
        x >= bx - HIT_MARGIN && x < bx + bw + HIT_MARGIN && y >= by - HIT_MARGIN && y < by + bh + HIT_MARGIN
    })
}

// Battery row: "NN% NNNN mV" right-aligned against a 108x20 bar.
const BATT_LABEL_RIGHT: i32 = (W - 180) / 2 - 60 + 180;
const BATT_BAR: (i32, i32) = (108, 20);
const BATT_BAR_X: i32 = BATT_LABEL_RIGHT + 12;
const BATT_CY_RUN: i32 = 129; // just below the Recal/Stop buttons
const BATT_CY_WAIT: i32 = 106; // higher, to clear the Honu image beneath it

const IMAGE_Y: i32 = H - 380;
static HONU: &[u8] = include_bytes!("../assets/honu.rgb565");
static HONU_BOTTOM: &[u8] = include_bytes!("../assets/honu_bottom.rgb565");

// --- colours ---------------------------------------------------------------------------------

fn rgb(c: u32) -> Rgb565 {
    Rgb565::new(((c >> 19) & 0x1F) as u8, ((c >> 10) & 0x3F) as u8, ((c >> 3) & 0x1F) as u8)
}

const BG_WAITING: u32 = 0xFFFFFF;
const BG_HOLD: u32 = 0xE6E600;
const BG_IDLE: u32 = 0x000000;
const BG_READY: u32 = 0x005A0D;
const BTN_BLUE: u32 = 0x2196F3;
const BTN_BLUE_PRESSED: u32 = 0x1565C0;
const BTN_GREY: u32 = 0x3A3A3A; // Bye-screen buttons
const BTN_GREEN: u32 = 0x1B7F3B; // ...while touched

// --- drawing ----------------------------------------------------------------------------------

/// Text centred on (cx, cy) in both directions.
fn text_c<F: u8g2_fonts::Font>(t: &mut Strip, s: &str, cx: i32, cy: i32, c: Rgb565) {
    text_at::<F>(t, s, cx, cy, HorizontalAlignment::Center, c);
}

fn text_at<F: u8g2_fonts::Font>(t: &mut Strip, s: &str, x: i32, cy: i32, a: HorizontalAlignment, c: Rgb565) {
    let _ = FontRenderer::new::<F>().render_aligned(s, Point::new(x, cy), VerticalPosition::Center, a, FontColor::Transparent(c), t);
}

fn pill(t: &mut Strip, x: i32, y: i32, w: i32, h: i32, c: Rgb565) {
    let r = (h / 2).min(w / 2).max(1) as u32;
    let _ = RoundedRectangle::with_equal_corners(Rectangle::new(Point::new(x, y), Size::new(w as u32, h as u32)), Size::new(r, r))
        .into_styled(PrimitiveStyle::with_fill(c))
        .draw(t);
}

/// A filled rounded rectangle with an explicit corner radius.
fn pill_rounded(t: &mut Strip, x: i32, y: i32, w: i32, h: i32, radius: u32, c: Rgb565) {
    let _ = RoundedRectangle::with_equal_corners(Rectangle::new(Point::new(x, y), Size::new(w as u32, h as u32)), Size::new(radius, radius))
        .into_styled(PrimitiveStyle::with_fill(c))
        .draw(t);
}

fn button(t: &mut Strip, b: Btn, s: &UiState) {
    let (x, y, w, h) = btn_rect(b);
    let pressed = s.pressed == Some(b);
    let colour = match (b, pressed) {
        (Btn::Update | Btn::Help | Btn::Honu, false) => BTN_GREY,
        (Btn::Update | Btn::Help | Btn::Honu, true) => BTN_GREEN,
        (_, false) => BTN_BLUE,
        (_, true) => BTN_BLUE_PRESSED,
    };
    let _ = RoundedRectangle::with_equal_corners(Rectangle::new(Point::new(x, y), Size::new(w as u32, h as u32)), Size::new(18, 18))
        .into_styled(PrimitiveStyle::with_fill(rgb(colour)))
        .draw(t);
    let (cx, cy) = (x + w / 2, y + h / 2);
    match b {
        Btn::Recal => text_c::<fonts::u8g2_font_logisoso22_tf>(t, "Recal", cx, cy, Rgb565::WHITE),
        Btn::Stop => text_c::<fonts::u8g2_font_logisoso22_tf>(t, "Stop", cx, cy, Rgb565::WHITE),
        Btn::Minus => text_c::<fonts::u8g2_font_logisoso34_tf>(t, "-", cx, cy, Rgb565::WHITE),
        Btn::Plus => text_c::<fonts::u8g2_font_logisoso34_tf>(t, "+", cx, cy, Rgb565::WHITE),
        // Restarting into update mode takes a moment: say so on the button itself.
        Btn::Help => text_c::<fonts::u8g2_font_logisoso22_tf>(t, "Help", cx, cy, Rgb565::WHITE),
        Btn::Honu => text_c::<fonts::u8g2_font_logisoso22_tf>(t, "Honu", cx, cy, Rgb565::WHITE),
        Btn::Update => text_c::<fonts::u8g2_font_logisoso24_tf>(t, if pressed { "Starting..." } else { "Update" }, cx, cy, Rgb565::WHITE),
    }
}

fn battery_row(t: &mut Strip, cy: i32, on_white: bool, s: &UiState) {
    let (text_colour, bar_bg) = if on_white { (Rgb565::BLACK, 0xD6D6D6) } else { (Rgb565::WHITE, 0x202020) };
    let label = if s.batt_pct < 0 { "--".to_string() } else { format!("{}% {} mV", s.batt_pct, s.batt_mv) };
    text_at::<fonts::u8g2_font_logisoso18_tf>(t, &label, BATT_LABEL_RIGHT, cy, HorizontalAlignment::Right, text_colour);

    let (bw, bh) = BATT_BAR;
    let by = cy - bh / 2;
    pill(t, BATT_BAR_X, by, bw, bh, rgb(bar_bg));
    if s.batt_pct > 0 {
        let fill = (bw * s.batt_pct.min(100) / 100).max(bh); // keep it at least a full pill
        pill(t, BATT_BAR_X, by, fill, bh, rgb(if s.batt_pct <= 20 { 0xE03B24 } else { 0x33C04D }));
    }
}

/// Paint the whole screen for `s` into the strip (clipped to it).
fn scene(t: &mut Strip, s: &UiState, qr: Option<&QrCode>) {
    let bg = match s.screen {
        Screen::Waiting => BG_WAITING,
        Screen::Hold => BG_HOLD,
        Screen::Run if s.ready => BG_READY,
        Screen::Run | Screen::Bye | Screen::Qr(_) => BG_IDLE,
    };
    let _ = t.clear(rgb(bg));

    match s.screen {
        Screen::Waiting | Screen::Hold => {
            t.blit_rgb565_le(0, IMAGE_Y, W, 380, if s.screen == Screen::Waiting { HONU } else { HONU_BOTTOM });
            if s.screen == Screen::Waiting {
                text_c::<fonts::u8g2_font_logisoso24_tf>(t, "Tap when in", W / 2, 25, Rgb565::BLACK);
                text_c::<fonts::u8g2_font_logisoso24_tf>(t, "putting position", W / 2, 65, Rgb565::BLACK);
                battery_row(t, BATT_CY_WAIT, true, s);
                text_at::<fonts::u8g2_font_helvR14_tf>(
                    t,
                    concat!("v", env!("FW_VERSION")),
                    W * 7 / 10,
                    H - 49,
                    HorizontalAlignment::Left,
                    rgb(0x707070),
                );
            } else {
                text_c::<fonts::u8g2_font_logisoso24_tf>(t, "Hold still...", W / 2, 45, Rgb565::BLACK);
            }
        }
        Screen::Run => {
            button(t, Btn::Recal, s);
            button(t, Btn::Stop, s);
            battery_row(t, BATT_CY_RUN, false, s);
            if s.ready {
                text_c::<fonts::u8g2_font_logisoso22_tf>(t, "Ready", W / 2, 188, Rgb565::WHITE);
            }
            let angle = format!("{:.1}\u{00B0}", s.angle10 as f32 / 10.0);
            text_c::<fonts::u8g2_font_logisoso34_tf>(t, &angle, W / 2, 255, Rgb565::WHITE);
            text_c::<fonts::u8g2_font_logisoso34_tf>(t, &format!("click  {}", s.level), W / 2, 325, Rgb565::WHITE);
            button(t, Btn::Minus, s);
            button(t, Btn::Plus, s);
        }
        Screen::Bye => {
            text_c::<fonts::u8g2_font_logisoso34_tf>(t, "Bye", W / 2, H / 2, Rgb565::WHITE);
            button(t, Btn::Help, s);
            button(t, Btn::Honu, s);
            button(t, Btn::Update, s);
        }
        Screen::Qr(page) => {
            text_c::<fonts::u8g2_font_logisoso24_tf>(t, page.title(), W / 2, 47, Rgb565::WHITE);
            // QR codes need a light quiet zone around them on a dark screen.
            pill_rounded(t, (W - 280) / 2, (H - 280) / 2 + 10, 280, 280, 12, Rgb565::WHITE);
            if let Some(qr) = qr {
                let n = qr.size();
                let scale = (252 / n).max(1);
                let off = (252 - scale * n) / 2;
                let (bx, by) = ((W - 252) / 2 + off, (H - 252) / 2 + 10 + off);
                for y in 0..n {
                    for x in 0..n {
                        if qr.get_module(x, y) {
                            let _ = Rectangle::new(Point::new(bx + x * scale, by + y * scale), Size::new(scale as u32, scale as u32))
                                .into_styled(PrimitiveStyle::with_fill(Rgb565::BLACK))
                                .draw(t);
                        }
                    }
                }
            }
            text_c::<fonts::u8g2_font_helvR14_tf>(t, "Upper button to go back", W / 2, H - 34 - 12, rgb(0x909090));
        }
    }
}

// --- redraw policy ----------------------------------------------------------------------------

const TOP: (usize, usize) = (0, 150); // wait label / Recal+Stop / battery row
const MID: (usize, usize) = (150, 360); // "Ready", angle, click level
const BOT: (usize, usize) = (360, HEIGHT); // - / +

fn top_pressed(s: &UiState) -> bool {
    matches!(s.pressed, Some(Btn::Recal | Btn::Stop))
}

fn bottom_pressed(s: &UiState) -> bool {
    matches!(s.pressed, Some(Btn::Minus | Btn::Plus))
}

/// Row bands that must be repainted to go from `prev` to `cur`.
fn dirty_bands(prev: Option<&UiState>, cur: &UiState) -> Vec<(usize, usize)> {
    let Some(p) = prev else { return vec![(0, HEIGHT)] };
    if p.screen != cur.screen || p.ready != cur.ready {
        return vec![(0, HEIGHT)];
    }
    let mut v = Vec::new();
    if top_pressed(p) != top_pressed(cur) || p.batt_pct != cur.batt_pct || p.batt_mv != cur.batt_mv {
        v.push(TOP);
    }
    if cur.screen == Screen::Bye && p.pressed != cur.pressed {
        // Help / Honu are in the top band; the Update button straddles the other two.
        v.push(TOP);
        v.push(MID);
        v.push(BOT);
    }
    if cur.screen == Screen::Run {
        if p.angle10 != cur.angle10 || p.level != cur.level {
            v.push(MID);
        }
        if bottom_pressed(p) != bottom_pressed(cur) {
            v.push(BOT);
        }
    }
    v
}

/// Repaints the screen whenever the shared `UiState` changes, and drives the panel's power mode.
/// Runs on its own thread: a redraw takes tens of milliseconds of CPU and must never delay IMU
/// sampling. `applied` is set to the `Panel` mode the hardware has actually reached, so the
/// sampling thread can wait for "dark" before cutting power.
pub fn display_thread(lcd: Co5300, state: Arc<Mutex<UiState>>, applied: Arc<AtomicU8>) {
    let mut buf = vec![0u8; WIDTH * gfx::STRIP_LINES * 2];
    let mut drawn: Option<UiState> = None;
    let mut qr: Option<(QrPage, QrCode)> = None;
    let mut hw = Panel::On;
    loop {
        FreeRtos::delay_ms(30);
        let cur = *state.lock().unwrap();
        if let Screen::Qr(page) = cur.screen {
            if qr.as_ref().map(|(p, _)| *p) != Some(page) {
                qr = QrCode::encode_text(&page.url(), QrCodeEcc::Medium).ok().map(|q| (page, q));
            }
        }
        let qr_ref = qr.as_ref().map(|(_, q)| q);

        if cur.panel != hw {
            match (hw, cur.panel) {
                (Panel::On, Panel::Blanked) => lcd.display_off(),
                (Panel::On, Panel::Asleep) => {
                    lcd.display_off();
                    FreeRtos::delay_ms(120);
                    lcd.sleep_in();
                }
                (Panel::Blanked, Panel::Asleep) => lcd.sleep_in(),
                (Panel::Asleep, Panel::Blanked) => {
                    lcd.sleep_out();
                }
                (_, Panel::On) => {
                    // Draw the current screen while the panel is still dark, then reveal it, so the
                    // stale frame that was in GRAM never flashes.
                    if hw == Panel::Asleep {
                        lcd.sleep_out();
                    }
                    for (y0, y1) in dirty_bands(None, &cur) {
                        gfx::render_rows(&lcd, &mut buf, y0, y1, |t| scene(t, &cur, qr_ref));
                    }
                    drawn = Some(cur);
                    lcd.display_on();
                    lcd.set_brightness(0xD0);
                }
                _ => {}
            }
            hw = cur.panel;
            applied.store(hw.code(), Ordering::Release);
            continue;
        }
        if hw != Panel::On {
            continue; // dark: don't spend CPU drawing; the wake path repaints everything
        }

        if drawn.as_ref() == Some(&cur) {
            continue;
        }
        for (y0, y1) in dirty_bands(drawn.as_ref(), &cur) {
            gfx::render_rows(&lcd, &mut buf, y0, y1, |t| scene(t, &cur, qr_ref));
        }
        drawn = Some(cur);
    }
}

/// Block until the display thread reports the panel has reached `want` (or `timeout_ms` passes).
pub fn wait_for_panel(applied: &AtomicU8, want: Panel, timeout_ms: u32) -> bool {
    let mut waited = 0;
    while applied.load(Ordering::Acquire) != want.code() {
        if waited >= timeout_ms {
            return false;
        }
        FreeRtos::delay_ms(10);
        waited += 10;
    }
    true
}
