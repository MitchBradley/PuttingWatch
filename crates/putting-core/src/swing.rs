//! SwingTracker - owns ALL putting-stroke logic: calibration, arming, click ratcheting,
//! reversal detection, re-arm gating. Knows nothing about the UI or audio. Driven by
//! `update(sample)` at a steady rate (~100 Hz); all timing comes from `sample.t_ms`.
//!
//! Where the C++ original fired callbacks (onClick, onReady, ...), `update` returns the
//! events that happened during that sample instead.

use crate::vec3::{angle_between_degrees, Vec3};

/// One processed IMU reading in the sensor body frame. `gravity` is the unit gravity estimate
/// from `GravityFilter`; the other fields are carried for future impact / face-angle work.
#[derive(Clone, Copy, Debug, Default)]
pub struct MotionSample {
    /// Milliseconds, monotonic - the only clock the state machine reads.
    pub t_ms: u32,
    pub gravity: Vec3,
    /// rad/s, raw gyro.
    pub rotation_rate: Vec3,
    /// g, raw accel (gravity not removed).
    pub accel: Vec3,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    WaitingForCalibration,
    Accumulating,
    Done,
    Monitoring,
}

/// The stroke lifecycle within `State::Monitoring`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SwingPhase {
    /// Clicking enabled, tracking away from rest.
    Backswing,
    /// Reversal seen - clicking suppressed, waiting out `end_of_swing_delay_ms`.
    ForwardStroke,
    /// Swing over - waiting for held-still time before re-arming.
    Recovering,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Event {
    /// One threshold crossing during the backswing.
    Click,
    /// Calibration completed (Accumulating/autocal -> Done).
    CalibrationComplete,
    /// ForwardStroke ended (-> Recovering); carries that swing's peak backswing angle, degrees.
    SwingComplete(f32),
    /// Phase became Backswing while armed: the initial arm and every re-arm.
    Ready,
    /// The backswing direction lock latched (diagnostic); carries the raw angle at the lock.
    DirectionLocked(f32),
}

#[derive(Clone, Debug)]
pub struct Config {
    pub accumulation_ms: u32,
    pub monitoring_delay_ms: u32,
    /// Range of the UI's 1..16 sensitivity levels (this class only reasons in degrees).
    pub click_threshold_degrees_min: f32,
    pub click_threshold_degrees_max: f32,
    /// Drop below peak => forward stroke.
    pub reversal_drop_degrees: f32,
    pub end_of_swing_delay_ms: u32,
    pub stillness_duration_ms: u32,
    /// Drift that resets the stillness window.
    pub stillness_tolerance_degrees: f32,
    /// A settled position must be this near the ORIGINAL calibrated rest to re-center.
    pub rest_recenter_max_drift_degrees: f32,
    /// Caps clicks per backswing attempt (a good stroke clicks at most twice).
    pub max_clicks_per_swing: i32,
    /// Once a swing reaches this many degrees its direction is latched as "the" backswing
    /// direction; the opposite side of rest then reads as angle 0.
    pub direction_lock_degrees: f32,
    /// Autocal: gravity Z (device frame) below this while held still means address position.
    pub autocal_not_flat_z: f32,
    pub autocal_stillness_ms: u32,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            accumulation_ms: 2000,
            monitoring_delay_ms: 2000,
            click_threshold_degrees_min: 3.0,
            click_threshold_degrees_max: 16.5,
            reversal_drop_degrees: 2.0,
            end_of_swing_delay_ms: 2000,
            stillness_duration_ms: 1000,
            stillness_tolerance_degrees: 3.0,
            rest_recenter_max_drift_degrees: 10.0,
            max_clicks_per_swing: 3,
            direction_lock_degrees: 4.0,
            autocal_not_flat_z: 0.25,
            autocal_stillness_ms: 3000,
        }
    }
}

pub struct SwingTracker {
    cfg: Config,
    state: State,
    swing_phase: SwingPhase,
    click_threshold_degrees: f32,

    // accumulation
    accum_pending: bool, // tap() seen, first Accumulating sample not yet processed
    accum_start_ms: u32,
    accum_sum: Vec3,
    accum_count: i32,

    // autocal (WaitingForCalibration)
    have_autocal_ref: bool,
    autocal_ref_gravity: Vec3,
    autocal_window_start_ms: u32,
    autocal_sum: Vec3,
    autocal_count: i32,

