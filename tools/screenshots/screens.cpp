// Headless renderer for every Honu Watch screen, for the user guide.
//
// Drives the REAL src/ui.cpp (putting app) and src/update_ui.cpp (update
// mode) on the host with a software-rendered LVGL display, so the captures
// have the firmware's actual fonts, layout, colours and QR codes. What is
// faked is everything underneath: the clock, the battery, and the update
// engine, whose status is set by hand for each screen.
//
//   tools/screenshots/render_screens.py        (builds and runs this)
//
// Writes one raw RGB file per screen into the directory given as argv[1];
// the Python script turns them into PNGs.

#include <lvgl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Arduino.h"
#include "Battery.h"
#include "pin_config.h"
#include "ui.h"
#include "update_ui.h"
#include "wifi_ota.h"

// ---- fakes for the firmware's dependencies ---------------------------------

static uint32_t gNow = 0;
uint32_t millis() { return gNow; }
EspClass ESP;

static int gFakePct = -1;      // what the fake battery reports; -1 = no battery
static bool gFakeCharger = false;

// "Millivolts" are really percent here, and percentForMillivolts is the
// identity: the harness only needs to say "this much battery".
bool Battery::begin(TwoWire&) { return true; }
int Battery::millivolts() { return gFakePct; }
int Battery::percentForMillivolts(int mv) { return mv; }
bool Battery::externalPower() { return gFakeCharger; }
void Battery::shutdown() {}

static wifiota::Status gFakeStatus;
namespace wifiota {
void begin(const char*) {}
Status status() { return gFakeStatus; }
void setPower(int, bool) {}
void install() {}
void retry() {}
void changeWifi() {}
}  // namespace wifiota

// ---- display ----------------------------------------------------------------

static const char* gOutDir = ".";
static std::vector<uint8_t> gFrame(LCD_WIDTH * LCD_HEIGHT * 3);
alignas(64) static uint8_t gDrawBuf[LCD_WIDTH * LCD_HEIGHT * 2 + 4096];

static void flushCb(lv_display_t* disp, const lv_area_t*, uint8_t* px) {
    if (lv_display_flush_is_last(disp)) {
        const uint32_t stride = lv_draw_buf_width_to_stride(LCD_WIDTH, LV_COLOR_FORMAT_RGB565);
        for (int y = 0; y < LCD_HEIGHT; y++) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(px + y * stride);
            uint8_t* dst = &gFrame[y * LCD_WIDTH * 3];
            for (int x = 0; x < LCD_WIDTH; x++) {
                uint16_t p = src[x];
                uint8_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                dst[x * 3 + 0] = (r << 3) | (r >> 2);
                dst[x * 3 + 1] = (g << 2) | (g >> 4);
                dst[x * 3 + 2] = (b << 3) | (b >> 2);
            }
        }
    }
    lv_display_flush_ready(disp);
}

static void capture(const char* name, bool advanceClock = true) {
    if (advanceClock) {
        for (int i = 0; i < 3; i++) {
            gNow += 120;
            lv_timer_handler();
        }
    } else {
        lv_timer_handler();
    }
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(nullptr);  // full redraw of the active screen, flushed synchronously

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.rgb", gOutDir, name);
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fwrite(gFrame.data(), 1, gFrame.size(), f);
    fclose(f);
    printf("captured %s\n", name);
}

// ---- the putting app's screens ----------------------------------------------

static void putingAppScreens() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_screen_load(scr);
    ui::init("0.1.1");

    ui::Snapshot s;
    s.batteryPct = 82;
    s.batteryMv = 4010;

    s.state = SwingTracker::State::WaitingForCalibration;
    ui::render(s);
    capture("app-1-tap-when-in-putting-position");

    s.state = SwingTracker::State::Accumulating;
    ui::render(s);
    capture("app-2-hold-still");

    s.state = SwingTracker::State::Monitoring;
    s.clickLevel = 6;
    s.angleDeg = 0.4f;
    s.ready = true;
    ui::render(s);
    capture("app-3-ready");

    s.ready = false;
    s.angleDeg = 14.8f;
    s.clickCount = 2;
    ui::render(s);
    capture("app-4-backswing");

    // Last: the Bye screen hides everything else for good (it is a one-way
    // trip to power-off), so nothing can be captured after it.
    ui::showByeScreen();
    capture("app-5-bye");

    // The Update button's two feedback states: held down, then clicked (which
    // relabels it just before the restart). The Bye screen is a one-way trip,
    // so these come last. Its only visible button is Update.
    lv_obj_t* active = lv_screen_active();
    lv_obj_t* update = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(active); i++) {
        lv_obj_t* c = lv_obj_get_child(active, i);
        if (lv_obj_check_type(c, &lv_button_class) && !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) update = c;
    }
    if (update) {
        lv_obj_add_state(update, LV_STATE_PRESSED);
        capture("app-6-bye-update-touched");
        lv_obj_remove_state(update, LV_STATE_PRESSED);
        lv_obj_send_event(update, LV_EVENT_CLICKED, nullptr);  // ui::onUpdate is unset here: no restart
        capture("app-7-bye-update-starting");
    }
}

