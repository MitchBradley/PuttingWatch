// Host-side unit tests for the pure swing logic (lib/PuttingCore).
//   pio test -e native
//
// These exercise SwingTracker the way the firmware will: a steady stream of
// MotionSample at 100 Hz, timing carried in sample.t_ms. Gravity vectors are
// synthesised by rotating the rest vector (0,0,1) about the X axis by a known
// tilt, so angleBetweenDegrees(g, rest) == that tilt exactly.

#include <unity.h>
#include <cmath>
#include "SwingTracker.h"

static constexpr uint32_t DT_MS = 10;  // 100 Hz

// Gravity unit vector tilted `deg` about X from the rest vector (0,0,1).
static MotionSample sampleAtTilt(float deg, uint32_t t_ms) {
    float r = deg * 3.14159265f / 180.0f;
    MotionSample s;
    s.t_ms = t_ms;
    s.gravity[0] = 0.0f;
    s.gravity[1] = -std::sin(r);
    s.gravity[2] = std::cos(r);
    return s;
}

// Push `count` samples at a fixed tilt, advancing the clock.
static void hold(SwingTracker& t, float deg, int count, uint32_t& clock) {
    for (int i = 0; i < count; i++) {
        t.update(sampleAtTilt(deg, clock));
        clock += DT_MS;
    }
}

// Push a linear tilt ramp from `fromDeg` to `toDeg` over `count` samples.
static void ramp(SwingTracker& t, float fromDeg, float toDeg, int count, uint32_t& clock) {
    for (int i = 0; i < count; i++) {
        float f = (count > 1) ? float(i) / (count - 1) : 1.0f;
        t.update(sampleAtTilt(fromDeg + (toDeg - fromDeg) * f, clock));
        clock += DT_MS;
    }
}

// Calibrate (tap + hold still) and advance into Monitoring. Leaves the clock
// just past the point Monitoring began.
static void calibrateIntoMonitoring(SwingTracker& t, uint32_t& clock) {
    t.tap();
    hold(t, 0.0f, 260, clock);  // 2.6 s still -> Accumulating finishes -> Done
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::Done ||
                     t.state() == SwingTracker::State::Monitoring);
    hold(t, 0.0f, 230, clock);  // 2.3 s more -> Done auto-advances to Monitoring
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::Monitoring);
}

void test_calibration_flow(void) {
    SwingTracker t;
    uint32_t clock = 0;
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::WaitingForCalibration);

    t.tap();
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::Accumulating);

    hold(t, 0.0f, 150, clock);  // 1.5 s - not enough yet
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::Accumulating);

    hold(t, 0.0f, 120, clock);  // now past 2 s
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::Done);

    hold(t, 0.0f, 210, clock);  // past the 2 s monitoring delay
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::Monitoring);
    TEST_ASSERT_TRUE(t.isReadyForSwing());
}

void test_backswing_clicks_then_reversal(void) {
    SwingTracker t;
    t.setClickThresholdDegrees(6.0f);
    uint32_t clock = 0;
    calibrateIntoMonitoring(t, clock);

    // Backswing out to 25 deg: crossings at 6/12/18/24 would be 4 clicks,
    // but maxClicksPerSwing (default 3) caps it at 6/12/18 - a swing that
    // fast isn't a good stroke, no point clicking every threshold past that.
    int clicks = 0;
    t.onClick = [&] { clicks++; };
    ramp(t, 0.0f, 25.0f, 60, clock);
    TEST_ASSERT_EQUAL_INT(3, clicks);
    TEST_ASSERT_EQUAL_INT(3, t.clickCount());
    TEST_ASSERT_TRUE(t.swingPhase() == SwingTracker::SwingPhase::Backswing);

    // Come back down. Clicking is off the monotonic peak, so the descent
    // fires NOTHING - no stray "click twice" as the angle re-crosses a
    // threshold before reversalDrop trips. Reversal (>=2 deg below the peak)
    // still switches to ForwardStroke - the cap doesn't touch reversal
    // detection at all, that's driven by the continuous peak, not the
    // (now capped) click bucket.
    ramp(t, 25.0f, 0.0f, 60, clock);
    TEST_ASSERT_EQUAL_INT(3, clicks);
    TEST_ASSERT_TRUE(t.swingPhase() == SwingTracker::SwingPhase::ForwardStroke);
    TEST_ASSERT_FALSE(t.isReadyForSwing());

    int clicksAfterReversal = clicks;
    ramp(t, 0.0f, 25.0f, 60, clock);  // a "swing" during forward-stroke: ignored
    TEST_ASSERT_EQUAL_INT(clicksAfterReversal, clicks);
}