    done_entered_ms: u32,

    // references
    rest_gravity: Vec3,         // recentred after each settled swing
    initial_rest_gravity: Vec3, // fixed at calibration; drift check for re-center

    // live swing state
    swing_angle_degrees: f32,
    last_click_bucket: i32,
    click_count: i32,
    peak_angle_this_swing: f32,

    // backswing direction lock
    have_direction_ref: bool,
    direction_ref: Vec3, // g - rest_gravity at the moment of the lock

    forward_stroke_start_ms: u32,

    have_stillness_ref: bool,
    stillness_ref_gravity: Vec3,
    stillness_window_start_ms: u32,
}

impl Default for SwingTracker {
    fn default() -> Self {
        Self::new(Config::default())
    }
}

impl SwingTracker {
    pub fn new(cfg: Config) -> Self {
        SwingTracker {
            cfg,
            state: State::WaitingForCalibration,
            swing_phase: SwingPhase::Backswing,
            click_threshold_degrees: 5.9, // level 3
            accum_pending: false,
            accum_start_ms: 0,
            accum_sum: Vec3::default(),
            accum_count: 0,
            have_autocal_ref: false,
            autocal_ref_gravity: Vec3::default(),
            autocal_window_start_ms: 0,
            autocal_sum: Vec3::default(),
            autocal_count: 0,
            done_entered_ms: 0,
            rest_gravity: Vec3::new(0.0, 0.0, 1.0),
            initial_rest_gravity: Vec3::new(0.0, 0.0, 1.0),
            swing_angle_degrees: 0.0,
            last_click_bucket: 0,
            click_count: 0,
            peak_angle_this_swing: 0.0,
            have_direction_ref: false,
            direction_ref: Vec3::default(),
            forward_stroke_start_ms: 0,
            have_stillness_ref: false,
            stillness_ref_gravity: Vec3::default(),
            stillness_window_start_ms: 0,
        }
    }

    // --- input ---------------------------------------------------------------

    /// WaitingForCalibration -> Accumulating. No-op in any other state.
    pub fn tap(&mut self) {
        if self.state != State::WaitingForCalibration {
            return;
        }
        self.state = State::Accumulating;
        self.accum_pending = true; // first Accumulating sample seeds the window + sums
    }

    /// Any state -> WaitingForCalibration (the "Recal" button).
    pub fn recal(&mut self) {
        self.state = State::WaitingForCalibration;
        self.swing_phase = SwingPhase::Backswing;
        self.accum_pending = false;
        self.accum_sum = Vec3::default();
        self.accum_count = 0;
        self.have_autocal_ref = false;
        self.autocal_ref_gravity = Vec3::default();
        self.autocal_window_start_ms = 0;
        self.autocal_sum = Vec3::default();
        self.autocal_count = 0;
        self.swing_angle_degrees = 0.0;
        self.last_click_bucket = 0;
        self.click_count = 0;
        self.peak_angle_this_swing = 0.0;
        self.have_direction_ref = false;
        self.direction_ref = Vec3::default();
        self.forward_stroke_start_ms = 0;
        self.have_stillness_ref = false;
    }

    /// Click interval in degrees, clamped to the configured range.
    pub fn set_click_threshold_degrees(&mut self, deg: f32) {
        self.click_threshold_degrees =
            deg.clamp(self.cfg.click_threshold_degrees_min, self.cfg.click_threshold_degrees_max);
    }

