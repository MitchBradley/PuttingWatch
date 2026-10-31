#include "MiniUi.h"

#include <Arduino.h>
#include <Arduino_GFX.h>
#include <algorithm>
#include <cstring>
#include "qrcodegen.h"

namespace mui {
namespace {

// Arduino_GFX that draws into a RAM strip: all drawing uses SCREEN
// coordinates, and whatever falls outside the current window is dropped, so
// widgets never need to know which strip they are being painted into.
class StripCanvas : public Arduino_GFX {
public:
    StripCanvas() : Arduino_GFX(kScreenW, kScreenH) {}

    bool begin(int32_t = GFX_NOT_DEFINED) override { return true; }

    void setBuffer(uint16_t* b) { buf_ = b; }
    void setWindow(int x, int y, int w, int h) {
        wx_ = x;
        wy_ = y;
        ww_ = w;
        wh_ = h;
    }

    void writePixelPreclipped(int16_t x, int16_t y, uint16_t c) override {
        int xx = x - wx_, yy = y - wy_;
        if ((unsigned)xx < (unsigned)ww_ && (unsigned)yy < (unsigned)wh_) buf_[yy * ww_ + xx] = c;
    }

    void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override {
        int x0 = std::max<int>(x, wx_), x1 = std::min<int>(x + w, wx_ + ww_);
        int y0 = std::max<int>(y, wy_), y1 = std::min<int>(y + h, wy_ + wh_);
        for (int yy = y0; yy < y1; yy++) {
            uint16_t* row = buf_ + (yy - wy_) * ww_ + (x0 - wx_);
            for (int xx = x0; xx < x1; xx++) *row++ = c;
        }
    }
    void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override {
        writeFillRectPreclipped(x, y, w, 1, c);
    }
    void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) override {
        writeFillRectPreclipped(x, y, 1, h, c);
    }

    void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* bitmap, int16_t w, int16_t h) override {
        int x0 = std::max<int>(x, wx_), x1 = std::min<int>(x + w, wx_ + ww_);
        int y0 = std::max<int>(y, wy_), y1 = std::min<int>(y + h, wy_ + wh_);
        if (x1 <= x0) return;
        for (int yy = y0; yy < y1; yy++)
            memcpy(buf_ + (yy - wy_) * ww_ + (x0 - wx_), bitmap + (yy - y) * w + (x0 - x),
                   (x1 - x0) * sizeof(uint16_t));
    }
    using Arduino_GFX::draw16bitRGBBitmap;

private:
    uint16_t* buf_ = nullptr;
    int wx_ = 0, wy_ = 0, ww_ = 0, wh_ = 0;
};

constexpr uint32_t kReleaseMs = 120;  // no touch report for this long = finger lifted

Arduino_GFX* gPanel = nullptr;
StripCanvas* gCanvas = nullptr;
uint16_t* gBuf = nullptr;
size_t gBufPixels = 0;
std::vector<Widget*> gWidgets;
std::vector<Rect> gDirty;
uint32_t gBackground = 0;
std::function<void()> gBackgroundClick;
std::function<bool(int*, int*)> gTouchSource;

bool gDown = false;
Widget* gPressed = nullptr;
uint32_t gLastReportMs = 0;
bool gEverTouched = false;