// The cap is per swing ATTEMPT, not lifetime: an oversized backswing still
// stops at 3, but re-arming resets it, so the next stroke can click up to 3
// again too.
void test_max_clicks_per_swing_cap_resets_on_rearm(void) {
    SwingTracker t;
    t.setClickThresholdDegrees(6.0f);
    uint32_t clock = 0;
    calibrateIntoMonitoring(t, clock);

    int clicks = 0;
    t.onClick = [&] { clicks++; };

    // Way oversized: 6/12/18/24/30/36 would be 6 crossings, capped to 3.
    ramp(t, 0.0f, 40.0f, 80, clock);
    TEST_ASSERT_EQUAL_INT(3, clicks);

    ramp(t, 40.0f, 0.0f, 60, clock);
    hold(t, 0.0f, 450, clock);  // 2 s end-of-swing + 1 s stillness -> re-arm
    TEST_ASSERT_TRUE(t.isReadyForSwing());

    ramp(t, 0.0f, 25.0f, 60, clock);  // ordinary swing, same 3-click cap applies fresh
    TEST_ASSERT_EQUAL_INT(6, clicks);  // 3 from before + 3 more, not stuck at 3
}

// A swing whose peak never reaches one full threshold step must still reverse
// (the bug fixed in Swift commit 3bd9688).
void test_small_swing_still_reverses(void) {
    SwingTracker t;
    t.setClickThresholdDegrees(12.0f);
    uint32_t clock = 0;
    calibrateIntoMonitoring(t, clock);

    int clicks = 0;
    t.onClick = [&] { clicks++; };
    ramp(t, 0.0f, 4.0f, 30, clock);   // peak 4 deg, never crosses 12
    ramp(t, 4.0f, 1.0f, 30, clock);   // drop 3 deg below peak

    TEST_ASSERT_EQUAL_INT(0, clicks);
    TEST_ASSERT_TRUE(t.swingPhase() == SwingTracker::SwingPhase::ForwardStroke);
}

// The on-course "ready, then a real backswing produces no clicks" report: a
// tiny settle-into-address wobble right after re-arm (a couple of degrees of
// hand tremor, well under clickThresholdDegreesMin) rising and falling back
// to rest must NOT be read as a completed swing - reversalDropDegrees (2 deg)
// alone is small enough that ordinary settling motion satisfies it, which
// used to swallow the entire real stroke that followed (stuck in Recovering,
// clicking impossible, confirmed via on-device EVT logging).
void test_settle_wobble_does_not_complete_phantom_swing(void) {
    SwingTracker t;
    t.setClickThresholdDegrees(6.0f);
    uint32_t clock = 0;
    calibrateIntoMonitoring(t, clock);

    int clicks = 0;
    t.onClick = [&] { clicks++; };

    // Settle wobble: peak 2.1 deg, well under clickThresholdDegreesMin (3.0).
    ramp(t, 0.0f, 2.1f, 20, clock);
    ramp(t, 2.1f, 0.0f, 20, clock);
    TEST_ASSERT_TRUE(t.swingPhase() == SwingTracker::SwingPhase::Backswing);
    TEST_ASSERT_TRUE(t.isReadyForSwing());
    TEST_ASSERT_EQUAL_INT(0, clicks);

    // The real backswing that follows must still work normally - not
    // swallowed by a phantom cycle the wobble would otherwise have started.
    ramp(t, 0.0f, 25.0f, 60, clock);
    TEST_ASSERT_EQUAL_INT(3, clicks);
    ramp(t, 25.0f, 0.0f, 40, clock);
    TEST_ASSERT_TRUE(t.swingPhase() == SwingTracker::SwingPhase::ForwardStroke);
}

// Jitter/dither across a threshold boundary must not double-click - the
// on-device "can't get just one click, it clicks twice" report.
void test_boundary_jitter_single_click(void) {
    SwingTracker t;
    t.setClickThresholdDegrees(6.0f);
    uint32_t clock = 0;
    calibrateIntoMonitoring(t, clock);

    int clicks = 0;
    t.onClick = [&] { clicks++; };

    ramp(t, 0.0f, 7.0f, 25, clock);   // cross 6 deg once
    for (int i = 0; i < 6; i++) {     // wobble either side of 6 deg
        ramp(t, 7.0f, 5.0f, 8, clock);
        ramp(t, 5.0f, 7.0f, 8, clock);
    }
    TEST_ASSERT_EQUAL_INT(1, clicks);

    ramp(t, 7.0f, 0.0f, 40, clock);   // reverse home
    TEST_ASSERT_EQUAL_INT(1, clicks);
    TEST_ASSERT_TRUE(t.swingPhase() == SwingTracker::SwingPhase::ForwardStroke);
}

