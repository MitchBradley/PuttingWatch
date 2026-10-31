//! Port of the C++ firmware's test/test_swingtracker (Unity), branch `old-cpp-version`. Same scenarios, same numbers:
//! a steady 100 Hz MotionSample stream whose gravity is the rest vector (0,0,1) rotated about X
//! by a known tilt, so angle_between(g, rest) == that tilt exactly.

use putting_core::{Event, MotionSample, State, SwingPhase, SwingTracker, Vec3};

const DT_MS: u32 = 10; // 100 Hz

fn sample_at_tilt(deg: f32, t_ms: u32) -> MotionSample {
    let r = deg.to_radians();
    MotionSample { t_ms, gravity: Vec3::new(0.0, -r.sin(), r.cos()), ..Default::default() }
}

struct Rig {
    t: SwingTracker,
    clock: u32,
    log: Vec<Event>,
}

impl Rig {
    fn new() -> Self {
        Rig { t: SwingTracker::default(), clock: 0, log: Vec::new() }
    }

    fn feed(&mut self, deg: f32) {
        let ev = self.t.update(&sample_at_tilt(deg, self.clock));
        self.log.extend(ev);
        self.clock += DT_MS;
    }

    fn hold(&mut self, deg: f32, count: usize) {
        for _ in 0..count {
            self.feed(deg);
        }
    }

    /// Linear tilt ramp from `from` to `to` over `count` samples.
    fn ramp(&mut self, from: f32, to: f32, count: usize) {
        for i in 0..count {
            let f = if count > 1 { i as f32 / (count - 1) as f32 } else { 1.0 };
            self.feed(from + (to - from) * f);
        }
    }

    /// Calibrate (tap + hold still) and advance into Monitoring.
    fn calibrate_into_monitoring(&mut self) {
        self.t.tap();
        self.hold(0.0, 260); // 2.6 s still -> Accumulating finishes -> Done
        assert!(matches!(self.t.state(), State::Done | State::Monitoring));
        self.hold(0.0, 230); // Done auto-advances to Monitoring
        assert_eq!(self.t.state(), State::Monitoring);
        self.log.clear();
    }

    fn clicks(&self) -> usize {
        self.log.iter().filter(|e| **e == Event::Click).count()
    }

    fn swing_completes(&self) -> Vec<f32> {
        self.log
            .iter()
            .filter_map(|e| if let Event::SwingComplete(p) = e { Some(*p) } else { None })
            .collect()
    }
}

fn rig_monitoring(threshold: f32) -> Rig {
    let mut r = Rig::new();
    r.t.set_click_threshold_degrees(threshold);
    r.calibrate_into_monitoring();
    r
}

#[test]
fn calibration_flow() {
    let mut r = Rig::new();
    assert_eq!(r.t.state(), State::WaitingForCalibration);

    r.t.tap();
    assert_eq!(r.t.state(), State::Accumulating);

    r.hold(0.0, 150); // 1.5 s - not enough yet
    assert_eq!(r.t.state(), State::Accumulating);

    r.hold(0.0, 120); // now past 2 s
    assert_eq!(r.t.state(), State::Done);

    r.hold(0.0, 210); // past the 2 s monitoring delay
    assert_eq!(r.t.state(), State::Monitoring);
    assert!(r.t.is_ready_for_swing());
}

#[test]
fn backswing_clicks_then_reversal() {
    let mut r = rig_monitoring(6.0);

    // Crossings at 6/12/18/24 would be 4 clicks, capped to 3 by max_clicks_per_swing.
    r.ramp(0.0, 25.0, 60);
    assert_eq!(r.clicks(), 3);
    assert_eq!(r.t.click_count(), 3);
    assert_eq!(r.t.swing_phase(), SwingPhase::Backswing);

    // Descent fires nothing (clicks come off the monotonic peak); reversal still happens.
    r.ramp(25.0, 0.0, 60);
    assert_eq!(r.clicks(), 3);
    assert_eq!(r.t.swing_phase(), SwingPhase::ForwardStroke);
    assert!(!r.t.is_ready_for_swing());

    r.ramp(0.0, 25.0, 60); // a "swing" during forward-stroke: ignored
    assert_eq!(r.clicks(), 3);
}