bool intersects(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
Rect unite(const Rect& a, const Rect& b) {
    int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    int x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
    return {x0, y0, x1 - x0, y1 - y0};
}

// u8g2 font header: byte 13 = ascent of 'A', byte 14 = descent of 'g' (<= 0).
int fontAscent(const uint8_t* f) { return (int8_t)f[13]; }
int fontDescent(const uint8_t* f) { return -(int8_t)f[14]; }

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t nl = s.find('\n', start);
        out.push_back(s.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

void drawText(const Widget& w, const std::string& text, int boxX, int boxY, int boxW, int boxH,
              uint32_t color) {
    if (text.empty() || !w.font) return;
    StripCanvas& c = *gCanvas;
    const int lh = lineHeight(w.font);
    const auto lines = splitLines(text);
    const int block = (int)lines.size() * lh;
    int top = (w.valign == VAlign::Middle) ? boxY + (boxH - block) / 2 : boxY;
    c.setFont(w.font);
    c.setUTF8Print(true);
    c.setTextWrap(false);
    c.setTextColor(rgb565(color));
    for (size_t i = 0; i < lines.size(); i++) {
        const int tw = textWidth(w.font, lines[i]);
        int x = boxX;
        if (w.align == Align::Center)
            x += (boxW - tw) / 2;
        else if (w.align == Align::Right)
            x += boxW - tw;
        const int baseline = top + (int)i * lh + (lh - (fontAscent(w.font) + fontDescent(w.font))) / 2 +
                             fontAscent(w.font);
        c.setCursor(x, baseline);
        c.print(lines[i].c_str());
        if (w.underline && tw > 0) c.fillRect(x, baseline + 3, tw, 2, rgb565(color));
    }
}

void drawWidget(const Widget& w) {
    StripCanvas& c = *gCanvas;
    const Rect& r = w.r;
    switch (w.kind) {
        case Kind::Box:
            if (w.hasBg) {
                if (w.radius)
                    c.fillRoundRect(r.x, r.y, r.w, r.h, w.radius, rgb565(w.bg));
                else
                    c.fillRect(r.x, r.y, r.w, r.h, rgb565(w.bg));
            }
            break;
        case Kind::Button: {
            uint32_t fill = w.disabled ? w.bgDisabled : (w.pressed ? w.bgPressed : w.bg);
            c.fillRoundRect(r.x, r.y, r.w, r.h, w.radius, rgb565(fill));
            drawText(w, w.text, r.x, r.y, r.w, r.h, w.disabled ? w.fgDisabled : w.fg);
            break;
        }
        case Kind::Label:
            drawText(w, w.text, r.x, r.y, r.w, r.h, w.fg);
            break;
        case Kind::Bar: {
            c.fillRoundRect(r.x, r.y, r.w, r.h, r.h / 2, rgb565(w.bg));
            if (w.value > 0) {
                int fw = std::max(r.h, r.w * std::min(w.value, 100) / 100);
                c.fillRoundRect(r.x, r.y, fw, r.h, r.h / 2, rgb565(w.fg));
            }
            break;
        }
        case Kind::Image:
            if (w.image) c.draw16bitRGBBitmap(r.x, r.y, const_cast<uint16_t*>(w.image->px), w.image->w, w.image->h);
            break;
        case Kind::Qr: {
            if (w.qrModules.empty()) break;
            const int n = qrcodegen_getSize(w.qrModules.data());
            const int scale = std::max(1, std::min(r.w, r.h) / n);
            const int ox = r.x + (r.w - n * scale) / 2, oy = r.y + (r.h - n * scale) / 2;
            for (int y = 0; y < n; y++)
                for (int x = 0; x < n; x++)
                    if (qrcodegen_getModule(w.qrModules.data(), x, y))
                        c.fillRect(ox + x * scale, oy + y * scale, scale, scale, rgb565(w.fg));
            break;
        }
    }
}

void renderRect(Rect r) {
    const int rows = (int)(gBufPixels / r.w) & ~1;
    if (rows < 2) return;
    for (int y = r.y; y < r.y + r.h; y += rows) {
        const int h = std::min(rows, r.y + r.h - y);
        const Rect strip{r.x, y, r.w, h};
        gCanvas->setWindow(strip.x, strip.y, strip.w, strip.h);
        gCanvas->writeFillRectPreclipped(strip.x, strip.y, strip.w, strip.h, rgb565(gBackground));
        for (const Widget* w : gWidgets)
            if (!w->hidden && intersects(w->r, strip)) drawWidget(*w);
        gPanel->draw16bitRGBBitmap(strip.x, strip.y, gBuf, strip.w, strip.h);
    }
}

Widget* hitTest(int x, int y) {
    for (size_t i = gWidgets.size(); i-- > 0;) {
        Widget* w = gWidgets[i];
        if (w->hidden || (!w->onClick && w->kind != Kind::Button)) continue;
        if (x >= w->r.x - w->ext && x < w->r.x + w->r.w + w->ext && y >= w->r.y - w->ext &&
            y < w->r.y + w->r.h + w->ext)
            return w;
    }
    return nullptr;
}

}  // namespace

// ---- widget setters ------------------------------------------------------------

void Widget::show(bool on) {
    if (hidden == !on) return;
    hidden = !on;
    invalidate(r);
}
void Widget::setText(const std::string& t) {
    if (text == t) return;
    text = t;
    invalidate(r);
}
void Widget::setFg(uint32_t rgb) {
    if (fg == rgb) return;
    fg = rgb;
    invalidate(r);
}
void Widget::setBg(uint32_t rgb) {
    if (bg == rgb && hasBg) return;
    bg = rgb;
    hasBg = true;
    invalidate(r);
}
void Widget::setPressed(bool on) {
    if (pressed == on) return;
    pressed = on;
    invalidate(r);
}
void Widget::setDisabled(bool on) {
    if (disabled == on) return;
    disabled = on;
    invalidate(r);
}
void Widget::setValue(int v) {
    if (value == v) return;
    value = v;
    invalidate(r);
}
void Widget::setRect(const Rect& nr) {
    if (nr.x == r.x && nr.y == r.y && nr.w == r.w && nr.h == r.h) return;
    invalidate(r);
    r = nr;
    invalidate(r);
}
void Widget::setFont(const uint8_t* f) {
    if (font == f) return;
    font = f;
    invalidate(r);
}
void Widget::setQr(const std::string& data) {
    if (data == qrText && !qrModules.empty()) return;
    qrText = data;
    // Version 1..12 is plenty for the short strings used here.
    std::vector<uint8_t> tmp(qrcodegen_BUFFER_LEN_FOR_VERSION(12));
    qrModules.assign(qrcodegen_BUFFER_LEN_FOR_VERSION(12), 0);
    if (!qrcodegen_encodeText(data.c_str(), tmp.data(), qrModules.data(), qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, 12, qrcodegen_Mask_AUTO, true))
        qrModules.clear();
    invalidate(r);
}

// ---- module API ----------------------------------------------------------------

void init(Arduino_GFX* panel, uint16_t* buf, size_t bufPixels) {
    gPanel = panel;
    gBuf = buf;
    gBufPixels = bufPixels;
    if (!gCanvas) gCanvas = new StripCanvas();
    gCanvas->setBuffer(buf);
    gCanvas->setWindow(0, 0, kScreenW, bufPixels / kScreenW);
}

void reset() {
    for (Widget* w : gWidgets) delete w;
    gWidgets.clear();
    gDirty.clear();
    gBackground = 0;
    gBackgroundClick = nullptr;
    gDown = false;
    gPressed = nullptr;
}

Widget* widgetAt(int x, int y) { return hitTest(x, y); }

Widget* add(Kind kind, Rect r) {
    Widget* w = new Widget();
    w->kind = kind;
    w->r = r;
    gWidgets.push_back(w);
    return w;
}

void setBackground(uint32_t rgb) {
    if (rgb == gBackground) return;
    gBackground = rgb;
    invalidateAll();
}
void onBackgroundClick(std::function<void()> cb) { gBackgroundClick = std::move(cb); }

void invalidate(const Rect& r0) {
    Rect r = r0;
    int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    int x1 = std::min(kScreenW, r.x + r.w), y1 = std::min(kScreenH, r.y + r.h);
    if (x1 <= x0 || y1 <= y0) return;
    r = {x0 & ~1, y0 & ~1, 0, 0};
    r.w = ((x1 - 1) | 1) + 1 - r.x;  // even start, odd end - see the header
    r.h = ((y1 - 1) | 1) + 1 - r.y;
    r.w = std::min(r.w, kScreenW - r.x);
    r.h = std::min(r.h, kScreenH - r.y);
    for (Rect& d : gDirty)
        if (intersects(d, r)) {
            d = unite(d, r);
            return;
        }
    if (gDirty.size() >= 12) {  // too fragmented: one big repaint is cheaper than many small ones
        Rect all = r;
        for (const Rect& d : gDirty) all = unite(all, d);
        gDirty.assign(1, all);
        return;
    }
    gDirty.push_back(r);
}
void invalidateAll() { invalidate({0, 0, kScreenW, kScreenH}); }

void refresh() {
    std::vector<Rect> todo;
    todo.swap(gDirty);
    for (const Rect& r : todo) renderRect(r);
}

void setTouchSource(std::function<bool(int*, int*)> src) { gTouchSource = std::move(src); }

void update(uint32_t nowMs) {
    int x, y;
    if (gTouchSource && gTouchSource(&x, &y)) touch(x, y, nowMs);
    service(nowMs);
    refresh();
}

void touch(int x, int y, uint32_t nowMs) {
    gLastReportMs = nowMs;
    gEverTouched = true;
    if (gDown) return;
    gDown = true;
    Widget* hit = hitTest(x, y);
    if (hit) {
        if (hit->disabled) return;
        gPressed = hit;
        hit->setPressed(true);
        refresh();  // show the touch before a handler that blocks (restart, ...)
        if (hit->onClick) hit->onClick();
    } else if (gBackgroundClick) {
        gBackgroundClick();
    }
}

void service(uint32_t nowMs) {
    if (gDown && nowMs - gLastReportMs > kReleaseMs) {
        gDown = false;
        if (gPressed) {
            gPressed->setPressed(false);
            gPressed = nullptr;
        }
    }
}

uint32_t idleMs(uint32_t nowMs) { return gEverTouched ? nowMs - gLastReportMs : nowMs; }

// ---- text metrics --------------------------------------------------------------

int ascent(const uint8_t* font) { return fontAscent(font); }
int lineHeight(const uint8_t* font) { return fontAscent(font) + fontDescent(font) + 6; }

int textWidth(const uint8_t* font, const std::string& line) {
    if (line.empty()) return 0;
    gCanvas->setFont(font);
    gCanvas->setUTF8Print(true);
    int16_t x1, y1;
    uint16_t w, h;
    gCanvas->getTextBounds(line.c_str(), 0, 100, &x1, &y1, &w, &h);
    return w;
}

std::string wrap(const uint8_t* font, const std::string& s, int maxW) {
    std::string out;
    bool firstPara = true;
    for (const std::string& para : splitLines(s)) {
        if (!firstPara) out += '\n';
        firstPara = false;
        std::string line;
        size_t pos = 0;
        while (pos <= para.size()) {
            size_t sp = para.find(' ', pos);
            std::string word = para.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
            std::string trial = line.empty() ? word : line + " " + word;
            if (!line.empty() && textWidth(font, trial) > maxW) {
                out += line + "\n";
                line = word;
            } else {
                line = trial;
            }
            if (sp == std::string::npos) break;
            pos = sp + 1;
        }
        out += line;
    }
    return out;
}

int lineCount(const std::string& s) { return 1 + (int)std::count(s.begin(), s.end(), '\n'); }

}  // namespace mui
