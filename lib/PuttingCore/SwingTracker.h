#pragma once
#include <functional>
#include "MotionSample.h"
#include "Vec3.h"

// SwingTracker - the Model. Owns ALL putting-stroke logic: calibration,
// arming, click ratcheting, reversal detection, re-arm gating. Knows nothing
// about LVGL, I2S, or Arduino - pure C++, host-compilable, unit-tested in
// test/test_swingtracker.
//
// Ported from PuttingCoachWatch/CalibrationManager.swift (the watch bring-up
// subset: clicks only, no impact detection or face-angle). The Swift version
// leaned on dispatch timers and a live CoreMotion callback; here everything is
// driven by update(sample) at a fixed rate, timing derived from
// sample.t_ms - the same approach the phone app's MotionManager uses.
//
// Keep swing logic OUT of the UI and audio layers. That split is deliberate
// (it was a real bug source in the Swift app); see the plan / memory.
class SwingTracker {
  public:
    enum class State {
        WaitingForCalibration,  // tap to begin
        Accumulating,           // holding still, averaging a rest gravity reading
        Done,                   // rest captured; showing it briefly
        Monitoring              // live: tracking the stroke, firing clicks
    };

    // The stroke lifecycle within Monitoring. Mirrors CalibrationManager's
    // private SwingPhase (itself a scaled-down MotionManager.StrokePhase).
    enum class SwingPhase {
        Backswing,      // clicking enabled, tracking away from rest
        ForwardStroke,  // reversal seen - clicking suppressed, waiting out endOfSwingDelay
        Recovering      // swing over - waiting for held-still time before re-arming
    };

    struct Config {
        uint32_t accumulationMs = 2000;    // hold-still averaging window
        uint32_t monitoringDelayMs = 2000; // Done -> Monitoring auto-advance
        // Matches the UI's 1..10 "sensitivity level" mapping (main.cpp:
        // kLevelBaseDegrees=3.5, kLevelStepDegrees=1.2) - level 1 and level
        // 10 respectively. Keep in sync if that mapping changes; this class
        // only ever reasons in degrees, the level is purely a UI concept.
        float clickThresholdDegreesMin = 3.5f;
        float clickThresholdDegreesMax = 14.3f;
        float reversalDropDegrees = 2.0f;      // drop below peak => forward stroke
        uint32_t endOfSwingDelayMs = 2000;     // forward stroke held this long => recovering
        uint32_t stillnessDurationMs = 1000;   // continuous still time before re-arm
        float stillnessToleranceDegrees = 3.0f;   // drift that resets the stillness window
        float restRecenterMaxDriftDegrees = 10.0f; // settled pos must be this near the
                                                   // ORIGINAL calibrated rest to re-center
        // A good putting stroke clicks at most twice; three or more in one
        // backswing means the golfer is swinging far too fast for the
        // stroke to be any good, so there's no point pestering them with
        // every further threshold crossing. Caps clicks per backswing
        // attempt, not overall - lastClickBucket_ resets on every re-arm.
        int maxClicksPerSwing = 3;
    };

    SwingTracker() = default;
    explicit SwingTracker(const Config& cfg) : cfg_(cfg) {}

    // --- input ------------------------------------------------------------
    // WaitingForCalibration -> Accumulating. No-op in any other state.
    void tap();
    // Any state -> WaitingForCalibration (the "Recal" button).
    void recal();
    // Feed one processed sample. Call at a steady rate (~100 Hz).
    void update(const MotionSample& s);

    // Click interval in degrees, clamped to [min,max]. Settable live from the
    // UI (replaces the watch's Digital Crown).
    void setClickThresholdDegrees(float deg);
    float clickThresholdDegrees() const { return clickThresholdDegrees_; }

    // --- output ---------------------------------------------------------------
    State state() const { return state_; }
    SwingPhase swingPhase() const { return swingPhase_; }
    // "armed, next backswing will register" - true only while Monitoring and
    // in the Backswing phase. Drives the green background tint.
    bool isReadyForSwing() const {
        return state_ == State::Monitoring && swingPhase_ == SwingPhase::Backswing;
    }
    // Angle between the current gravity and the calibrated rest gravity, in
    // degrees. Valid (non-zero-meaning) only while Monitoring.
    float swingAngleDegrees() const { return swingAngleDegrees_; }
    int clickCount() const { return clickCount_; }
    Vec3 restGravity() const { return restGravity_; }

    // Fired once per threshold crossing during the backswing. The renderer
    // (audio click / haptic) subscribes here; it does no logic of its own.
    std::function<void()> onClick;
    // Fired once when a calibration completes (Accumulating -> Done). Lets the
    // renderer play its "calibrated" cue.
    std::function<void()> onCalibrationComplete;

  private:
    void enterMonitoring();
    void processMonitoringSample(const MotionSample& s);

    Config cfg_{};
    State state_ = State::WaitingForCalibration;
    SwingPhase swingPhase_ = SwingPhase::Backswing;

    float clickThresholdDegrees_ = 5.9f;  // level 3 (3.5 + 2*1.2) - see Config's min/max comment

    // --- accumulation ---
    bool accumPending_ = false;    // tap() seen, first Accumulating sample not yet processed
    uint32_t accumStartMs_ = 0;
    Vec3 accumSum_{0, 0, 0};
    int accumCount_ = 0;

    uint32_t doneEnteredMs_ = 0;

    // --- references ---
    Vec3 restGravity_{0, 0, 1};          // recentred after each settled swing
    Vec3 initialRestGravity_{0, 0, 1};   // fixed at calibration; drift check for re-center

    // --- live swing state ---
    float swingAngleDegrees_ = 0.0f;
    int lastClickBucket_ = 0;
    int clickCount_ = 0;
    float peakAngleThisSwing_ = 0.0f;

    uint32_t forwardStrokeStartMs_ = 0;

    bool haveStillnessRef_ = false;
    Vec3 stillnessRefGravity_{0, 0, 0};
    uint32_t stillnessWindowStartMs_ = 0;
};
