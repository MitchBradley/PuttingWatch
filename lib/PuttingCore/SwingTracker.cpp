#include "SwingTracker.h"

void SwingTracker::setClickThresholdDegrees(float deg) {
    if (deg < cfg_.clickThresholdDegreesMin) deg = cfg_.clickThresholdDegreesMin;
    if (deg > cfg_.clickThresholdDegreesMax) deg = cfg_.clickThresholdDegreesMax;
    clickThresholdDegrees_ = deg;
}

void SwingTracker::tap() {
    if (state_ != State::WaitingForCalibration) return;
    state_ = State::Accumulating;
    accumPending_ = true;  // first Accumulating sample seeds the window + sums
}

void SwingTracker::recal() {
    state_ = State::WaitingForCalibration;
    swingPhase_ = SwingPhase::Backswing;
    accumPending_ = false;
    accumSum_ = Vec3{0, 0, 0};
    accumCount_ = 0;
    haveAutocalRef_ = false;
    autocalRefGravity_ = Vec3{0, 0, 0};
    autocalWindowStartMs_ = 0;
    autocalSum_ = Vec3{0, 0, 0};
    autocalCount_ = 0;
    swingAngleDegrees_ = 0.0f;
    lastClickBucket_ = 0;
    clickCount_ = 0;
    peakAngleThisSwing_ = 0.0f;
    haveDirectionRef_ = false;
    directionRef_ = Vec3{0, 0, 0};
    forwardStrokeStartMs_ = 0;
    haveStillnessRef_ = false;
}

void SwingTracker::update(const MotionSample& s) {
    const Vec3 g(s.gravity);

    switch (state_) {
        // Autocal: rather than requiring tap(), watch for the watch tilting
        // out of face-up viewing position (gravity Z drops below
        // autocalNotFlatZ) and being held still there - that's the golfer at
        // address. tap() remains a live manual override, handled separately
        // (it jumps straight to Accumulating, bypassing this entirely).
        case State::WaitingForCalibration: {
            if (g.z >= cfg_.autocalNotFlatZ) {
                // Still flat/viewing - no candidate window in progress.
                haveAutocalRef_ = false;
                break;
            }
            const float driftFromRef =
                haveAutocalRef_ ? angleBetweenDegrees(g, autocalRefGravity_) : 0.0f;
            if (!haveAutocalRef_ || driftFromRef > cfg_.stillnessToleranceDegrees) {
                // (Re)start the window from here.
                haveAutocalRef_ = true;
                autocalRefGravity_ = g;
                autocalWindowStartMs_ = s.t_ms;
                autocalSum_ = g;
                autocalCount_ = 1;
                break;
            }
            autocalSum_ = autocalSum_ + g;
            autocalCount_++;
            if (s.t_ms - autocalWindowStartMs_ >= cfg_.autocalStillnessMs) {
                restGravity_ = (autocalSum_ * (1.0f / autocalCount_)).normalized();
                initialRestGravity_ = restGravity_;
                state_ = State::Done;
                doneEnteredMs_ = s.t_ms;
                haveAutocalRef_ = false;
                if (onCalibrationComplete) onCalibrationComplete();
            }
            break;
        }

        case State::Accumulating: {
            if (accumPending_) {
                accumPending_ = false;
                accumStartMs_ = s.t_ms;
                accumSum_ = Vec3{0, 0, 0};
                accumCount_ = 0;
            }
            accumSum_ = accumSum_ + g;
            accumCount_++;
            if (s.t_ms - accumStartMs_ >= cfg_.accumulationMs) {
                if (accumCount_ > 0) {
                    restGravity_ = (accumSum_ * (1.0f / accumCount_)).normalized();
                    initialRestGravity_ = restGravity_;
                    state_ = State::Done;
                    doneEnteredMs_ = s.t_ms;
                    if (onCalibrationComplete) onCalibrationComplete();
                } else {
                    // No samples arrived - shouldn't happen when update() is
                    // the one pumping them, but fall back rather than divide
                    // by zero.
                    state_ = State::WaitingForCalibration;
                }
            }
            break;
        }

        case State::Done:
            if (s.t_ms - doneEnteredMs_ >= cfg_.monitoringDelayMs) {
                enterMonitoring();
            }
            break;

        case State::Monitoring:
            processMonitoringSample(s);
            break;
    }
}

void SwingTracker::enterMonitoring() {
    state_ = State::Monitoring;
    swingPhase_ = SwingPhase::Backswing;
    swingAngleDegrees_ = 0.0f;
    lastClickBucket_ = 0;
    clickCount_ = 0;
    peakAngleThisSwing_ = 0.0f;
    haveDirectionRef_ = false;
    directionRef_ = Vec3{0, 0, 0};
    forwardStrokeStartMs_ = 0;
    haveStillnessRef_ = false;
    if (onReady) onReady();
}