// The cap is per swing ATTEMPT, not lifetime.
#[test]
fn max_clicks_cap_resets_on_rearm() {
    let mut r = rig_monitoring(6.0);

    r.ramp(0.0, 40.0, 80); // six crossings, capped to 3
    assert_eq!(r.clicks(), 3);

    r.ramp(40.0, 0.0, 60);
    r.hold(0.0, 450); // 2 s end-of-swing + 1 s stillness -> re-arm
    assert!(r.t.is_ready_for_swing());

    r.ramp(0.0, 25.0, 60);
    assert_eq!(r.clicks(), 6); // 3 + 3 more, not stuck at 3
}

// A swing whose peak never reaches one full threshold step must still reverse.
#[test]
fn small_swing_still_reverses() {
    let mut r = rig_monitoring(12.0);

    r.ramp(0.0, 4.0, 30); // peak 4 deg, never crosses 12
    r.ramp(4.0, 1.0, 30); // 3 deg below peak

    assert_eq!(r.clicks(), 0);
    assert_eq!(r.t.swing_phase(), SwingPhase::ForwardStroke);
}

// Settle-into-address wobble (well under click_threshold_degrees_min) must not complete a
// phantom swing that swallows the real stroke.
#[test]
fn settle_wobble_does_not_complete_phantom_swing() {
    let mut r = rig_monitoring(6.0);

    r.ramp(0.0, 2.1, 20);
    r.ramp(2.1, 0.0, 20);
    assert_eq!(r.t.swing_phase(), SwingPhase::Backswing);
    assert!(r.t.is_ready_for_swing());
    assert_eq!(r.clicks(), 0);

    r.ramp(0.0, 25.0, 60);
    assert_eq!(r.clicks(), 3);
    r.ramp(25.0, 0.0, 40);
    assert_eq!(r.t.swing_phase(), SwingPhase::ForwardStroke);
}

// Jitter across a threshold boundary must not double-click.
#[test]
fn boundary_jitter_single_click() {
    let mut r = rig_monitoring(6.0);

    r.ramp(0.0, 7.0, 25); // cross 6 deg once
    for _ in 0..6 {
        r.ramp(7.0, 5.0, 8);
        r.ramp(5.0, 7.0, 8);
    }
    assert_eq!(r.clicks(), 1);

    r.ramp(7.0, 0.0, 40);
    assert_eq!(r.clicks(), 1);
    assert_eq!(r.t.swing_phase(), SwingPhase::ForwardStroke);
}

#[test]
fn rearm_when_settled_near_original_rest() {
    let mut r = rig_monitoring(6.0);

    r.ramp(0.0, 25.0, 60);
    r.ramp(25.0, 0.0, 40);
    assert_eq!(r.t.swing_phase(), SwingPhase::ForwardStroke);

    r.hold(0.0, 450);
    assert_eq!(r.t.swing_phase(), SwingPhase::Backswing);
    assert!(r.t.is_ready_for_swing());
}

#[test]
fn no_rearm_when_settled_far_from_original_rest() {
    let mut r = rig_monitoring(6.0);

    r.ramp(0.0, 25.0, 60);
    // 20 deg reverses (20 <= 25-2) but is outside the 10 deg re-center gate.
    r.hold(20.0, 600);
    assert_eq!(r.t.swing_phase(), SwingPhase::Recovering);
    assert!(!r.t.is_ready_for_swing());
}

#[test]
fn recal_resets_everything() {
    let mut r = Rig::new();
    r.calibrate_into_monitoring();
    r.ramp(0.0, 15.0, 40);
    assert!(r.t.click_count() > 0);

    r.t.recal();
    assert_eq!(r.t.state(), State::WaitingForCalibration);
    assert_eq!(r.t.click_count(), 0);
    assert!(!r.t.is_ready_for_swing());
}

