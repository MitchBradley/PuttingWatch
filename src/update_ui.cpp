#include "update_ui.h"

#include <Arduino.h>
#include <lvgl.h>
#include <esp_system.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "Battery.h"
#include "UpdateCore.h"
#include "pin_config.h"
#include "update_config.h"
#include "wifi_ota.h"

namespace updateui {
namespace {

using Phase = wifiota::Phase;

constexpr uint32_t kBg = 0x000000;
constexpr uint32_t kTextMain = 0xFFFFFF;
constexpr uint32_t kTextBody = 0xD0D0D0;
constexpr uint32_t kTextDim = 0x9A9A9A;
constexpr uint32_t kTextWarn = 0xFF8A7A;
constexpr uint32_t kLink = 0x6CB4FF;
constexpr uint32_t kBtnGreen = 0x1B7F3B;
constexpr uint32_t kBtnGrey = 0x3A3A3A;

constexpr int kContentW = 350;
constexpr uint32_t kIdleRestartMs = 5 * 60 * 1000;  // walked away on a result screen: go back to normal

const char* gRunningVersion = "";
Battery* gBattery = nullptr;
Phase gShownPhase = Phase::Starting;  // what the visible buttons were built for

// Widgets, in on-screen order (the flex column lays out whatever isn't hidden).
lv_obj_t* titleLbl;
lv_obj_t* busyLbl;   // "busy" dots - deliberately not an animated spinner, see buildScreen()
lv_obj_t* qrBox;
lv_obj_t* qr;
lv_obj_t* bar;
lv_obj_t* pctLbl;
lv_obj_t* bodyLbl;
lv_obj_t* noteLbl;
lv_obj_t* primaryBtn;
lv_obj_t* primaryLbl;
lv_obj_t* secondaryBtn;
lv_obj_t* secondaryLbl;
lv_obj_t* linkLbl;

void restartToNormal() {
    delay(50);
    ESP.restart();  // update mode is a one-shot flag: the next boot is the normal app
}

void onPrimary(lv_event_t*) {
    switch (gShownPhase) {
        case Phase::Available: wifiota::install(); break;
        case Phase::UpToDate: restartToNormal(); break;
        case Phase::Error: wifiota::retry(); break;
        default: break;
    }
}
void onSecondary(lv_event_t*) { restartToNormal(); }
void onLink(lv_event_t*) { wifiota::changeWifi(); }

lv_obj_t* makeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color, int width) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, width);
    lv_obj_set_height(l, LV_SIZE_CONTENT);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t* makeButton(lv_obj_t* parent, uint32_t bg, int height, lv_event_cb_t cb, lv_obj_t** label) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_size(b, 300, height);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x444444), LV_STATE_DISABLED);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(kTextMain), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x888888), LV_STATE_DISABLED);
    lv_obj_center(l);
    *label = l;
    return b;
}

