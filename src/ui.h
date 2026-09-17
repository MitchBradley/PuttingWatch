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
    float clickThresholdDeg = 6.0f;
    int clickCount = 0;
    float restG[3] = {0, 0, 0};
    bool imuOk = true;
    int batteryPct = -1;  // 0..100, -1 = unknown
    int batteryMv = -1;   // terminal voltage, mV, -1 = unknown
    bool experimentalMode = false;  // SwingTracker::experimentalMode()
};

void init();
void render(const Snapshot& s);

// Full-screen "Bye", replacing whatever's on screen, with an immediate
// forced flush (bypasses the normal ~20 Hz render cadence) so it's actually
// visible even when the caller powers off a few milliseconds later. For a
// shutdown path right before the display goes dark - not part of the normal
// state-driven render() flow.
void showByeScreen();

// Raised by the view; wired to the tracker in main.
extern std::function<void()> onTap;                    // screen tapped (start calibration)
extern std::function<void()> onRecal;
extern std::function<void()> onStop;
extern std::function<void(int)> onAdjustClickThreshold;  // -1 / +1 from the bottom buttons
extern std::function<void()> onToggleExperimentalMode;   // right-edge N/XZ button

}  // namespace ui
