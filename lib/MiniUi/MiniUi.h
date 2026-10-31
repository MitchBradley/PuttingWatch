#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "mui_image.h"

class Arduino_GFX;

// A very small retained-mode widget layer on top of Arduino_GFX, replacing
// LVGL for this app's few, mostly static screens.
//
// Widgets (box, label, button, bar, image, QR code) live at fixed screen
// positions and are drawn in creation order. Changing one invalidates its
// rectangle; refresh() repaints just the invalidated rectangles, strip by
// strip, into a RAM buffer and pushes each strip to the panel in one go - the
// same partial-update scheme LVGL used, so each region lands on the panel
// atomically and the panel's GRAM keeps everything else. Rectangles are
// widened to even start / odd end first (the CO5300 stripes otherwise).
//
// Text is Arduino_GFX's built-in u8g2-format font renderer (1 bit per pixel);
// font arrays come from src/assets/ui_fonts.h.
//
// Single-threaded: call everything from the loop task.
namespace mui {

constexpr int kScreenW = 410;
constexpr int kScreenH = 502;

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

enum class Kind : uint8_t { Box, Label, Button, Bar, Image, Qr };
enum class Align : uint8_t { Left, Center, Right };
enum class VAlign : uint8_t { Top, Middle };

struct Widget {
    Kind kind;
    Rect r;
    bool hidden = false;
    bool disabled = false;
    bool pressed = false;

    uint32_t bg = 0;         // RGB888; Box/Button fill, Bar track
    bool hasBg = false;      // Box: fill at all?
    uint32_t bgPressed = 0;  // Button, while touched
    uint32_t bgDisabled = 0x444444;
    uint32_t fg = 0xFFFFFF;  // text colour; Bar fill; Qr dark modules
    uint32_t fgDisabled = 0x888888;
    int radius = 0;

    const uint8_t* font = nullptr;
    Align align = Align::Center;
    VAlign valign = VAlign::Middle;
    bool underline = false;
    std::string text;  // lines separated by '\n'

    int value = 0;  // Bar, 0..100

    const mui_image_t* image = nullptr;

    std::string qrText;
    std::vector<uint8_t> qrModules;  // qrcodegen output

    int ext = 0;  // invisible extra touch margin, px
    std::function<void()> onClick;

    // Setters repaint only when something actually changed.
    void show(bool on);
    void setText(const std::string& t);
    void setFg(uint32_t rgb);
    void setBg(uint32_t rgb);
    void setPressed(bool on);
    void setDisabled(bool on);
    void setValue(int v);
    void setRect(const Rect& nr);
    void setQr(const std::string& data);
    void setFont(const uint8_t* f);
};

// `buf`/`bufPixels`: RGB565 scratch for one strip; bigger means fewer strips
// per repaint (410 px * 100 rows = ~82 KB was what LVGL used).
void init(Arduino_GFX* panel, uint16_t* buf, size_t bufPixels);

Widget* add(Kind kind, Rect r);
void reset();  // forget every widget (the screenshot tool builds several screens in turn)
Widget* widgetAt(int x, int y);  // the visible, clickable widget a touch there would hit

// The colour behind every widget, and what a touch that hits no widget does.
void setBackground(uint32_t rgb);
void onBackgroundClick(std::function<void()> cb);

void invalidate(const Rect& r);
void invalidateAll();
void refresh();  // paint everything invalidated since the last call

// One touch report from the controller (it keeps reporting while the finger
// is down): a new touch clicks the widget under it, once; the touch ends
// when reports stop arriving. Call service() regularly.
// Where touch reports come from: returns true (and the position) when the
// controller has a new report. Polled by update().
void setTouchSource(std::function<bool(int*, int*)> src);
// The per-loop call: poll touch, end finished touches, repaint what changed.
void update(uint32_t nowMs);
void touch(int x, int y, uint32_t nowMs);
void service(uint32_t nowMs);
uint32_t idleMs(uint32_t nowMs);  // time since the last touch report

// Text metrics for the current layout code. Heights are per line.
int lineHeight(const uint8_t* font);
int ascent(const uint8_t* font);
int textWidth(const uint8_t* font, const std::string& line);
// Greedy word wrap of `s` to `maxW` px; explicit '\n's are kept.
std::string wrap(const uint8_t* font, const std::string& s, int maxW);
int lineCount(const std::string& s);

constexpr uint16_t rgb565(uint32_t rgb) {
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

}  // namespace mui