void buildScreen() {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* col = lv_obj_create(scr);
    lv_obj_set_size(col, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_left(col, 20, 0);
    lv_obj_set_style_pad_right(col, 20, 0);
    lv_obj_set_style_pad_top(col, 26, 0);
    lv_obj_set_style_pad_bottom(col, 22, 0);
    lv_obj_set_style_pad_row(col, 9, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    titleLbl = makeLabel(col, &lv_font_montserrat_32, kTextMain, kContentW);

    // "Working..." indicator. NOT lv_spinner: this panel's tearing-effect sync
    // is off (CO5300_WC_TEARON is commented out in the init sequence), so
    // frames are pushed unsynchronised and any continuously moving graphic
    // tears badly. A fixed-size label that changes a couple of times a second
    // is the same kind of update the rest of the UI does cleanly.
    busyLbl = makeLabel(col, &lv_font_montserrat_32, kTextMain, 64);
    lv_obj_set_height(busyLbl, 40);
    lv_obj_set_style_text_align(busyLbl, LV_TEXT_ALIGN_LEFT, 0);

    // White box around the QR code: scanners need the quiet zone, and the
    // screen is otherwise black.
    qrBox = lv_obj_create(col);
    lv_obj_set_size(qrBox, 220, 220);
    lv_obj_set_style_bg_color(qrBox, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qrBox, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(qrBox, 12, 0);
    lv_obj_set_style_border_width(qrBox, 0, 0);
    lv_obj_set_style_pad_all(qrBox, 10, 0);
    lv_obj_remove_flag(qrBox, LV_OBJ_FLAG_SCROLLABLE);
    qr = lv_qrcode_create(qrBox);
    lv_qrcode_set_size(qr, 200);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_obj_center(qr);

    bar = lv_bar_create(col);
    lv_obj_set_size(bar, 300, 26);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x33C04D), LV_PART_INDICATOR);

    pctLbl = makeLabel(col, &lv_font_montserrat_28, kTextMain, kContentW);

    bodyLbl = makeLabel(col, &lv_font_montserrat_22, kTextBody, kContentW);
    noteLbl = makeLabel(col, &lv_font_montserrat_16, kTextDim, kContentW);

    primaryBtn = makeButton(col, kBtnGreen, 64, onPrimary, &primaryLbl);
    secondaryBtn = makeButton(col, kBtnGrey, 56, onSecondary, &secondaryLbl);

    // "Change Wi-Fi" sits directly under the main buttons, so it is easy to hit
    // by accident. Three things keep it out of the way: it is only as wide as
    // its words (a label defaults to the full row, which made the whole strip
    // under a button tappable), its invisible touch margin is small, and it has
    // a real gap above it - more than the buttons' and the link's touch margins
    // added together (8 + 6), so their touch areas can never overlap.
    linkLbl = makeLabel(col, &lv_font_montserrat_20, kLink, kContentW);
    lv_obj_set_width(linkLbl, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(linkLbl, 28, 0);
    lv_obj_add_flag(linkLbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(linkLbl, 6);
    lv_obj_add_event_cb(linkLbl, onLink, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_style_text_decor(linkLbl, LV_TEXT_DECOR_UNDERLINE, 0);
}

// ---- diff-based updates ----------------------------------------------------
// Same rule as ui.cpp: touch an LVGL object only when its content changed, or
// every frame re-invalidates and re-pushes screen regions over QSPI.

struct Cached {
    std::string s;
    bool init = false;
};

void setText(lv_obj_t* label, Cached& c, const std::string& now) {
    if (c.init && c.s == now) return;
    c.s = now;
    c.init = true;
    lv_label_set_text(label, now.c_str());
}

void setShown(lv_obj_t* o, int& last, bool show) {
    if (last == (int)show) return;
    last = (int)show;
    if (show)
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Everything a phase wants on screen.
struct View {
    std::string title, body, note, primary, secondary, link, qrData;
    bool titleSmall = false;
    bool busy = false, qr = false, bar = false, pct = false;
    bool primaryEnabled = true, warn = false;
    int progress = 0;
};

const char* siteWithoutScheme() {
    const char* s = strstr(OTA_SITE_URL, "://");
    return s ? s + 3 : OTA_SITE_URL;
}

View viewFor(const wifiota::Status& st, bool batteryOk) {
    View v;
    char buf[160];
    switch (st.phase) {
        case Phase::Starting:
            v.title = "Starting...";
            v.busy = true;
            v.secondary = "Cancel";
            break;
        case Phase::Scanning:
            v.title = "Looking for\nWi-Fi networks";
            v.busy = true;
            v.secondary = "Cancel";
            break;
        case Phase::Provisioning:
            v.title = "Set up Wi-Fi";
            v.titleSmall = true;
            v.qr = true;
            if (st.portalClient) {
                // Joined the watch's network. The sign-in sheet usually opens
                // the page by itself; if it didn't, this opens it.
                v.qrData = "http://192.168.4.1/";
                v.body = "If the page didn't open,\nscan this to open it";
            } else {
                snprintf(buf, sizeof(buf), "WIFI:T:WPA;S:%s;P:%s;;", st.apSsid, st.apPass);
                v.qrData = buf;
                // No manual line break in the instruction: it wraps by itself
                // into two balanced lines, whereas "...camera,\nthen tap Join"
                // wrapped into three with "camera," stranded alone.
                v.body = st.error[0] ? std::string(st.error) + "\nScan to try again."
                                     : "Scan with your phone's camera, then tap Join";
            }
            snprintf(buf, sizeof(buf), "Or join %s\nPassword: %s", st.apSsid, st.apPass);
            v.note = buf;
            v.secondary = "Cancel";
            break;
        case Phase::Connecting:
            v.title = "Joining Wi-Fi";
            v.body = st.ssid;
            v.busy = true;
            v.secondary = "Cancel";
            break;
        case Phase::Checking:
            v.title = "Checking for\nupdates";
            v.busy = true;
            v.secondary = "Cancel";
            break;
        case Phase::UpToDate:
            v.title = "You're up to date";
            snprintf(buf, sizeof(buf), "Version %s", gRunningVersion);
            v.body = buf;
            v.primary = "Done";
            v.link = "Change Wi-Fi";
            break;
        case Phase::Available:
            v.title = "Update available";
            snprintf(buf, sizeof(buf), "Version %s\n(you have %s)", st.version, gRunningVersion);
            v.body = buf;
            if (st.notes[0]) v.body += std::string("\n") + st.notes;
            if (!batteryOk) {
                v.note = "Battery is low. Plug in a charger,\nor charge to " +
                         std::to_string(updatecore::kMinBatteryPct) + "%, to install.";
                v.warn = true;
            }
            v.primary = "Install";
            v.primaryEnabled = batteryOk;
            v.secondary = "Not now";
            v.link = "Change Wi-Fi";
            break;
        case Phase::NeedsUsbInstall:
            v.title = "USB install\nneeded";
            {
                // Host and path on separate lines: together they are wider than
                // the screen and would break in the middle of the host name.
                const std::string site = siteWithoutScheme();
                const size_t slash = site.find('/');
                v.body = std::string("Version ") + st.version +
                         " can't be installed from the watch.\nOn a computer, open\n" +
                         site.substr(0, slash);
                if (slash != std::string::npos) v.body += "\n" + site.substr(slash);
            }
            v.secondary = "Close";
            break;
        case Phase::Downloading:
            v.title = "Downloading\nupdate";
            v.bar = v.pct = true;
            v.progress = st.progressPct;
            v.note = "Keep the watch on until it restarts";
            v.secondary = "Cancel";
            break;
        case Phase::Installed:
            v.title = "Update installed";
            v.body = "Restarting...";
            break;
        case Phase::Error:
            v.title = "Something\nwent wrong";
            v.body = st.error;
            v.primary = "Try again";
            v.secondary = "Cancel";
            v.link = "Change Wi-Fi";
            break;
    }
    return v;
}

void render(const wifiota::Status& st, bool batteryOk) {
    static Cached cTitle, cBody, cNote, cPrimary, cSecondary, cLink, cPct, cQr, cBusy;
    static int sBusy = -1, sQr = -1, sBar = -1, sPct = -1, sBody = -1, sNote = -1;
    static int sPrimary = -1, sSecondary = -1, sLink = -1;
    static int lastTitleSmall = -1, lastWarn = -1, lastPrimaryEnabled = -1, lastProgress = -1;

    View v = viewFor(st, batteryOk);
    gShownPhase = st.phase;

    setText(titleLbl, cTitle, v.title);
    if (lastTitleSmall != (int)v.titleSmall) {
        lastTitleSmall = (int)v.titleSmall;
        lv_obj_set_style_text_font(titleLbl, v.titleSmall ? &lv_font_montserrat_28 : &lv_font_montserrat_32, 0);
    }
    setText(bodyLbl, cBody, v.body);
    setText(noteLbl, cNote, v.note);
    if (lastWarn != (int)v.warn) {
        lastWarn = (int)v.warn;
        lv_obj_set_style_text_color(noteLbl, lv_color_hex(v.warn ? kTextWarn : kTextDim), 0);
    }
    setText(primaryLbl, cPrimary, v.primary);
    setText(secondaryLbl, cSecondary, v.secondary);
    setText(linkLbl, cLink, v.link);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", v.progress);
    setText(pctLbl, cPct, pct);
    if (lastProgress != v.progress) {
        lastProgress = v.progress;
        lv_bar_set_value(bar, v.progress, LV_ANIM_OFF);
    }
    if (lastPrimaryEnabled != (int)v.primaryEnabled) {
        lastPrimaryEnabled = (int)v.primaryEnabled;
        if (v.primaryEnabled)
            lv_obj_remove_state(primaryBtn, LV_STATE_DISABLED);
        else
            lv_obj_add_state(primaryBtn, LV_STATE_DISABLED);
    }
    if (v.qr && (!cQr.init || cQr.s != v.qrData)) {
        cQr.s = v.qrData;
        cQr.init = true;
        lv_qrcode_update(qr, v.qrData.c_str(), v.qrData.size());
    }

    if (v.busy) {
        static const char* const kDots[] = {".", "..", "..."};
        setText(busyLbl, cBusy, kDots[(millis() / 450) % 3]);
    }
    setShown(busyLbl, sBusy, v.busy);
    setShown(qrBox, sQr, v.qr);
    setShown(bar, sBar, v.bar);
    setShown(pctLbl, sPct, v.pct);
    setShown(bodyLbl, sBody, !v.body.empty());
    setShown(noteLbl, sNote, !v.note.empty());
    setShown(primaryBtn, sPrimary, !v.primary.empty());
    setShown(secondaryBtn, sSecondary, !v.secondary.empty());
    setShown(linkLbl, sLink, !v.link.empty());
}

}  // namespace

void begin(const char* runningVersion, Battery& battery) {
    gRunningVersion = runningVersion ? runningVersion : "";
    gBattery = &battery;
    buildScreen();
    wifiota::begin(gRunningVersion);
}

void loop() {
    lv_timer_handler();
    const uint32_t now = millis();

    // Battery, for the install gate (an I2C read - every couple of seconds).
    static uint32_t lastPoll = 0;
    static bool batteryOk = true;
    if (lastPoll == 0 || now - lastPoll >= 2000) {
        lastPoll = now ? now : 1;
        int mv = gBattery->millivolts();
        int pct = Battery::percentForMillivolts(mv);
        bool ext = gBattery->externalPower();
        wifiota::setPower(pct, ext);
        batteryOk = updatecore::batteryAllowsUpdate(pct, ext);
    }

    static uint32_t lastRender = 0;
    if (now - lastRender >= 100) {
        lastRender = now;
        render(wifiota::status(), batteryOk);
    }

    // BOOT button = cancel, anywhere: restarting is always safe (see wifi_ota.h).
    static bool bootWasDown = false;
    bool down = digitalRead(BTN_BOOT) == LOW;
    if (down && !bootWasDown) restartToNormal();
    bootWasDown = down;

    // Left on a result screen: go back to the normal app rather than sit
    // there with the screen and radio on.
    if (lv_display_get_inactive_time(nullptr) > kIdleRestartMs &&
        (gShownPhase == Phase::UpToDate || gShownPhase == Phase::Available ||
         gShownPhase == Phase::NeedsUsbInstall || gShownPhase == Phase::Error)) {
        restartToNormal();
    }

    delay(5);  // yield; lv_timer_handler never blocks
}

}  // namespace updateui
