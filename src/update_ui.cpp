#include "update_ui.h"

#include <Arduino.h>
#include <esp_system.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "Battery.h"
#include "MiniUi.h"
#include "UpdateCore.h"
#include "pin_config.h"
#include "ui_fonts.h"
#include "update_config.h"
#include "wifi_ota.h"

namespace updateui {
namespace {

using mui::Align;
using mui::Kind;
using mui::Rect;
using mui::Widget;
using Phase = wifiota::Phase;

constexpr uint32_t kBg = 0x000000;
constexpr uint32_t kTextMain = 0xFFFFFF;
constexpr uint32_t kTextBody = 0xD0D0D0;
constexpr uint32_t kTextDim = 0x9A9A9A;
constexpr uint32_t kTextWarn = 0xFF8A7A;
constexpr uint32_t kLink = 0x6CB4FF;
constexpr uint32_t kBtnGreen = 0x1B7F3B;
constexpr uint32_t kBtnGreenPressed = 0x145F2C;
constexpr uint32_t kBtnGrey = 0x3A3A3A;
constexpr uint32_t kBtnGreyPressed = 0x5A5A5A;

// u8g2 fonts (1-bit), named by the size LVGL's Montserrat had here.
const uint8_t* const kFont32 = u8g2_font_logisoso24_tf;
const uint8_t* const kFont28 = u8g2_font_logisoso22_tf;
const uint8_t* const kFont22 = u8g2_font_helvR18_tf;
const uint8_t* const kFont20 = u8g2_font_helvR14_tf;
const uint8_t* const kFont16 = u8g2_font_helvR12_tf;

constexpr int kContentW = 350;
constexpr uint32_t kIdleRestartMs = 5 * 60 * 1000;  // walked away on a result screen: go back to normal

// The screen is one centred column: padding, a gap between items, and the
// whole stack centred vertically in what's left.
constexpr int kPadTop = 26, kPadBottom = 22, kRowGap = 9;
constexpr int kLinkMarginTop = 28;  // extra gap above "Change Wi-Fi", see buildScreen()

const char* gRunningVersion = "";
Battery* gBattery = nullptr;
Phase gShownPhase = Phase::Starting;  // what the visible buttons were built for

// Widgets, in on-screen order (layout() stacks whatever isn't hidden).
Widget* titleLbl;
Widget* busyLbl;   // "busy" dots - deliberately not an animated spinner, see buildScreen()
Widget* qrBox;
Widget* qr;
Widget* bar;
Widget* pctLbl;
Widget* bodyLbl;
Widget* noteLbl;
Widget* primaryBtn;
Widget* secondaryBtn;
Widget* linkLbl;

void restartToNormal() {
    delay(50);
    ESP.restart();  // update mode is a one-shot flag: the next boot is the normal app
}

void onPrimary() {
    switch (gShownPhase) {
        case Phase::Available: wifiota::install(); break;
        case Phase::UpToDate: restartToNormal(); break;
        case Phase::Error: wifiota::retry(); break;
        default: break;
    }
}
void onSecondary() { restartToNormal(); }
void onLink() { wifiota::changeWifi(); }

Widget* makeLabel(const uint8_t* font, uint32_t color) {
    Widget* l = mui::add(Kind::Label, {0, 0, kContentW, 0});
    l->font = font;
    l->fg = color;
    l->valign = mui::VAlign::Top;
    l->hidden = true;
    return l;
}

Widget* makeButton(uint32_t bg, uint32_t bgPressed, int height, std::function<void()> cb) {
    Widget* b = mui::add(Kind::Button, {0, 0, 300, height});
    b->font = kFont28;
    b->bg = bg;
    b->bgPressed = bgPressed;
    b->radius = 18;
    b->ext = 8;
    b->onClick = std::move(cb);
    b->hidden = true;
    return b;
}

void buildScreen() {
    mui::setBackground(kBg);

    titleLbl = makeLabel(kFont32, kTextMain);

    // "Working..." indicator. NOT an animated spinner: this panel's tearing-
    // effect sync is off (CO5300_WC_TEARON is commented out in the init
    // sequence), so frames are pushed unsynchronised and any continuously
    // moving graphic tears badly. A fixed-size label that changes a couple of
    // times a second is the same kind of update the rest of the UI does cleanly.
    busyLbl = makeLabel(kFont32, kTextMain);
    busyLbl->r.w = 64;
    busyLbl->r.h = 40;
    busyLbl->align = Align::Left;

    // White box around the QR code: scanners need the quiet zone, and the
    // screen is otherwise black.
    qrBox = mui::add(Kind::Box, {0, 0, 220, 220});
    qrBox->bg = 0xFFFFFF;
    qrBox->hasBg = true;
    qrBox->radius = 12;
    qrBox->hidden = true;
    qr = mui::add(Kind::Qr, {0, 0, 200, 200});
    qr->fg = 0x000000;
    qr->hidden = true;

    bar = mui::add(Kind::Bar, {0, 0, 300, 26});
    bar->bg = 0x333333;
    bar->fg = 0x33C04D;
    bar->hidden = true;

    pctLbl = makeLabel(kFont28, kTextMain);
    bodyLbl = makeLabel(kFont22, kTextBody);
    noteLbl = makeLabel(kFont16, kTextDim);

    primaryBtn = makeButton(kBtnGreen, kBtnGreenPressed, 64, onPrimary);
    secondaryBtn = makeButton(kBtnGrey, kBtnGreyPressed, 56, onSecondary);

    // "Change Wi-Fi" sits directly under the main buttons, so it is easy to hit
    // by accident. Three things keep it out of the way: it is only as wide as
    // its words, its invisible touch margin is small, and it has a real gap
    // above it - more than the buttons' and the link's touch margins added
    // together (8 + 6), so their touch areas can never overlap.
    linkLbl = makeLabel(kFont20, kLink);
    linkLbl->underline = true;
    linkLbl->ext = 6;
    linkLbl->onClick = onLink;
}

// ---- layout -----------------------------------------------------------------

// Place the visible widgets as a centred column. Sizes come from the content
// (wrapped text), so this runs whenever a phase's text changes; setRect only
// repaints what actually moved.
void layout() {
    struct Item {
        Widget* w;
        int h;
        int w_;  // width
        int before;  // extra space above
    };
    Item items[11];
    int n = 0;
    auto add = [&](Widget* w, int h, int width, int before = 0) {
        if (!w->hidden) items[n++] = {w, h, width, before};
    };
    auto labelH = [](Widget* l) { return mui::lineCount(l->text) * mui::lineHeight(l->font); };

    add(titleLbl, labelH(titleLbl), kContentW);
    add(busyLbl, 40, 64);
    add(qrBox, 220, 220);
    add(bar, 26, 300);
    add(pctLbl, labelH(pctLbl), kContentW);
    add(bodyLbl, labelH(bodyLbl), kContentW);
    add(noteLbl, labelH(noteLbl), kContentW);
    add(primaryBtn, primaryBtn->r.h, 300);
    add(secondaryBtn, secondaryBtn->r.h, 300);
    add(linkLbl, labelH(linkLbl), mui::textWidth(linkLbl->font, linkLbl->text), kLinkMarginTop);

    int total = 0;
    for (int i = 0; i < n; i++) total += items[i].h + items[i].before + (i ? kRowGap : 0);
    int y = kPadTop + ((mui::kScreenH - kPadTop - kPadBottom) - total) / 2;
    for (int i = 0; i < n; i++) {
        if (i) y += kRowGap;
        y += items[i].before;
        const int x = (mui::kScreenW - items[i].w_) / 2;
        items[i].w->setRect({x, y, items[i].w_, items[i].h});
        if (items[i].w == qrBox) qr->setRect({x + 10, y + 10, 200, 200});
        y += items[i].h;
    }
}

// ---- diff-based updates ----------------------------------------------------
// The widget setters only repaint on a real change; these small wrappers keep
// the wrapped-text work off the hot path.

void setWrapped(Widget* l, const std::string& text) {
    l->setText(mui::wrap(l->font, text, kContentW));
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
    View v = viewFor(st, batteryOk);
    gShownPhase = st.phase;

    titleLbl->setFont(v.titleSmall ? kFont28 : kFont32);
    setWrapped(titleLbl, v.title);
    setWrapped(bodyLbl, v.body);
    setWrapped(noteLbl, v.note);
    noteLbl->setFg(v.warn ? kTextWarn : kTextDim);
    primaryBtn->setText(v.primary);
    secondaryBtn->setText(v.secondary);
    linkLbl->setText(v.link);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", v.progress);
    pctLbl->setText(pct);
    bar->setValue(v.progress);
    primaryBtn->setDisabled(!v.primaryEnabled);
    if (v.qr) qr->setQr(v.qrData);

    if (v.busy) {
        static const char* const kDots[] = {".", "..", "..."};
        busyLbl->setText(kDots[(millis() / 450) % 3]);
    }
    busyLbl->show(v.busy);
    qrBox->show(v.qr);
    qr->show(v.qr);
    bar->show(v.bar);
    pctLbl->show(v.pct);
    titleLbl->show(!v.title.empty());
    bodyLbl->show(!v.body.empty());
    noteLbl->show(!v.note.empty());
    primaryBtn->show(!v.primary.empty());
    secondaryBtn->show(!v.secondary.empty());
    linkLbl->show(!v.link.empty());
    layout();
}

}  // namespace

void begin(const char* runningVersion, Battery& battery) {
    gRunningVersion = runningVersion ? runningVersion : "";
    gBattery = &battery;
    buildScreen();
    wifiota::begin(gRunningVersion);
}

void loop() {
    mui::update(millis());
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
    if (mui::idleMs(now) > kIdleRestartMs &&
        (gShownPhase == Phase::UpToDate || gShownPhase == Phase::Available ||
         gShownPhase == Phase::NeedsUsbInstall || gShownPhase == Phase::Error)) {
        restartToNormal();
    }

    delay(5);  // yield
}

}  // namespace updateui
