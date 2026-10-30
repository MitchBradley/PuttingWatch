#pragma once
#include <functional>
#include "SwingTracker.h"

// LVGL view layer. Builds the screen once (init), then render() pushes a
// plain snapshot of Model state onto it each frame. No swing logic here -
// user input comes back out through the std::function hooks, which main wires
// to the SwingTracker.
namespace ui {

struct Snapshot {
    SwingTracker::State state = SwingTracker::State::WaitingForCalibration;
    SwingTracker::SwingPhase phase = SwingTracker::SwingPhase::Backswing;
    bool ready = false;
    float angleDeg = 0.0f;
    int clickLevel = 3;  // user-facing 1..10 "sensitivity" - see main.cpp's levelForDegrees()
    int clickCount = 0;
    float restG[3] = {0, 0, 0};
    bool imuOk = true;
    int batteryPct = -1;  // 0..100, -1 = unknown
    int batteryMv = -1;   // terminal voltage, mV, -1 = unknown
};

// `firmwareVersion` is shown on the initial "Tap when in putting position"
// screen (string must outlive the call - pass the FW_VERSION literal).
void init(const char* firmwareVersion);
void render(const Snapshot& s);

// Full-screen "Bye", replacing whatever's on screen, with an immediate
// forced flush (bypasses the normal ~20 Hz render cadence) so it's actually
// visible even when the caller powers off a few milliseconds later. For a
// shutdown path right before the display goes dark - not part of the normal
// state-driven render() flow. Also shows an "Update" button (-> onUpdate): the
// Bye screen is the one place the user is deliberately looking at the watch,
// so a tap here can't be an accident the way one on the "Tap when" screen
// (typically with the watch out of sight) could be.
void showByeScreen();

// The Bye screen's Help / Honu buttons each replace it with a full-screen QR
// code (guide page / honugolf.com). While one is up, the caller keeps the
// Bye screen alive instead of powering off: poll byeQrActive(), call
// byeTick() to apply the 60 s auto-dismiss, and dismissByeQr() on the upper
// side button. Dismissing returns to the Bye screen.
bool byeQrActive();
void byeTick();
void dismissByeQr();

// Raised by the view; wired to the tracker in main.
extern std::function<void()> onTap;                    // screen tapped (start calibration)
extern std::function<void()> onRecal;
extern std::function<void()> onStop;
extern std::function<void(int)> onAdjustClickThreshold;  // -1 / +1 from the bottom buttons
extern std::function<void()> onUpdate;                 // "Update" button on the Bye screen

}  // namespace ui