// ---- update mode's screens ---------------------------------------------------

using wifiota::Phase;
using wifiota::Status;

static void showUpdate(const char* name, const Status& st, int batteryPct = 82, bool charger = false) {
    gFakeStatus = st;
    gFakePct = batteryPct;
    gFakeCharger = charger;
    // Jump the clock well past the 2 s battery poll and 100 ms render
    // intervals, landing where the "busy" dots read "..." (the nicest frame).
    gNow = ((gNow / 2700) + 1) * 2700 + 900;
    updateui::loop();
    updateui::loop();
    // The theme animates state changes (e.g. a button greying out) over ~200 ms.
    // Let LVGL's animations finish by advancing the clock between ticks WITHOUT
    // calling updateui::loop() again: its render pass is what reads the clock
    // for the dots, so they keep the frame chosen above.
    for (int i = 0; i < 5; i++) {
        gNow += 100;
        lv_timer_handler();
    }
    capture(name, /*advanceClock=*/false);
}

static void copy(char* dst, size_t n, const char* src) { snprintf(dst, n, "%s", src); }

static void updateModeScreens() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_screen_load(scr);
    static Battery battery;
    updateui::begin("0.1.1", battery);

    Status st;

    st = Status();
    st.phase = Phase::Scanning;
    showUpdate("update-01-looking-for-networks", st);

    st = Status();
    st.phase = Phase::Provisioning;
    copy(st.apSsid, sizeof(st.apSsid), "HonuWatch-A1B2");
    copy(st.apPass, sizeof(st.apPass), "K7M2QX9TRN");
    showUpdate("update-02-setup-scan-to-join", st);

    st.portalClient = true;
    showUpdate("update-03-setup-scan-to-open-page", st);

    st.portalClient = false;
    copy(st.error, sizeof(st.error), "Couldn't join \"HomeWiFi\". Wrong password?");
    showUpdate("update-04-setup-wrong-password", st);

    st = Status();
    st.phase = Phase::Connecting;
    copy(st.ssid, sizeof(st.ssid), "HomeWiFi");
    showUpdate("update-05-joining-wifi", st);

    st = Status();
    st.phase = Phase::Checking;
    showUpdate("update-06-checking", st);

    st = Status();
    st.phase = Phase::UpToDate;
    copy(st.version, sizeof(st.version), "0.1.1");
    showUpdate("update-07-up-to-date", st);

    st = Status();
    st.phase = Phase::Available;
    copy(st.version, sizeof(st.version), "0.2.0");
    copy(st.notes, sizeof(st.notes), "Faster click response");
    showUpdate("update-08-update-available", st);
    showUpdate("update-09-update-available-low-battery", st, /*batteryPct=*/25, /*charger=*/false);

    st = Status();
    st.phase = Phase::Downloading;
    st.progressPct = 42;
    showUpdate("update-10-downloading", st);

    st = Status();
    st.phase = Phase::Installed;
    showUpdate("update-11-installed", st);

    st = Status();
    st.phase = Phase::Error;
    copy(st.error, sizeof(st.error), "The update server returned error 404.");
    showUpdate("update-12-something-went-wrong", st);

    st = Status();
    st.phase = Phase::NeedsUsbInstall;
    copy(st.version, sizeof(st.version), "0.3.0");
    showUpdate("update-13-usb-install-needed", st);
}

int main(int argc, char** argv) {
    if (argc > 1) gOutDir = argv[1];

    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return gNow; });
    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_flush_cb(disp, flushCb);
    lv_display_set_buffers(disp, gDrawBuf, nullptr, sizeof(gDrawBuf), LV_DISPLAY_RENDER_MODE_FULL);

    putingAppScreens();
    updateModeScreens();
    return 0;
}
