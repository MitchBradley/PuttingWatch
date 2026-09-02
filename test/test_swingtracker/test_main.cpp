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

    // Backswing out to 25 deg: crossings at 6/12/18/24 -> 4 clicks.
    int clicks = 0;
    t.onClick = [&] { clicks++; };
    ramp(t, 0.0f, 25.0f, 60, clock);
    TEST_ASSERT_EQUAL_INT(4, clicks);
    TEST_ASSERT_EQUAL_INT(4, t.clickCount());
    TEST_ASSERT_TRUE(t.swingPhase() == SwingTracker::SwingPhase::Backswing);

    // Come back down. Clicking is off the monotonic peak, so the descent
    // fires NOTHING - no stray "click twice" as the angle re-crosses a
    // threshold before reversalDrop trips. Reversal (>=2 deg below the peak)
    // still switches to ForwardStroke.
    ramp(t, 25.0f, 0.0f, 60, clock);
    TEST_ASSERT_EQUAL_INT(4, clicks);
    TEST_ASSERT_TRUE(t.swingPhase() == SwingTracker::SwingPhase::ForwardStroke);
    TEST_ASSERT_FALSE(t.isReadyForSwing());

    int clicksAfterReversal = clicks;
    ramp(t, 0.0f, 25.0f, 60, clock);  // a "swing" during forward-stroke: ignored
    TEST_ASSERT_EQUAL_INT(clicksAfterReversal, clicks);
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

    // Hold still back at address: 2 s to end the swing + 2 s stillness -> re-arm.
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

void test_click_threshold_clamped(void) {
    SwingTracker t;
    t.setClickThresholdDegrees(0.5f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, t.clickThresholdDegrees());
    t.setClickThresholdDegrees(99.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, t.clickThresholdDegrees());
    t.setClickThresholdDegrees(7.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.0f, t.clickThresholdDegrees());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_calibration_flow);
    RUN_TEST(test_backswing_clicks_then_reversal);
    RUN_TEST(test_small_swing_still_reverses);
    RUN_TEST(test_boundary_jitter_single_click);
    RUN_TEST(test_rearm_when_settled_near_original_rest);
    RUN_TEST(test_no_rearm_when_settled_far_from_original_rest);
    RUN_TEST(test_recal_resets_everything);
    RUN_TEST(test_click_threshold_clamped);
    return UNITY_END();
}
