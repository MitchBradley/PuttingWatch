#pragma once
#include "Vec3.h"

// Complementary filter that turns raw QMI8658 accel + gyro into a stable unit
// gravity estimate in the sensor body frame.
//
// Why this exists: the iOS app got `gravity` for free from CoreMotion's
// sensor fusion. This board has no equivalent, so we fuse it ourselves - gyro
// integration for short-term stability (immune to the linear acceleration of
// a stroke), pulled back toward the raw accelerometer direction slowly so it
// can't drift. `alpha` near 1 trusts the gyro more (smoother, slower to
// correct); the default is a starting point to be tuned on-device against
// real putter-mounted strokes.
class GravityFilter {
  public:
    // Fraction of the gyro-propagated estimate kept each update; the rest is
    // taken from the normalised accelerometer. 0.98 @ ~100 Hz => ~0.5 s pull
    // toward the accel direction.
    explicit GravityFilter(float alpha = 0.98f) : alpha_(alpha) {}

    void setAlpha(float a) { alpha_ = a; }
    bool initialised() const { return initialised_; }

    // Seed the estimate directly from an accelerometer reading (call once the
    // device is known to be roughly still, e.g. at the start of calibration).
    void reset(const Vec3& accel_g) {
        gravity_ = accel_g.normalized();
        initialised_ = (gravity_.norm() > 0.5f);
    }

    // accel_g:  accelerometer vector in g (gravity NOT removed)
    // gyro_rps: gyro vector in rad/s
    // dt_s:     seconds since the previous update
    void update(const Vec3& accel_g, const Vec3& gyro_rps, float dt_s) {
        if (!initialised_) {
            reset(accel_g);
            return;
        }
        if (dt_s <= 0.0f || dt_s > 0.5f) dt_s = 0.01f;  // guard clock glitches

        // A world-fixed vector seen from a frame rotating at omega changes as
        // dg/dt = -omega x g. Propagate the gravity direction by that, then
        // renormalise (first-order step is fine at 100 Hz / putting speeds).
        Vec3 predicted = (gravity_ - gyro_rps.cross(gravity_) * dt_s).normalized();

        Vec3 accel_hat = accel_g.normalized();
        if (accel_hat.norm() < 0.5f) {
            gravity_ = predicted;  // unusable accel this tick - gyro only
        } else {
            gravity_ = (predicted * alpha_ + accel_hat * (1.0f - alpha_)).normalized();
        }
    }

    const Vec3& gravity() const { return gravity_; }

    // accel with the gravity component removed, in g. Kept for the future
    // impact detector (plan M5); unused by the watch-subset state machine.
    Vec3 linearAccel(const Vec3& accel_g) const { return accel_g - gravity_; }

  private:
    float alpha_;
    Vec3 gravity_{0, 0, 1};
    bool initialised_ = false;
};
