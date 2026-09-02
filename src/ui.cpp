#include "ui.h"
#include <Arduino.h>
#include <lvgl.h>
#include <cstdio>
#include <cstring>
#include "pin_config.h"
#include "honu_img.h"

namespace ui {

std::function<void()> onTap;
std::function<void()> onRecal;
std::function<void()> onStop;
std::function<void(int)> onAdjustClickThreshold;

namespace {

lv_obj_t* root = nullptr;
lv_obj_t* honuImg = nullptr;     // waiting-screen background image
lv_obj_t* waitLabel = nullptr;   // "Tap ..." text, black, above the image
lv_obj_t* statusLabel = nullptr;
lv_obj_t* bigLabel = nullptr;   // swing angle in Monitoring, rest xyz in Done
lv_obj_t* threshLabel = nullptr;
lv_obj_t* recalBtn = nullptr;
lv_obj_t* stopBtn = nullptr;
lv_obj_t* battBar = nullptr;
lv_obj_t* battLabel = nullptr;
lv_obj_t* minusBtn = nullptr;
lv_obj_t* plusBtn = nullptr;

// Background colours, matching ContentView.swift's palette intent.
constexpr uint32_t kBgWaiting = 0xFFFFFF;   // white - behind the Honu image
constexpr uint32_t kBgAccum = 0xE6E600;     // bright yellow "hold still"
constexpr uint32_t kBgIdleDark = 0x000000;  // black
constexpr uint32_t kBgReady = 0x005A0D;     // clear green = armed

void rootClicked(lv_event_t*) {
    if (onTap) onTap();
}
void recalClicked(lv_event_t*) {
    if (onRecal) onRecal();
}
void stopClicked(lv_event_t*) {
    if (onStop) onStop();
}
void minusClicked(lv_event_t*) {
    if (onAdjustClickThreshold) onAdjustClickThreshold(-1);
}
void plusClicked(lv_event_t*) {
    if (onAdjustClickThreshold) onAdjustClickThreshold(+1);
}

void setHidden(lv_obj_t* o, bool hidden) {
    if (hidden)
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Big fingertip target: fixed size + a generous invisible margin that still
// registers a press. `font` is the label font (montserrat 28 or 48).
lv_obj_t* makeButton(const char* text, lv_event_cb_t cb, lv_align_t align, int dx, int dy,
                     int w, int h, const lv_font_t* font) {
    lv_obj_t* b = lv_button_create(root);
    lv_obj_set_size(b, w, h);
    lv_obj_set_ext_click_area(b, 24);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_align(b, align, dx, dy);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

}  // namespace

void init() {
    root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(kBgWaiting), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, rootClicked, LV_EVENT_CLICKED, nullptr);

    // Waiting screen: the Honu image centred vertically on white, with the
    // prompt in the band above it. Shown only in WaitingForCalibration.
    // Image flush to the bottom; the ~122 px of white it leaves is all at the
    // top, and that's where the prompt goes (two explicit lines, kept well
    // inside the rounded corners).
    honuImg = lv_image_create(root);
    lv_image_set_src(honuImg, &honu_img);  // 410x380
    lv_obj_align(honuImg, LV_ALIGN_BOTTOM_MID, 0, 0);

    waitLabel = lv_label_create(root);
    lv_obj_set_style_text_font(waitLabel, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(waitLabel, lv_color_black(), 0);
    lv_obj_set_width(waitLabel, LCD_WIDTH - 80);
    lv_obj_set_style_text_align(waitLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(waitLabel, LV_ALIGN_TOP_MID, 0, 18);

    // Recal / Stop across the TOP.
    recalBtn = makeButton("Recal", recalClicked, LV_ALIGN_TOP_MID, -88, 12, 150, 82,
                          &lv_font_montserrat_28);
    stopBtn = makeButton("Stop", stopClicked, LV_ALIGN_TOP_MID, 88, 12, 150, 82,
                         &lv_font_montserrat_28);

    // Battery: a plain bar + a "NN%" number to its right, just below the top
    // buttons (no label). Shown only when the top buttons are (Monitoring/Done).
    battBar = lv_bar_create(root);
    lv_obj_set_size(battBar, 132, 20);
    lv_bar_set_range(battBar, 0, 100);
    lv_obj_align(battBar, LV_ALIGN_TOP_MID, -46, 104);
    lv_obj_set_style_bg_color(battBar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(battBar, lv_color_hex(0x33C04D), LV_PART_INDICATOR);

    battLabel = lv_label_create(root);
    lv_obj_set_style_text_font(battLabel, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(battLabel, lv_color_white(), 0);
    lv_obj_align_to(battLabel, battBar, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

    // Click-interval - / + across the BOTTOM (no slider).
    minusBtn = makeButton("-", minusClicked, LV_ALIGN_BOTTOM_MID, -88, -16, 140, 104,
                          &lv_font_montserrat_48);
    plusBtn = makeButton("+", plusClicked, LV_ALIGN_BOTTOM_MID, 88, -16, 140, 104,
                         &lv_font_montserrat_48);

    // Every dynamic label gets a FIXED box (full width, fixed height, fixed
    // position) with the text centred inside it - see the note in render():
    // an auto-sized label re-invalidates a moving region and tears.
    statusLabel = lv_label_create(root);
    lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(statusLabel, lv_color_white(), 0);
    lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(statusLabel, LCD_WIDTH - 40, 72);
    lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0);
    // Dropped from -110 to -60: in Monitoring "Ready" sat jammed against the
    // top buttons; now it sits just above the angle readout.
    lv_obj_align(statusLabel, LV_ALIGN_CENTER, 0, -60);

    bigLabel = lv_label_create(root);
    lv_obj_set_style_text_font(bigLabel, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(bigLabel, lv_color_white(), 0);
    lv_obj_set_size(bigLabel, LCD_WIDTH, 70);
    lv_obj_set_style_text_align(bigLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(bigLabel, LV_ALIGN_CENTER, 0, 4);
    lv_label_set_text(bigLabel, "");

    threshLabel = lv_label_create(root);
    lv_obj_set_style_text_font(threshLabel, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(threshLabel, lv_color_white(), 0);
    lv_obj_set_size(threshLabel, LCD_WIDTH, 64);
    lv_obj_set_style_text_align(threshLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(threshLabel, LV_ALIGN_CENTER, 0, 74);
}

// render() is called ~20x/s. It MUST only touch an LVGL object when that
// object's content actually changed - any unconditional setter (especially
// on `root`) re-invalidates the whole 410x502 screen every frame, which
// software-renders + QSPI-pushes the entire panel over and over and drags
// the whole UI (including touch) to a crawl. Everything below diffs first.
void render(const Snapshot& s) {
    using State = SwingTracker::State;

    static char lastStatus[48] = "\x01";
    static char lastWait[48] = "\x01";
    static char lastBig[48] = "\x01";
    static char lastThresh[48] = "\x01";
    static uint32_t lastBg = 0xFFFFFFFFu;
    static int lastVis = -1;
    static int lastBatt = -2;

    char status[48] = "";
    char wait[48] = "";
    char big[48] = "";
    char thresh[48] = "";
    uint32_t bg = kBgIdleDark;
    bool showBig = false, showThresh = false, showTopBtns = false, showNudge = false;
    bool showWait = false;

    switch (s.state) {
        case State::WaitingForCalibration:
            bg = kBgWaiting;
            snprintf(wait, sizeof(wait), "%s",
                     s.imuOk ? "Tap when in\nputting position" : "IMU not found\ncheck wiring");
            showWait = true;
            break;

        case State::Accumulating:
            // Same image + top-band layout as the waiting screen, just a
            // yellow background instead of white.
            bg = kBgAccum;
            snprintf(wait, sizeof(wait), "Hold still...");
            showWait = true;
            break;

        case State::Done:
            bg = kBgIdleDark;
            snprintf(status, sizeof(status), "Rest captured");
            snprintf(big, sizeof(big), "%+.2f %+.2f %+.2f", s.restG[0], s.restG[1], s.restG[2]);
            showBig = showTopBtns = true;
            break;

        case State::Monitoring:
            bg = s.ready ? kBgReady : kBgIdleDark;
            snprintf(status, sizeof(status), "%s", s.ready ? "Ready" : "");
            snprintf(big, sizeof(big), "%.1f\xC2\xB0", s.angleDeg);
            snprintf(thresh, sizeof(thresh), "click  %.0f\xC2\xB0", s.clickThresholdDeg);
            showBig = showThresh = showTopBtns = showNudge = true;
            break;
    }

    if (strcmp(status, lastStatus)) {
        strcpy(lastStatus, status);
        lv_label_set_text(statusLabel, status);
    }
    if (strcmp(wait, lastWait)) {
        strcpy(lastWait, wait);
        lv_label_set_text(waitLabel, wait);
    }
    if (strcmp(big, lastBig)) {
        strcpy(lastBig, big);
        lv_label_set_text(bigLabel, big);
    }
    if (strcmp(thresh, lastThresh)) {
        strcpy(lastThresh, thresh);
        lv_label_set_text(threshLabel, thresh);
    }
    if (bg != lastBg) {
        lastBg = bg;
        lv_obj_set_style_bg_color(root, lv_color_hex(bg), LV_PART_MAIN);
    }
    if (showTopBtns && s.batteryPct != lastBatt) {
        lastBatt = s.batteryPct;
        int pct = s.batteryPct;
        lv_bar_set_value(battBar, pct < 0 ? 0 : pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(battBar,
                                  lv_color_hex(pct >= 0 && pct <= 20 ? 0xE03B24 : 0x33C04D),
                                  LV_PART_INDICATOR);
        char b[8];
        snprintf(b, sizeof(b), pct < 0 ? "--" : "%d%%", pct);
        lv_label_set_text(battLabel, b);
    }

    int vis = (showBig ? 1 : 0) | (showThresh ? 2 : 0) | (showTopBtns ? 4 : 0) |
              (showNudge ? 8 : 0) | (showWait ? 16 : 0);
    if (vis != lastVis) {
        lastVis = vis;
        setHidden(honuImg, !showWait);
        setHidden(waitLabel, !showWait);
        setHidden(statusLabel, showWait);
        setHidden(bigLabel, !showBig);
        setHidden(threshLabel, !showThresh);
        setHidden(recalBtn, !showTopBtns);
        setHidden(stopBtn, !showTopBtns);
        setHidden(battBar, !showTopBtns);
        setHidden(battLabel, !showTopBtns);
        setHidden(minusBtn, !showNudge);
        setHidden(plusBtn, !showNudge);
    }
}

}  // namespace ui
