#include "ui.h"
#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include "MiniUi.h"
#include "app_config.h"
#include "honu_img.h"
#include "pin_config.h"
#include "ui_fonts.h"
#include "update_config.h"

namespace ui {

std::function<void()> onTap;
std::function<void()> onRecal;
std::function<void()> onStop;
std::function<void(int)> onAdjustClickThreshold;
std::function<void()> onUpdate;

namespace {

using mui::Align;
using mui::Kind;
using mui::Rect;
using mui::Widget;

// Fonts, named by the size LVGL's Montserrat used to have at each spot. The u8g2
// fonts are 1-bit; logisoso for the big/bold text, Helvetica for small text.
const uint8_t* const kFont48 = u8g2_font_logisoso34_tf;
const uint8_t* const kFont32 = u8g2_font_logisoso24_tf;
const uint8_t* const kFont28 = u8g2_font_logisoso22_tf;
const uint8_t* const kFont24 = u8g2_font_logisoso18_tf;
const uint8_t* const kFont18 = u8g2_font_helvR14_tf;

Widget* honuImg = nullptr;        // waiting screen ("Tap ...")
Widget* honuBottomImg = nullptr;  // "Hold still ..." screen
Widget* waitLabel = nullptr;      // black text above the image, both screens
Widget* statusLabel = nullptr;
Widget* bigLabel = nullptr;   // swing angle in Monitoring, rest xyz in Done
Widget* threshLabel = nullptr;
Widget* recalBtn = nullptr;
Widget* stopBtn = nullptr;
Widget* battBar = nullptr;
Widget* battLabel = nullptr;
Widget* minusBtn = nullptr;
Widget* plusBtn = nullptr;
Widget* updateBtn = nullptr;      // Bye screen only: reboot into firmware-update mode
Widget* versionLabel = nullptr;   // waiting screen only: firmware version, along the bottom
Widget* byeLabel = nullptr;
Widget* helpBtn = nullptr;        // Bye screen only: QR code for the guide
Widget* honuBtn = nullptr;        // Bye screen only: QR code for honugolf.com
Widget* qrTitle = nullptr;        // QR page: what the code is for
Widget* qrBox = nullptr;          // QR page: white quiet-zone square
Widget* qrCode = nullptr;
Widget* qrHint = nullptr;         // QR page: how to leave
bool qrShowing = false;
uint32_t qrShownMs = 0;
constexpr uint32_t kQrTimeoutMs = 60000;  // back to Bye if nobody dismisses it

// Background colours, matching ContentView.swift's palette intent.
constexpr uint32_t kBgWaiting = 0xFFFFFF;   // white - behind the Honu image
constexpr uint32_t kBgAccum = 0xE6E600;     // bright yellow "hold still"
constexpr uint32_t kBgIdleDark = 0x000000;  // black
constexpr uint32_t kBgReady = 0x005A0D;     // clear green = armed

constexpr uint32_t kBtnBlue = 0x2196F3;         // the run-screen buttons
constexpr uint32_t kBtnBluePressed = 0x1565C0;
constexpr uint32_t kBtnGrey = 0x3A3A3A;         // the Bye-screen buttons
constexpr uint32_t kBtnGreen = 0x1B7F3B;        // ... while touched

// Battery row Y position: on Monitoring/Done it sits just below the top
// Recal/Stop buttons; on the initial "Tap when in putting position" screen
// (no buttons there) it's shifted up by its own text height to clear the
// Honu image, which starts close beneath it.
constexpr int kBattYNormal  = 112;
constexpr int kBattTextH    = 34;
constexpr int kBattYWaiting = kBattYNormal - kBattTextH + 11;
constexpr int kBattLabelX   = (mui::kScreenW - 180) / 2 - 60;
constexpr int kBattBarW = 108, kBattBarH = 20;

void updateClicked() {
    // Restarting into update mode takes a moment, long enough that the user
    // can't tell whether the touch registered. Say so, on screen, first: turn
    // the button green (it already is while held), relabel it, and force that
    // frame out NOW, because the restart below starts before the normal ~20 Hz
    // render would draw it.
    updateBtn->setBg(kBtnGreen);
    updateBtn->setText("Starting...");
    mui::refresh();
    if (onUpdate) onUpdate();
}

void setHidden(Widget* w, bool hidden) { w->show(!hidden); }

void showQr(const char* title, const char* url) {
    setHidden(byeLabel, true);
    setHidden(helpBtn, true);
    setHidden(honuBtn, true);
    setHidden(updateBtn, true);
    qrTitle->setText(title);
    qrCode->setQr(url);
    setHidden(qrTitle, false);
    setHidden(qrBox, false);
    setHidden(qrCode, false);
    setHidden(qrHint, false);
    qrShowing = true;
    qrShownMs = millis();
}

// A fixed-size label box: the text is centred (or aligned) inside it and the
// box never changes size, so repainting it covers exactly the same pixels.
Widget* makeLabel(const uint8_t* font, Rect r, uint32_t color, Align align = Align::Center) {
    Widget* l = mui::add(Kind::Label, r);
    l->font = font;
    l->fg = color;
    l->align = align;
    l->hidden = true;  // first render() picks what shows
    return l;
}

// Big fingertip target: fixed size + a generous invisible margin that still
// registers a press.
Widget* makeButton(const char* text, std::function<void()> cb, Rect r, const uint8_t* font,
                   uint32_t bg, uint32_t bgPressed) {
    Widget* b = mui::add(Kind::Button, r);
    b->text = text;
    b->font = font;
    b->bg = bg;
    b->bgPressed = bgPressed;
    b->radius = 18;
    b->ext = 24;
    b->onClick = std::move(cb);
    return b;
}

// x of a box `w` wide, centred on the screen and then moved by `dx`.
constexpr int centeredX(int w, int dx = 0) { return (mui::kScreenW - w) / 2 + dx; }
constexpr int centeredY(int h, int dy = 0) { return (mui::kScreenH - h) / 2 + dy; }

}  // namespace

void init(const char* firmwareVersion) {
    mui::setBackground(kBgWaiting);
    mui::onBackgroundClick([] {
        if (onTap) onTap();
    });

    // Waiting screen: the Honu image flush to the bottom on white, with the
    // prompt in the ~122 px band above it, kept well inside the rounded
    // corners. Shown only in WaitingForCalibration.
    honuImg = mui::add(Kind::Image, {0, mui::kScreenH - honu_img.h, honu_img.w, honu_img.h});
    honuImg->image = &honu_img;
    honuImg->hidden = true;
    honuBottomImg =
        mui::add(Kind::Image, {0, mui::kScreenH - honu_bottom_img.h, honu_bottom_img.w, honu_bottom_img.h});
    honuBottomImg->image = &honu_bottom_img;
    honuBottomImg->hidden = true;  // first render picks the image

    waitLabel = makeLabel(kFont32, {40, 12, mui::kScreenW - 80, 2 * mui::lineHeight(kFont32)}, 0x000000);

    // Recal / Stop across the TOP.
    recalBtn = makeButton("Recal", [] { if (onRecal) onRecal(); },
                          {centeredX(150, -88), 12, 150, 82}, kFont28, kBtnBlue, kBtnBluePressed);
    stopBtn = makeButton("Stop", [] { if (onStop) onStop(); },
                         {centeredX(150, 88), 12, 150, 82}, kFont28, kBtnBlue, kBtnBluePressed);

    // Battery: "NN% NNNN mV" with the bar to its right, just below the top
    // buttons. Shown only when the top buttons are (Monitoring/Done), and on
    // the waiting screen. The label gets a FIXED box with right-aligned text
    // (snug against the bar).
    battLabel = makeLabel(kFont24, {kBattLabelX, kBattYNormal, 180, kBattTextH}, 0xFFFFFF, Align::Right);
    battBar = mui::add(Kind::Bar, {kBattLabelX + 180 + 12, kBattYNormal + (kBattTextH - kBattBarH) / 2,
                                   kBattBarW, kBattBarH});
    battBar->bg = 0x202020;
    battBar->fg = 0x33C04D;
    battBar->hidden = true;

    // Click-interval - / + across the BOTTOM. Start hidden so an early touch
    // (before the first render sets visibility) can't click them.
    minusBtn = makeButton("-", [] { if (onAdjustClickThreshold) onAdjustClickThreshold(-1); },
                          {centeredX(140, -88), mui::kScreenH - 104 - 16, 140, 104}, kFont48,
                          kBtnBlue, kBtnBluePressed);
    plusBtn = makeButton("+", [] { if (onAdjustClickThreshold) onAdjustClickThreshold(+1); },
                         {centeredX(140, 88), mui::kScreenH - 104 - 16, 140, 104}, kFont48, kBtnBlue,
                         kBtnBluePressed);
    minusBtn->hidden = plusBtn->hidden = true;
    recalBtn->hidden = stopBtn->hidden = true;

    // "Help" / "Honu": on the Bye screen only, across the top like Recal/Stop
    // (which are hidden there). Each shows a QR code page - see showQr().
    helpBtn = makeButton("Help", [] { showQr("Help", HELP_URL); }, {centeredX(150, -88), 12, 150, 82},
                         kFont28, kBtnGrey, kBtnGreen);
    honuBtn = makeButton("Honu", [] { showQr("Honu", HONU_URL); }, {centeredX(150, 88), 12, 150, 82},
                         kFont28, kBtnGrey, kBtnGreen);
    helpBtn->hidden = honuBtn->hidden = true;

    // The QR page both buttons share: title, QR on a white square (QR codes
    // need a light quiet zone around them on a dark screen), and how to leave.
    qrTitle = makeLabel(kFont32, {0, 30, mui::kScreenW, mui::lineHeight(kFont32)}, 0xFFFFFF);
    qrBox = mui::add(Kind::Box, {centeredX(280), centeredY(280, 10), 280, 280});
    qrBox->bg = 0xFFFFFF;
    qrBox->hasBg = true;
    qrBox->hidden = true;
    qrCode = mui::add(Kind::Qr, {centeredX(252), centeredY(252, 10), 252, 252});
    qrCode->fg = 0x000000;
    qrCode->hidden = true;
    qrHint = makeLabel(kFont18, {0, mui::kScreenH - 34 - mui::lineHeight(kFont18), mui::kScreenW,
                                 mui::lineHeight(kFont18)}, 0x909090);
    qrHint->text = "Upper button to go back";

    // "Update": on the Bye screen only, below the "Bye" text. Not on the "Tap
    // when in putting position" screen, where it was too easy to hit by
    // accident (that tap usually happens with the watch out of sight). Hidden
    // until showByeScreen().
    updateBtn = makeButton("Update", updateClicked, {centeredX(240), centeredY(76, 110), 240, 76}, kFont32,
                           kBtnGrey, kBtnGreen);
    updateBtn->hidden = true;

    // Firmware version along the bottom of the waiting screen, over the empty
    // lower-right of the Honu image, starting at 70% of the screen width.
    // Static text, set once. Starts hidden so it can't flash on other screens
    // before the first render picks one. The panel has large rounded corners,
    // so keep it well off them. A long development version string
    // ("4bd9850-dirty") runs further right than a release's "v0.1.1".
    char ver[40];
    // Release builds are "0.1.0" -> "v0.1.0"; development builds ("4bd9850-dirty") as-is.
    snprintf(ver, sizeof(ver), "%s%s", (firmwareVersion && firmwareVersion[0] >= '0' && firmwareVersion[0] <= '9') ? "v" : "",
             firmwareVersion ? firmwareVersion : "");
    const int vx = mui::kScreenW * 70 / 100;
    versionLabel = makeLabel(kFont18, {vx, mui::kScreenH - 36 - mui::lineHeight(kFont18),
                                       mui::kScreenW - vx, mui::lineHeight(kFont18)}, 0x707070, Align::Left);
    versionLabel->text = ver;

    // Every dynamic label gets a FIXED box (fixed position and size) with the
    // text centred inside it - see the note in render().
    statusLabel = makeLabel(kFont28, {20, centeredY(72, -60), mui::kScreenW - 40, 72}, 0xFFFFFF);
    bigLabel = makeLabel(kFont48, {0, centeredY(70, 4), mui::kScreenW, 70}, 0xFFFFFF);
    threshLabel = makeLabel(kFont48, {0, centeredY(64, 74), mui::kScreenW, 64}, 0xFFFFFF);
    byeLabel = makeLabel(kFont48, {0, centeredY(70), mui::kScreenW, 70}, 0xFFFFFF);
    byeLabel->text = "Bye";
    mui::invalidateAll();
}

void showByeScreen() {
    mui::setBackground(kBgIdleDark);
    setHidden(honuImg, true);
    setHidden(honuBottomImg, true);
    setHidden(waitLabel, true);
    setHidden(statusLabel, true);
    setHidden(bigLabel, true);
    setHidden(threshLabel, true);
    setHidden(recalBtn, true);
    setHidden(stopBtn, true);
    setHidden(battBar, true);
    setHidden(battLabel, true);
    setHidden(minusBtn, true);
    setHidden(plusBtn, true);
    setHidden(versionLabel, true);
    setHidden(qrTitle, true);
    setHidden(qrBox, true);
    setHidden(qrCode, true);
    setHidden(qrHint, true);
    qrShowing = false;
    setHidden(byeLabel, false);
    setHidden(helpBtn, false);
    setHidden(honuBtn, false);
    setHidden(updateBtn, false);
    mui::refresh();  // force the flush now - normal render() cadence
                     // (~20 Hz) may not get another chance to run
}

bool byeQrActive() { return qrShowing; }

void dismissByeQr() {
    if (qrShowing) showByeScreen();
}

void byeTick() {
    if (qrShowing && millis() - qrShownMs >= kQrTimeoutMs) dismissByeQr();
}

void update() { mui::update(millis()); }
void invalidateAll() { mui::invalidateAll(); }
uint32_t idleMs() { return mui::idleMs(millis()); }

// render() is called ~20x/s. The widget setters only repaint when a value
// really changed, but the diffs below also keep the string formatting and
// the visibility bookkeeping off the hot path.
void render(const Snapshot& s) {
    using State = SwingTracker::State;

    static char lastStatus[48] = "\x01";
    static char lastWait[48] = "\x01";
    static char lastBig[48] = "\x01";
    static char lastThresh[48] = "\x01";
    static uint32_t lastBg = 0xFFFFFFFFu;
    static int lastVis = -1;
    static int lastBatt = -2;
    static int lastBattMv = -2;
    static int lastBattOnWaitScreen = -1;

    char status[48] = "";
    char wait[48] = "";
    char big[48] = "";
    char thresh[48] = "";
    uint32_t bg = kBgIdleDark;
    bool showBig = false, showThresh = false, showTopBtns = false, showNudge = false;
    bool showWait = false;    // image + top-band text layout
    bool showBottom = false;  // which image: false = Honu (tap), true = HonuBottom (hold still)

    switch (s.state) {
        case State::WaitingForCalibration:
            bg = kBgWaiting;
            snprintf(wait, sizeof(wait), "%s",
                     s.imuOk ? "Tap when in\nputting position" : "IMU not found\ncheck wiring");
            showWait = true;
            break;

        case State::Accumulating:
            // Same top-band layout as the waiting screen, yellow background,
            // the HonuBottom image.
            bg = kBgAccum;
            snprintf(wait, sizeof(wait), "Hold still...");
            showWait = showBottom = true;
            break;

        case State::Done:
#if SHOW_REST_CAPTURED_SCREEN
            // Debug screen: averaged rest-gravity vector, held for
            // SwingTracker::Config::monitoringDelayMs before Monitoring.
            bg = kBgIdleDark;
            snprintf(status, sizeof(status), "Rest captured");
            snprintf(big, sizeof(big), "%+.2f %+.2f %+.2f", s.restG[0], s.restG[1], s.restG[2]);
            showBig = showTopBtns = true;
            break;
#else
            // Screen compiled out for production. Done is a single-tick
            // pass-through to Monitoring (monitoringDelayMs == 0), so render it
            // as Monitoring in case the view samples state during that tick.
            [[fallthrough]];
#endif

        case State::Monitoring:
            bg = s.ready ? kBgReady : kBgIdleDark;
            snprintf(status, sizeof(status), "%s", s.ready ? "Ready" : "");
            snprintf(big, sizeof(big), "%.1f\xC2\xB0", s.angleDeg);
            snprintf(thresh, sizeof(thresh), "click  %d", s.clickLevel);
            showBig = showThresh = showTopBtns = showNudge = true;
            break;
    }

    // Battery shows on the top-button screens (Monitoring/Done) AND the
    // initial "Tap when in putting position" screen - the one place besides
    // Monitoring a golfer is likely to check it before starting. There it's
    // black text (white background, instead of the usual white) and shifted
    // up (kBattYWaiting) to clear the Honu image just beneath it.
    bool showBatt = showTopBtns || s.state == State::WaitingForCalibration;
    bool battOnWaitScreen = s.state == State::WaitingForCalibration;
    bool showVersion = showWait && !showBottom;  // the waiting screen only, not "Hold still"

    if (strcmp(status, lastStatus)) {
        strcpy(lastStatus, status);
        statusLabel->setText(status);
    }
    if (strcmp(wait, lastWait)) {
        strcpy(lastWait, wait);
        waitLabel->setText(wait);
    }
    if (strcmp(big, lastBig)) {
        strcpy(lastBig, big);
        bigLabel->setText(big);
    }
    if (strcmp(thresh, lastThresh)) {
        strcpy(lastThresh, thresh);
        threshLabel->setText(thresh);
    }
    if (bg != lastBg) {
        lastBg = bg;
        mui::setBackground(bg);
    }
    if (showBatt && (s.batteryPct != lastBatt || s.batteryMv != lastBattMv)) {
        lastBatt = s.batteryPct;
        lastBattMv = s.batteryMv;
        int pct = s.batteryPct;
        battBar->setValue(pct < 0 ? 0 : pct);
        battBar->setFg(pct >= 0 && pct <= 20 ? 0xE03B24 : 0x33C04D);
        char b[20];
        if (pct < 0)
            snprintf(b, sizeof(b), "--");
        else
            snprintf(b, sizeof(b), "%d%% %d mV", pct, s.batteryMv);
        battLabel->setText(b);
    }
    if ((int)battOnWaitScreen != lastBattOnWaitScreen) {
        lastBattOnWaitScreen = (int)battOnWaitScreen;
        const int y = battOnWaitScreen ? kBattYWaiting : kBattYNormal;
        battLabel->setFg(battOnWaitScreen ? 0x000000 : 0xFFFFFF);
        battLabel->setRect({kBattLabelX, y, 180, kBattTextH});
        battBar->setBg(battOnWaitScreen ? 0xD6D6D6 : 0x202020);
        battBar->setRect({kBattLabelX + 180 + 12, y + (kBattTextH - kBattBarH) / 2, kBattBarW, kBattBarH});
    }

    int vis = (showBig ? 1 : 0) | (showThresh ? 2 : 0) | (showTopBtns ? 4 : 0) |
              (showNudge ? 8 : 0) | (showWait ? 16 : 0) | (showBottom ? 32 : 0) |
              (showBatt ? 64 : 0) | (showVersion ? 128 : 0);
    if (vis != lastVis) {
        lastVis = vis;
        setHidden(honuImg, !showWait || showBottom);
        setHidden(honuBottomImg, !showWait || !showBottom);
        setHidden(waitLabel, !showWait);
        setHidden(statusLabel, showWait);
        setHidden(bigLabel, !showBig);
        setHidden(threshLabel, !showThresh);
        setHidden(recalBtn, !showTopBtns);
        setHidden(stopBtn, !showTopBtns);
        setHidden(battBar, !showBatt);
        setHidden(battLabel, !showBatt);
        setHidden(minusBtn, !showNudge);
        setHidden(plusBtn, !showNudge);
        setHidden(versionLabel, !showVersion);
    }
}

}  // namespace ui
