use crate::vec3::Vec3;

/// Complementary filter turning raw accel + gyro into a stable unit gravity estimate in the
/// sensor frame: gyro integration for short-term stability (immune to stroke acceleration),
/// pulled slowly toward the accelerometer direction so it can't drift.
pub struct GravityFilter {
    alpha: f32,
    gravity: Vec3,
    initialised: bool,
}

impl GravityFilter {
    /// `alpha` is the fraction of the gyro-propagated estimate kept each update; 0.98 at
    /// ~100 Hz gives roughly a 0.5 s pull toward the accel direction.
    pub fn new(alpha: f32) -> Self {
        GravityFilter { alpha, gravity: Vec3::new(0.0, 0.0, 1.0), initialised: false }
    }

    pub fn set_alpha(&mut self, a: f32) {
        self.alpha = a;
    }

    pub fn initialised(&self) -> bool {
        self.initialised
    }

    /// Seed the estimate from an accelerometer reading (device known to be roughly still).
    pub fn reset(&mut self, accel_g: Vec3) {
        self.gravity = accel_g.normalized();
        self.initialised = self.gravity.norm() > 0.5;
    }

    /// `accel_g` in g (gravity not removed), `gyro_rps` in rad/s, `dt_s` since the last update.
    pub fn update(&mut self, accel_g: Vec3, gyro_rps: Vec3, mut dt_s: f32) {
        if !self.initialised {
            self.reset(accel_g);
            return;
        }
        if dt_s <= 0.0 || dt_s > 0.5 {
            dt_s = 0.01; // guard clock glitches
        }

        // A world-fixed vector seen from a frame rotating at omega changes as dg/dt = -omega x g.
        // First-order step is fine at 100 Hz and putting speeds.
        let predicted = (self.gravity - gyro_rps.cross(self.gravity) * dt_s).normalized();

        let accel_hat = accel_g.normalized();
        self.gravity = if accel_hat.norm() < 0.5 {
            predicted // unusable accel this tick - gyro only
        } else {
            (predicted * self.alpha + accel_hat * (1.0 - self.alpha)).normalized()
        };
    }

    pub fn gravity(&self) -> Vec3 {
        self.gravity
    }

    /// Accel with the gravity component removed, in g.
    pub fn linear_accel(&self, accel_g: Vec3) -> Vec3 {
        accel_g - self.gravity
    }
}

impl Default for GravityFilter {
    fn default() -> Self {
        Self::new(0.98)
    }
}