    /// Feed one processed sample; returns the events it caused.
    pub fn update(&mut self, s: &MotionSample) -> Vec<Event> {
        let mut ev = Vec::new();
        let g = s.gravity;

        match self.state {
            // Autocal: rather than requiring tap(), watch for the watch tilting out of face-up
            // viewing position (gravity Z below autocal_not_flat_z) and being held still there.
            State::WaitingForCalibration => {
                if g.z >= self.cfg.autocal_not_flat_z {
                    self.have_autocal_ref = false; // still flat/viewing
                    return ev;
                }
                let drift = if self.have_autocal_ref {
                    angle_between_degrees(g, self.autocal_ref_gravity)
                } else {
                    0.0
                };
                if !self.have_autocal_ref || drift > self.cfg.stillness_tolerance_degrees {
                    // (Re)start the window from here.
                    self.have_autocal_ref = true;
                    self.autocal_ref_gravity = g;
                    self.autocal_window_start_ms = s.t_ms;
                    self.autocal_sum = g;
                    self.autocal_count = 1;
                    return ev;
                }
                self.autocal_sum = self.autocal_sum + g;
                self.autocal_count += 1;
                if s.t_ms.wrapping_sub(self.autocal_window_start_ms) >= self.cfg.autocal_stillness_ms {
                    self.rest_gravity = (self.autocal_sum * (1.0 / self.autocal_count as f32)).normalized();
                    self.initial_rest_gravity = self.rest_gravity;
                    self.state = State::Done;
                    self.done_entered_ms = s.t_ms;
                    self.have_autocal_ref = false;
                    ev.push(Event::CalibrationComplete);
                }
            }

            State::Accumulating => {
                if self.accum_pending {
                    self.accum_pending = false;
                    self.accum_start_ms = s.t_ms;
                    self.accum_sum = Vec3::default();
                    self.accum_count = 0;
                }
                self.accum_sum = self.accum_sum + g;
                self.accum_count += 1;
                if s.t_ms.wrapping_sub(self.accum_start_ms) >= self.cfg.accumulation_ms {
                    if self.accum_count > 0 {
                        self.rest_gravity = (self.accum_sum * (1.0 / self.accum_count as f32)).normalized();
                        self.initial_rest_gravity = self.rest_gravity;
                        self.state = State::Done;
                        self.done_entered_ms = s.t_ms;
                        ev.push(Event::CalibrationComplete);
                    } else {
                        self.state = State::WaitingForCalibration;
                    }
                }
            }

            State::Done => {
                if s.t_ms.wrapping_sub(self.done_entered_ms) >= self.cfg.monitoring_delay_ms {
                    self.enter_monitoring(&mut ev);
                }
            }

            State::Monitoring => self.process_monitoring_sample(s, &mut ev),
        }
        ev
    }

    // --- output ----------------------------------------------------------------

    pub fn state(&self) -> State {
        self.state
    }

    pub fn swing_phase(&self) -> SwingPhase {
        self.swing_phase
    }

    /// "Armed, next backswing will register": Monitoring and in the Backswing phase.
    pub fn is_ready_for_swing(&self) -> bool {
        self.state == State::Monitoring && self.swing_phase == SwingPhase::Backswing
    }

    /// Angle between current and calibrated rest gravity, degrees. Meaningful only while Monitoring.
    pub fn swing_angle_degrees(&self) -> f32 {
        self.swing_angle_degrees
    }

    /// While waiting for calibration: how long the current autocal still-window has run, or
    /// `None` if no candidate window is open (watch flat, or moving too much).
    pub fn autocal_elapsed_ms(&self, now_ms: u32) -> Option<u32> {
        (self.state == State::WaitingForCalibration && self.have_autocal_ref)
            .then(|| now_ms.wrapping_sub(self.autocal_window_start_ms))
    }

    pub fn click_count(&self) -> i32 {
        self.click_count
    }

    pub fn click_threshold_degrees(&self) -> f32 {
        self.click_threshold_degrees
    }

    pub fn rest_gravity(&self) -> Vec3 {
        self.rest_gravity
    }

    /// Rest gravity fixed at calibration; re-arming needs the settled position within
    /// `rest_recenter_max_drift_degrees` of this.
    pub fn initial_rest_gravity(&self) -> Vec3 {
        self.initial_rest_gravity
    }

    // --- internals ---------------------------------------------------------------

    fn enter_monitoring(&mut self, ev: &mut Vec<Event>) {
        self.state = State::Monitoring;
        self.swing_phase = SwingPhase::Backswing;
        self.swing_angle_degrees = 0.0;
        self.last_click_bucket = 0;
        self.click_count = 0;
        self.peak_angle_this_swing = 0.0;
        self.have_direction_ref = false;
        self.direction_ref = Vec3::default();
        self.forward_stroke_start_ms = 0;
        self.have_stillness_ref = false;
        ev.push(Event::Ready);
    }

