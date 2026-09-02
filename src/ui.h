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
};

void init();
void render(const Snapshot& s);

// Raised by the view; wired to the tracker in main.
extern std::function<void()> onTap;                    // screen tapped (start calibration)
extern std::function<void()> onRecal;
extern std::function<void()> onStop;
extern std::function<void(int)> onAdjustClickThreshold;  // -1 / +1 from the bottom buttons

}  // namespace ui
