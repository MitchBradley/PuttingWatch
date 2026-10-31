// Headless renderer for every Honu Watch screen, for the user guide.
//
// Drives the REAL src/ui.cpp (putting app) and src/update_ui.cpp (update
// mode) on the host with MiniUi drawing into a RAM "panel", so the captures
// have the firmware's actual fonts, layout, colours and QR codes. What is
// faked is everything underneath: the clock, the battery, and the update
// engine, whose status is set by hand for each screen.
//
//   tools/screenshots/render_screens.py        (builds and runs this)
//
// Writes one raw RGB file per screen into the directory given as argv[1];
// the Python script turns them into PNGs.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Arduino.h"
#include "Arduino_GFX.h"
#include "MiniUi.h"
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

// The panel: whatever MiniUi pushes lands in this frame buffer.
class HostPanel : public Arduino_GFX {
public:
    HostPanel() : Arduino_GFX(LCD_WIDTH, LCD_HEIGHT), px_(LCD_WIDTH * LCD_HEIGHT, 0) {}
    bool begin(int32_t = GFX_NOT_DEFINED) override { return true; }
    void writePixelPreclipped(int16_t x, int16_t y, uint16_t c) override { px_[y * LCD_WIDTH + x] = c; }
    void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* bitmap, int16_t w, int16_t h) override {
        for (int yy = 0; yy < h; yy++) memcpy(&px_[(y + yy) * LCD_WIDTH + x], bitmap + yy * w, w * 2);
    }
    using Arduino_GFX::draw16bitRGBBitmap;
    const std::vector<uint16_t>& pixels() const { return px_; }

private:
    std::vector<uint16_t> px_;
};

static HostPanel gPanel;
static std::vector<uint16_t> gStrip(LCD_WIDTH * 100);

static void capture(const char* name) {
    mui::update(gNow);
    mui::invalidateAll();
    mui::refresh();  // full redraw, flushed synchronously

    std::vector<uint8_t> frame(LCD_WIDTH * LCD_HEIGHT * 3);
    const std::vector<uint16_t>& px = gPanel.pixels();
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        uint16_t p = px[i];
        uint8_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
        frame[i * 3 + 0] = (r << 3) | (r >> 2);
        frame[i * 3 + 1] = (g << 2) | (g >> 4);
        frame[i * 3 + 2] = (b << 3) | (b >> 2);
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.rgb", gOutDir, name);
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fwrite(frame.data(), 1, frame.size(), f);
    fclose(f);
    printf("captured %s\n", name);
}

// ---- the putting app's screens ----------------------------------------------

static void putingAppScreens() {
    mui::reset();
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

    // The Bye screen's own buttons (Help, Honu, Update). Each press is a fresh
    // touch: let the previous one end first.
    auto press = [](int x, int y) {
        gNow += 300;
        mui::update(gNow);
        mui::touch(x, y, gNow);
    };
    press(117, 53);  // Help
    capture("app-8-bye-help-qr");
    ui::dismissByeQr();
    press(293, 53);  // Honu
    capture("app-9-bye-honu-qr");
    ui::dismissByeQr();

    // The Update button's two feedback states: held down, then clicked (which
    // relabels it just before the restart). The Bye screen is a one-way trip,
    // so these come last.
    mui::Widget* update = mui::widgetAt(205, 361);
    if (update) {
        update->setPressed(true);
        capture("app-6-bye-update-touched");
        update->setPressed(false);
        press(205, 361);  // ui::onUpdate is unset here: no restart
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
    capture(name);
}

static void copy(char* dst, size_t n, const char* src) { snprintf(dst, n, "%s", src); }

static void updateModeScreens() {
    mui::reset();
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

    mui::init(&gPanel, gStrip.data(), gStrip.size());

    putingAppScreens();
    updateModeScreens();
    return 0;
}