void test_rearm_when_settled_near_original_rest(void) {
    SwingTracker t;
    t.setClickThresholdDegrees(6.0f);
    uint32_t clock = 0;
    calibrateIntoMonitoring(t, clock);

    ramp(t, 0.0f, 25.0f, 60, clock);
    ramp(t, 25.0f, 0.0f, 40, clock);
    TEST_ASSERT_TRUE(t.swingPhase() == SwingTracker::SwingPhase::ForwardStroke);

    // Hold still back at address: 2 s to end the swing + 1 s stillness -> re-arm.
    hold(t, 0.0f, 450, clock);
    TEST_ASSERT_TRUE(t.swingPhase() == SwingTracker::SwingPhase::Backswing);
    TEST_ASSERT_TRUE(t.isReadyForSwing());
}

void test_no_rearm_when_settled_far_from_original_rest(void) {
    SwingTracker t;
    t.setClickThresholdDegrees(6.0f);
    uint32_t clock = 0;
    calibrateIntoMonitoring(t, clock);

    ramp(t, 0.0f, 25.0f, 60, clock);
    // Jump to 20 deg and hold there: reverses (20 <= 25-2), but 20 deg is well
    // outside the 10 deg re-center drift gate, so it must NOT re-arm.
    hold(t, 20.0f, 600, clock);
    TEST_ASSERT_TRUE(t.swingPhase() == SwingTracker::SwingPhase::Recovering);
    TEST_ASSERT_FALSE(t.isReadyForSwing());
}

void test_recal_resets_everything(void) {
    SwingTracker t;
    uint32_t clock = 0;
    calibrateIntoMonitoring(t, clock);
    ramp(t, 0.0f, 15.0f, 40, clock);
    TEST_ASSERT_TRUE(t.clickCount() > 0);

    t.recal();
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::WaitingForCalibration);
    TEST_ASSERT_EQUAL_INT(0, t.clickCount());
    TEST_ASSERT_FALSE(t.isReadyForSwing());
}

// The on-course bug report: a small wobble in the OPPOSITE direction from the
// real backswing (e.g. at address, before the golfer starts the real stroke)
// must not be mistaken for a completed swing. Without the direction lock,
// angleBetweenDegrees is unsigned - the wobble raises peakAngleThisSwing_,
// returning toward rest satisfies the reversal check, and the state machine
// jumps to ForwardStroke (clicking suppressed) right before the real
// backswing, swallowing it. With the lock (established off the first real
// swing, which is assumed to clear directionLockDegrees), an opposite-
// direction excursion instead reads as angle 0 and can't touch phase or peak.
// Autocal: no tap() needed. Gravity Z (device frame) reads > 0.9 flat/viewing
// on the wrist and ~0.125 in putting address (measured on hardware) - tilting
// past autocalNotFlatZ (0.25) and holding still there for autocalStillnessMs
// should calibrate on its own, the same way tap()+hold used to require an
// explicit tap.
void test_autocal_fires_after_notflat_still(void) {
    SwingTracker t;
    uint32_t clock = 0;
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::WaitingForCalibration);

    hold(t, 0.0f, 100, clock);  // flat/viewing for 1 s - must not arm
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::WaitingForCalibration);

    // ~83 deg tilt -> gravity.z ~= cos(83deg) ~= 0.122, well past the 0.25
    // not-flat threshold - stand-in for real putting-position address.
    hold(t, 83.0f, 310, clock);  // 3.1 s held still there -> past autocalStillnessMs (3 s)
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::Done);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, std::cos(83.0f * 3.14159265f / 180.0f), t.restGravity().z);

    hold(t, 83.0f, 210, clock);  // past the 2 s Done->Monitoring delay
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::Monitoring);
    TEST_ASSERT_TRUE(t.isReadyForSwing());
}

void test_autocal_does_not_fire_while_flat(void) {
    SwingTracker t;
    uint32_t clock = 0;
    hold(t, 0.0f, 500, clock);  // 5 s flat/viewing, well past autocalStillnessMs
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::WaitingForCalibration);
}

// A disturbance mid-window (still not flat, but a big jump - e.g. adjusting
// grip before settling into address) must restart the stillness window
// rather than let the earlier, now-stale position complete calibration.
void test_autocal_window_restarts_on_motion(void) {
    SwingTracker t;
    uint32_t clock = 0;

    hold(t, 83.0f, 150, clock);  // 1.5 s - not enough yet
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::WaitingForCalibration);

    hold(t, 88.0f, 310, clock);  // jumps to a different not-flat tilt and settles there
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::Done);
    // Calibrated against the 88 deg position it actually settled at, not 83.
    TEST_ASSERT_FLOAT_WITHIN(0.02f, std::cos(88.0f * 3.14159265f / 180.0f), t.restGravity().z);
}

// tap() remains a live manual override even mid-autocal-window.
void test_tap_overrides_autocal_mid_window(void) {
    SwingTracker t;
    uint32_t clock = 0;
    hold(t, 83.0f, 150, clock);  // mid-window, autocal hasn't fired yet
    t.tap();
    TEST_ASSERT_TRUE(t.state() == SwingTracker::State::Accumulating);
}

