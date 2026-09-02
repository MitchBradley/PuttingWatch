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
std::function<void(float)> onSetClickThreshold;

namespace {

lv_obj_t* root = nullptr;
lv_obj_t* statusLabel = nullptr;
lv_obj_t* bigLabel = nullptr;     // swing angle in Monitoring, "Hold still" etc otherwise
lv_obj_t* threshLabel = nullptr;
lv_obj_t* threshSlider = nullptr;
lv_obj_t* recalBtn = nullptr;
lv_obj_t* stopBtn = nullptr;

// Background colours, matching ContentView.swift's palette intent.
constexpr uint32_t kBgWaiting = 0x8C8026;    // muted olive
constexpr uint32_t kBgAccum = 0xE6E600;      // bright yellow "hold still"
constexpr uint32_t kBgIdleDark = 0x000000;   // black
constexpr uint32_t kBgReady = 0x005A0D;      // clear green = armed

void rootClicked(lv_event_t*) {
    if (onTap) onTap();
}
void recalClicked(lv_event_t*) {
    if (onRecal) onRecal();
}
void stopClicked(lv_event_t*) {
    if (onStop) onStop();
}
void threshChanged(lv_event_t* e) {
    auto* s = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (onSetClickThreshold) onSetClickThreshold((float)lv_slider_get_value(s));
}

void setHidden(lv_obj_t* o, bool hidden) {
    if (hidden)
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* makeButton(const char* text, lv_event_cb_t cb, lv_align_t align, int dx, int dy) {
    lv_obj_t* b = lv_button_create(root);
    // Big touch target: this is a 410x502 round panel driven by a fingertip,
    // so the default LVGL button is far too small. ~150x96 px + a generous
    // invisible margin around it that still counts as a press.
    lv_obj_set_size(b, 150, 96);
    lv_obj_set_ext_click_area(b, 24);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_align(b, align, dx, dy);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
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

    // Every dynamic label gets a FIXED box (full width, fixed height, fixed
    // position) with the text centred inside it. If a label is left to
    // auto-size, each text change resizes and re-centres the object, so LVGL
    // invalidates union(old box, new box) - a moving, growing dirty region
    // that spans several render tiles and tears. A fixed box keeps the dirty
    // area small, constant, and inside one tile.
    statusLabel = lv_label_create(root);
    lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(statusLabel, lv_color_white(), 0);
    lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(statusLabel, LCD_WIDTH - 40, 80);
    lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(statusLabel, LV_ALIGN_CENTER, 0, -130);

    bigLabel = lv_label_create(root);
    lv_obj_set_style_text_font(bigLabel, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(bigLabel, lv_color_white(), 0);
    lv_obj_set_size(bigLabel, LCD_WIDTH, 70);
    lv_obj_set_style_text_align(bigLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(bigLabel, LV_ALIGN_CENTER, 0, -30);
    lv_label_set_text(bigLabel, "");

    threshLabel = lv_label_create(root);
    lv_obj_set_style_text_font(threshLabel, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(threshLabel, lv_color_white(), 0);
    lv_obj_set_size(threshLabel, LCD_WIDTH, 40);
    lv_obj_set_style_text_align(threshLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(threshLabel, LV_ALIGN_CENTER, 0, 45);

    threshSlider = lv_slider_create(root);
    lv_obj_set_width(threshSlider, 240);
    lv_slider_set_range(threshSlider, 2, 12);
    lv_slider_set_value(threshSlider, 6, LV_ANIM_OFF);
    lv_obj_align(threshSlider, LV_ALIGN_CENTER, 0, 85);
    lv_obj_add_event_cb(threshSlider, threshChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    // Pulled in from the corners toward the bottom-centre: the round panel
    // clips its corners, so LV_ALIGN_BOTTOM_LEFT/RIGHT would push part of the
    // button off the visible circle.
    recalBtn = makeButton("Recal", recalClicked, LV_ALIGN_BOTTOM_MID, -85, -24);
    stopBtn = makeButton("Stop", stopClicked, LV_ALIGN_BOTTOM_MID, 85, -24);
}

// render() is called ~20x/s. It MUST only touch an LVGL object when that
// object's content actually changed - any unconditional setter (especially
// on `root`) re-invalidates the whole 410x502 screen every frame, which
// software-renders + QSPI-pushes the entire panel over and over and drags
// the whole UI (including touch, which is polled from lv_timer_handler) to a
// crawl. Everything below diffs against the last value first.
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
    bool showBig = false, showThresh = false, showButtons = false;

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
            showBig = showButtons = true;
            break;

        case State::Monitoring:
            bg = s.ready ? kBgReady : kBgIdleDark;
            snprintf(status, sizeof(status), "%s", s.ready ? "Ready" : "");
            snprintf(big, sizeof(big), "%.1f\xC2\xB0", s.angleDeg);
            snprintf(thresh, sizeof(thresh), "click: %.0f\xC2\xB0", s.clickThresholdDeg);
            showBig = showThresh = showButtons = true;
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

    int vis = (showBig ? 1 : 0) | (showThresh ? 2 : 0) | (showButtons ? 4 : 0);
    if (vis != lastVis) {
        lastVis = vis;
        setHidden(bigLabel, !showBig);
        setHidden(threshLabel, !showThresh);
        setHidden(threshSlider, !showThresh);
        setHidden(recalBtn, !showButtons);
        setHidden(stopBtn, !showButtons);
    }
}

}  // namespace ui
