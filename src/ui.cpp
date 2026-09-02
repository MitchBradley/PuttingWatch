#include "ui.h"
#include <Arduino.h>
#include <lvgl.h>
#include <cstdio>
#include <cstring>
#include "pin_config.h"

namespace ui {

std::function<void()> onTap;
std::function<void()> onRecal;
std::function<void()> onStop;
std::function<void(int)> onAdjustClickThreshold;

namespace {

lv_obj_t* root = nullptr;
lv_obj_t* statusLabel = nullptr;
lv_obj_t* bigLabel = nullptr;   // swing angle in Monitoring, rest xyz in Done
lv_obj_t* threshLabel = nullptr;
lv_obj_t* recalBtn = nullptr;
lv_obj_t* stopBtn = nullptr;
lv_obj_t* minusBtn = nullptr;
lv_obj_t* plusBtn = nullptr;

// Background colours, matching ContentView.swift's palette intent.
constexpr uint32_t kBgWaiting = 0x8C8026;   // muted olive
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

    // Recal / Stop across the TOP.
    recalBtn = makeButton("Recal", recalClicked, LV_ALIGN_TOP_MID, -88, 16, 150, 88,
                          &lv_font_montserrat_28);
    stopBtn = makeButton("Stop", stopClicked, LV_ALIGN_TOP_MID, 88, 16, 150, 88,
                         &lv_font_montserrat_28);

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
    lv_obj_set_size(statusLabel, LCD_WIDTH - 40, 76);
    lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(statusLabel, LV_ALIGN_CENTER, 0, -110);

    bigLabel = lv_label_create(root);
    lv_obj_set_style_text_font(bigLabel, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(bigLabel, lv_color_white(), 0);
    lv_obj_set_size(bigLabel, LCD_WIDTH, 70);
    lv_obj_set_style_text_align(bigLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(bigLabel, LV_ALIGN_CENTER, 0, -25);
    lv_label_set_text(bigLabel, "");

    threshLabel = lv_label_create(root);
    lv_obj_set_style_text_font(threshLabel, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(threshLabel, lv_color_white(), 0);
    lv_obj_set_size(threshLabel, LCD_WIDTH, 64);
    lv_obj_set_style_text_align(threshLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(threshLabel, LV_ALIGN_CENTER, 0, 60);
}

// render() is called ~20x/s. It MUST only touch an LVGL object when that
// object's content actually changed - any unconditional setter (especially
// on `root`) re-invalidates the whole 410x502 screen every frame, which
// software-renders + QSPI-pushes the entire panel over and over and drags
// the whole UI (including touch) to a crawl. Everything below diffs first.
void render(const Snapshot& s) {
    using State = SwingTracker::State;

    static char lastStatus[48] = "\x01";
    static char lastBig[48] = "\x01";
    static char lastThresh[48] = "\x01";
    static uint32_t lastBg = 0xFFFFFFFFu;
    static int lastVis = -1;

    char status[48] = "";
    char big[48] = "";
    char thresh[48] = "";
    uint32_t bg = kBgIdleDark;
    bool showBig = false, showThresh = false, showTopBtns = false, showNudge = false;

    switch (s.state) {
        case State::WaitingForCalibration:
            bg = kBgWaiting;
            snprintf(status, sizeof(status), "%s",
                     s.imuOk ? "Tap when in putting position" : "IMU not found - check wiring");
            break;

        case State::Accumulating:
            bg = kBgAccum;
            snprintf(status, sizeof(status), "Hold still...");
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

    int vis = (showBig ? 1 : 0) | (showThresh ? 2 : 0) | (showTopBtns ? 4 : 0) | (showNudge ? 8 : 0);
    if (vis != lastVis) {
        lastVis = vis;
        setHidden(bigLabel, !showBig);
        setHidden(threshLabel, !showThresh);
        setHidden(recalBtn, !showTopBtns);
        setHidden(stopBtn, !showTopBtns);
        setHidden(minusBtn, !showNudge);
        setHidden(plusBtn, !showNudge);
    }
}

}  // namespace ui