// onSwingComplete fires exactly once per swing, at the ForwardStroke ->
// Recovering transition, carrying that swing's peak backswing angle.
void test_swing_complete_fires_with_peak_angle(void) {
    SwingTracker t;
    t.setClickThresholdDegrees(6.0f);
    uint32_t clock = 0;
    calibrateIntoMonitoring(t, clock);

    int fireCount = 0;
    float lastPeak = -1.0f;
    t.onSwingComplete = [&](float peakDeg) {
        fireCount++;
        lastPeak = peakDeg;
    };

    ramp(t, 0.0f, 25.0f, 60, clock);
    ramp(t, 25.0f, 0.0f, 40, clock);
    TEST_ASSERT_TRUE(t.swingPhase() == SwingTracker::SwingPhase::ForwardStroke);
    TEST_ASSERT_EQUAL_INT(0, fireCount);  // not yet - ForwardStroke hasn't ended

    hold(t, 0.0f, 210, clock);  // past endOfSwingDelayMs -> ForwardStroke ends
    TEST_ASSERT_EQUAL_INT(1, fireCount);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 25.0f, lastPeak);

    // Continue through re-arm and a second, smaller swing - fires again with
    // the NEW peak, not the stale one, and still just once.
    hold(t, 0.0f, 250, clock);  // finish the 1 s stillness -> re-arm
    TEST_ASSERT_TRUE(t.isReadyForSwing());

    ramp(t, 0.0f, 10.0f, 40, clock);
    ramp(t, 10.0f, 0.0f, 40, clock);
    hold(t, 0.0f, 210, clock);
    TEST_ASSERT_EQUAL_INT(2, fireCount);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 10.0f, lastPeak);
}

void test_direction_lock_ignores_opposite_wobble(void) {
    SwingTracker t;
    t.setClickThresholdDegrees(6.0f);
    uint32_t clock = 0;
    calibrateIntoMonitoring(t, clock);

    int clicks = 0;
    t.onClick = [&] { clicks++; };

    // First real backswing: establishes the direction lock (peak clears the
    // default 4 deg directionLockDegrees) and clicks normally.
    ramp(t, 0.0f, 25.0f, 60, clock);
    TEST_ASSERT_EQUAL_INT(3, clicks);
    ramp(t, 25.0f, 0.0f, 40, clock);
    hold(t, 0.0f, 450, clock);  // 2 s end-of-swing + 1 s stillness -> re-arm
    TEST_ASSERT_TRUE(t.isReadyForSwing());

    // A wobble in the OPPOSITE direction, well under the click threshold -
    // the exact shape of the reported false-click-suppression scenario. Must
    // not raise the peak, trigger a reversal, or leave Backswing at all.
    ramp(t, 0.0f, -3.0f, 30, clock);
    ramp(t, -3.0f, 0.0f, 30, clock);
    TEST_ASSERT_TRUE(t.swingPhase() == SwingTracker::SwingPhase::Backswing);
    TEST_ASSERT_TRUE(t.isReadyForSwing());
    TEST_ASSERT_EQUAL_INT(3, clicks);

    // The real second backswing must still click normally - it wasn't
    // silently swallowed by a false ForwardStroke from the wobble.
    ramp(t, 0.0f, 25.0f, 60, clock);
    TEST_ASSERT_EQUAL_INT(6, clicks);
}

void test_click_threshold_clamped(void) {
    SwingTracker t;
    t.setClickThresholdDegrees(0.5f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, t.clickThresholdDegrees());
    t.setClickThresholdDegrees(99.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 16.5f, t.clickThresholdDegrees());
    t.setClickThresholdDegrees(7.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.0f, t.clickThresholdDegrees());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_calibration_flow);
    RUN_TEST(test_backswing_clicks_then_reversal);
    RUN_TEST(test_max_clicks_per_swing_cap_resets_on_rearm);
    RUN_TEST(test_small_swing_still_reverses);
    RUN_TEST(test_settle_wobble_does_not_complete_phantom_swing);
    RUN_TEST(test_boundary_jitter_single_click);
    RUN_TEST(test_rearm_when_settled_near_original_rest);
    RUN_TEST(test_no_rearm_when_settled_far_from_original_rest);
    RUN_TEST(test_autocal_fires_after_notflat_still);
    RUN_TEST(test_autocal_does_not_fire_while_flat);
    RUN_TEST(test_autocal_window_restarts_on_motion);
    RUN_TEST(test_tap_overrides_autocal_mid_window);
    RUN_TEST(test_swing_complete_fires_with_peak_angle);
    RUN_TEST(test_direction_lock_ignores_opposite_wobble);
    RUN_TEST(test_recal_resets_everything);
    RUN_TEST(test_click_threshold_clamped);
    return UNITY_END();
}