// Ported from CalibrationManager.processMonitoringSample. `angle` is the
// unsigned angle between current gravity and the calibrated rest gravity - no
// need to identify which axis the rotation is about, the vector angle
// captures it directly (the on-the-spot simplification the user made in the
// Swift version instead of empirical axis detection).
void SwingTracker::processMonitoringSample(const MotionSample& s) {
    const Vec3 g(s.gravity);
    const float angle = angleBetweenDegrees(g, restGravity_);
    swingAngleDegrees_ = angle;

    switch (swingPhase_) {
        case SwingPhase::Backswing: {
            // angle alone can't tell a real backswing from a wobble in the
            // opposite direction - it's an unsigned distance from rest. Once
            // a swing this calibration has gone far enough to be trusted as
            // the real backswing direction, latch it; from then on, motion
            // on the other side of rest reads as 0 instead of raising the
            // peak (see Config::directionLockDegrees).
            const Vec3 deviation = g - restGravity_;
            float effectiveAngle = angle;
            if (!haveDirectionRef_) {
                if (angle >= cfg_.directionLockDegrees) {
                    directionRef_ = deviation;
                    haveDirectionRef_ = true;
                    if (onDirectionLocked) onDirectionLocked(angle);
                }
            } else if (deviation.dot(directionRef_) <= 0.0f) {
                effectiveAngle = 0.0f;
            }

            if (effectiveAngle > peakAngleThisSwing_) peakAngleThisSwing_ = effectiveAngle;

            // Click off the MONOTONIC peak, not the instantaneous angle:
            // - never fires on the way back down (the angle re-crossing a
            //   threshold before reversalDrop trips was the "clicks twice"),
            // - immune to jitter dithering across a threshold boundary,
            // - still fires at the same instant on the way up (peak == angle
            //   while rising).
            // Loop so a fast swing that jumps several thresholds in one
            // sample still emits one click per threshold crossed - but capped
            // at maxClicksPerSwing (lastClickBucket_ is already exactly "how
            // many clicks this backswing attempt has fired", since it resets
            // on every re-arm, so gating on it directly is enough - no
            // separate counter needed).
            const int peakBucket = static_cast<int>(peakAngleThisSwing_ / clickThresholdDegrees_);
            while (lastClickBucket_ < peakBucket && lastClickBucket_ < cfg_.maxClicksPerSwing) {
                lastClickBucket_++;
                clickCount_++;
                if (onClick) onClick();
            }

            // Reversal is measured from the CONTINUOUS peak, not from the
            // click-bucket value: a swing whose peak never reaches one full
            // threshold step would leave the bucket stuck at 0 and (since the
            // vector angle can't go negative) reversal could never trigger -
            // the phase machine would never leave Backswing and clicks would
            // fire straight through the follow-through too. (Bug fixed in the
            // Swift app, commit 3bd9688.)
            //
            // But peak alone isn't enough of a gate on its own: reversalDrop
            // (2 deg) is small enough that ordinary settling-into-address
            // motion right after a re-arm - just a couple of degrees of hand
            // tremor rising and falling back to rest - satisfies it on its
            // own. That completes a whole phantom Backswing -> ForwardStroke
            // -> Recovering cycle before the golfer's real stroke even
            // starts, leaving the tracker stuck in Recovering (clicking
            // impossible) through the entire real swing that follows -
            // silent misses, confirmed on-device via the EVT log. Gate on
            // clickThresholdDegreesMin too: a swing too small to ever fire
            // even the most sensitive click is indistinguishable from noise,
            // so don't let it complete a reversal at all - just keep
            // tracking Backswing, and a real swing that follows continues
            // growing the same peak normally.
            if (peakAngleThisSwing_ >= cfg_.clickThresholdDegreesMin &&
                effectiveAngle <= peakAngleThisSwing_ - cfg_.reversalDropDegrees) {
                swingPhase_ = SwingPhase::ForwardStroke;
                forwardStrokeStartMs_ = s.t_ms;
            }
            break;
        }

        case SwingPhase::ForwardStroke:
            // Clicking suppressed on the way back down. Once the reversal has
            // held for endOfSwingDelay, declare the swing over and start
            // watching for stillness.
            if (s.t_ms - forwardStrokeStartMs_ >= cfg_.endOfSwingDelayMs) {
                swingPhase_ = SwingPhase::Recovering;
                stillnessRefGravity_ = g;
                haveStillnessRef_ = true;
                stillnessWindowStartMs_ = s.t_ms;
                if (onSwingComplete) onSwingComplete(peakAngleThisSwing_);
            }
            break;

        case SwingPhase::Recovering: {
            if (!haveStillnessRef_) {
                stillnessRefGravity_ = g;
                haveStillnessRef_ = true;
                stillnessWindowStartMs_ = s.t_ms;
                break;
            }
            const float driftFromRef = angleBetweenDegrees(g, stillnessRefGravity_);
            if (driftFromRef > cfg_.stillnessToleranceDegrees) {
                // Moved too much to count as still - restart the window from
                // here, not from wherever it began.
                stillnessRefGravity_ = g;
                stillnessWindowStartMs_ = s.t_ms;
            } else if (s.t_ms - stillnessWindowStartMs_ >= cfg_.stillnessDurationMs) {
                // Held still long enough. Only re-center (and re-arm) if the
                // settled position is also close to the ORIGINAL calibrated
                // rest - otherwise a stroke that finished somewhere off
                // address would silently become the new baseline and drift
                // the reference further with every swing.
                const float driftFromInitial = angleBetweenDegrees(g, initialRestGravity_);
                if (driftFromInitial <= cfg_.restRecenterMaxDriftDegrees) {
                    restGravity_ = g;
                    swingPhase_ = SwingPhase::Backswing;
                    lastClickBucket_ = 0;
                    peakAngleThisSwing_ = 0.0f;
                    forwardStrokeStartMs_ = 0;
                    haveStillnessRef_ = false;
                    if (onReady) onReady();
                }
                // else: still, but too far from the original rest to trust.
                // Stay in Recovering; re-checked every sample, so it re-arms
                // as soon as both conditions hold.
            }
            break;
        }
    }
}
