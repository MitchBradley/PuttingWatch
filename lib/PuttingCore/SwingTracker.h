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
        // Matches the UI's 1..16 "sensitivity level" mapping (main.cpp:
        // kLevelBaseDegrees=3.0, kLevelStepDegrees=0.9, kLevelCount=16) -
        // level 1 and level 16 respectively. Keep in sync if that mapping
        // changes; this class only ever reasons in degrees, the level is
        // purely a UI concept.
        float clickThresholdDegreesMin = 3.0f;
        float clickThresholdDegreesMax = 16.5f;
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
        // A tilt opposite the real backswing direction (address wobble, a
        // false start) is otherwise indistinguishable from a real small
        // backswing: swingAngleDegrees is an unsigned distance from rest, so
        // either one raises the peak and can trip a false reversal, which
        // then swallows the real stroke that follows (ForwardStroke
        // suppresses clicking outright). Once any swing this calibration
        // reaches this many degrees, its direction is latched as "the"
        // backswing direction; anything on the other side of rest from then
        // on reads as angle 0 instead of contributing to the peak. Only the
        // very first excursion of a calibration is unprotected (no direction
        // latched yet) - assumed to be a real backswing, not a wobble.
        float directionLockDegrees = 4.0f;

        // Autocal (WaitingForCalibration): rather than requiring a tap, watch
        // for the gravity Z component (device frame) dropping below this
        // while held still - the watch tilts from face-up viewing position
        // (Z > 0.9 measured) to putting position (Z ~0.125 measured) when the
        // golfer addresses the ball. tap() still works as a manual override.
        float autocalNotFlatZ = 0.25f;
        // How long that not-flat position must be held still (reusing
        // stillnessToleranceDegrees as the drift gate) before it's accepted
        // as the calibrated rest - about how long a golfer naturally pauses
        // at address.
        uint32_t autocalStillnessMs = 3000;
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
    // Fired once per swing when ForwardStroke ends (-> Recovering), carrying
    // that swing's peak backswing angle in degrees. Lets the renderer speak
    // it; this class does no rounding/clamping to the speakable range, that's
    // a renderer/UI concern (see Clicker::maxSpokenNumber).
    std::function<void(float)> onSwingComplete;

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

    // --- autocal (WaitingForCalibration) ---
    bool haveAutocalRef_ = false;
    Vec3 autocalRefGravity_{0, 0, 0};   // resets the window whenever drift exceeds tolerance
    uint32_t autocalWindowStartMs_ = 0;
    Vec3 autocalSum_{0, 0, 0};
    int autocalCount_ = 0;

    uint32_t doneEnteredMs_ = 0;

    // --- references ---
    Vec3 restGravity_{0, 0, 1};          // recentred after each settled swing
    Vec3 initialRestGravity_{0, 0, 1};   // fixed at calibration; drift check for re-center

    // --- live swing state ---
    float swingAngleDegrees_ = 0.0f;
    int lastClickBucket_ = 0;
    int clickCount_ = 0;
    float peakAngleThisSwing_ = 0.0f;

    // --- backswing direction lock (see Config::directionLockDegrees) ---
    bool haveDirectionRef_ = false;
    Vec3 directionRef_{0, 0, 0};  // g - restGravity_ at the moment of the lock

    uint32_t forwardStrokeStartMs_ = 0;

    bool haveStillnessRef_ = false;
    Vec3 stillnessRefGravity_{0, 0, 0};
    uint32_t stillnessWindowStartMs_ = 0;
};