    // `angle` is the unsigned angle between current gravity and calibrated rest gravity - the
    // vector angle captures the rotation directly, no axis detection needed.
    fn process_monitoring_sample(&mut self, s: &MotionSample, ev: &mut Vec<Event>) {
        let g = s.gravity;
        let angle = angle_between_degrees(g, self.rest_gravity);
        self.swing_angle_degrees = angle;

        match self.swing_phase {
            SwingPhase::Backswing => {
                // angle alone can't tell a real backswing from a wobble the other way. Once a
                // swing has gone far enough to be trusted, latch its direction; from then on
                // motion on the other side of rest reads as 0 instead of raising the peak.
                let deviation = g - self.rest_gravity;
                let mut effective_angle = angle;
                if !self.have_direction_ref {
                    if angle >= self.cfg.direction_lock_degrees {
                        self.direction_ref = deviation;
                        self.have_direction_ref = true;
                        ev.push(Event::DirectionLocked(angle));
                    }
                } else if deviation.dot(self.direction_ref) <= 0.0 {
                    effective_angle = 0.0;
                }

                if effective_angle > self.peak_angle_this_swing {
                    self.peak_angle_this_swing = effective_angle;
                }

                // Click off the MONOTONIC peak, not the instantaneous angle: never fires on the
                // way back down, immune to jitter across a threshold boundary, and still fires
                // at the same instant on the way up. Loop so a fast swing that jumps several
                // thresholds in one sample still emits one click each, capped at
                // max_clicks_per_swing (last_click_bucket resets on every re-arm, so it already
                // counts this attempt's clicks).
                let peak_bucket = (self.peak_angle_this_swing / self.click_threshold_degrees) as i32;
                while self.last_click_bucket < peak_bucket
                    && self.last_click_bucket < self.cfg.max_clicks_per_swing
                {
                    self.last_click_bucket += 1;
                    self.click_count += 1;
                    ev.push(Event::Click);
                }

                // Reversal is measured from the CONTINUOUS peak, not the click bucket: a swing
                // that never reaches one full threshold would leave the bucket at 0 and the
                // phase machine would never leave Backswing.
                //
                // Also gate on click_threshold_degrees_min: reversal_drop (2 deg) alone is small
                // enough that settling-into-address tremor satisfies it, completing a phantom
                // Backswing -> ForwardStroke -> Recovering cycle before the real stroke starts
                // and swallowing it. A swing too small to fire even the most sensitive click is
                // indistinguishable from noise.
                if self.peak_angle_this_swing >= self.cfg.click_threshold_degrees_min
                    && effective_angle <= self.peak_angle_this_swing - self.cfg.reversal_drop_degrees
                {
                    self.swing_phase = SwingPhase::ForwardStroke;
                    self.forward_stroke_start_ms = s.t_ms;
                }
            }

            // Clicking suppressed on the way back down. Once the reversal has held for
            // end_of_swing_delay, the swing is over; start watching for stillness.
            SwingPhase::ForwardStroke => {
                if s.t_ms.wrapping_sub(self.forward_stroke_start_ms) >= self.cfg.end_of_swing_delay_ms {
                    self.swing_phase = SwingPhase::Recovering;
                    self.stillness_ref_gravity = g;
                    self.have_stillness_ref = true;
                    self.stillness_window_start_ms = s.t_ms;
                    ev.push(Event::SwingComplete(self.peak_angle_this_swing));
                }
            }

            SwingPhase::Recovering => {
                if !self.have_stillness_ref {
                    self.stillness_ref_gravity = g;
                    self.have_stillness_ref = true;
                    self.stillness_window_start_ms = s.t_ms;
                    return;
                }
                let drift = angle_between_degrees(g, self.stillness_ref_gravity);
                if drift > self.cfg.stillness_tolerance_degrees {
                    // Moved too much to count as still - restart the window from here.
                    self.stillness_ref_gravity = g;
                    self.stillness_window_start_ms = s.t_ms;
                } else if s.t_ms.wrapping_sub(self.stillness_window_start_ms) >= self.cfg.stillness_duration_ms {
                    // Held still long enough. Only re-center (and re-arm) if the settled position
                    // is also close to the ORIGINAL calibrated rest - otherwise a stroke that
                    // finished off address would drift the reference further with every swing.
                    // If too far, stay in Recovering; it is re-checked every sample.
                    let drift_from_initial = angle_between_degrees(g, self.initial_rest_gravity);
                    if drift_from_initial <= self.cfg.rest_recenter_max_drift_degrees {
                        self.rest_gravity = g;
                        self.swing_phase = SwingPhase::Backswing;
                        self.last_click_bucket = 0;
                        self.peak_angle_this_swing = 0.0;
                        self.forward_stroke_start_ms = 0;
                        self.have_stillness_ref = false;
                        ev.push(Event::Ready);
                    }
                }
            }
        }
    }
}