// Autocal: no tap needed. Past autocal_not_flat_z (0.25) and held still for 3 s => calibrates.
#[test]
fn autocal_fires_after_notflat_still() {
    let mut r = Rig::new();
    assert_eq!(r.t.state(), State::WaitingForCalibration);

    r.hold(0.0, 100); // flat/viewing for 1 s - must not arm
    assert_eq!(r.t.state(), State::WaitingForCalibration);

    r.hold(83.0, 310); // gravity.z ~= 0.122, past the not-flat threshold
    assert_eq!(r.t.state(), State::Done);
    assert!((r.t.rest_gravity().z - 83.0f32.to_radians().cos()).abs() < 0.01);

    r.hold(83.0, 210);
    assert_eq!(r.t.state(), State::Monitoring);
    assert!(r.t.is_ready_for_swing());
}

#[test]
fn autocal_does_not_fire_while_flat() {
    let mut r = Rig::new();
    r.hold(0.0, 500);
    assert_eq!(r.t.state(), State::WaitingForCalibration);
}

// A big jump mid-window restarts the stillness window.
#[test]
fn autocal_window_restarts_on_motion() {
    let mut r = Rig::new();

    r.hold(83.0, 150);
    assert_eq!(r.t.state(), State::WaitingForCalibration);

    r.hold(88.0, 310);
    assert_eq!(r.t.state(), State::Done);
    assert!((r.t.rest_gravity().z - 88.0f32.to_radians().cos()).abs() < 0.02);
}

#[test]
fn tap_overrides_autocal_mid_window() {
    let mut r = Rig::new();
    r.hold(83.0, 150);
    r.t.tap();
    assert_eq!(r.t.state(), State::Accumulating);
}

// SwingComplete fires exactly once per swing, carrying that swing's peak angle.
#[test]
fn swing_complete_fires_with_peak_angle() {
    let mut r = rig_monitoring(6.0);

    r.ramp(0.0, 25.0, 60);
    r.ramp(25.0, 0.0, 40);
    assert_eq!(r.t.swing_phase(), SwingPhase::ForwardStroke);
    assert!(r.swing_completes().is_empty()); // ForwardStroke hasn't ended

    r.hold(0.0, 210);
    let c = r.swing_completes();
    assert_eq!(c.len(), 1);
    assert!((c[0] - 25.0).abs() < 0.1);

    r.hold(0.0, 250); // finish stillness -> re-arm
    assert!(r.t.is_ready_for_swing());

    r.ramp(0.0, 10.0, 40);
    r.ramp(10.0, 0.0, 40);
    r.hold(0.0, 210);
    let c = r.swing_completes();
    assert_eq!(c.len(), 2);
    assert!((c[1] - 10.0).abs() < 0.1);
}

// An opposite-direction wobble after the direction is locked must not trigger a false reversal.
#[test]
fn direction_lock_ignores_opposite_wobble() {
    let mut r = rig_monitoring(6.0);

    r.ramp(0.0, 25.0, 60);
    assert_eq!(r.clicks(), 3);
    r.ramp(25.0, 0.0, 40);
    r.hold(0.0, 450); // re-arm
    assert!(r.t.is_ready_for_swing());

    r.ramp(0.0, -3.0, 30);
    r.ramp(-3.0, 0.0, 30);
    assert_eq!(r.t.swing_phase(), SwingPhase::Backswing);
    assert!(r.t.is_ready_for_swing());
    assert_eq!(r.clicks(), 3);

    r.ramp(0.0, 25.0, 60);
    assert_eq!(r.clicks(), 6);
}

#[test]
fn click_threshold_clamped() {
    let mut t = SwingTracker::default();
    t.set_click_threshold_degrees(0.5);
    assert!((t.click_threshold_degrees() - 3.0).abs() < 0.001);
    t.set_click_threshold_degrees(99.0);
    assert!((t.click_threshold_degrees() - 16.5).abs() < 0.001);
    t.set_click_threshold_degrees(7.0);
    assert!((t.click_threshold_degrees() - 7.0).abs() < 0.001);
}
