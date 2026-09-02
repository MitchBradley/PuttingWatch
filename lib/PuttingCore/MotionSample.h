#pragma once
#include <cstdint>
#include <cmath>

// One processed IMU reading, in the sensor body frame. This is the unit the
// swing logic (SwingTracker) consumes - it deliberately does NOT expose raw
// QMI8658 counts. `gravity` is the filtered unit gravity estimate produced by
// GravityFilter (there is no CoreMotion-style sensor fusion on this board, so
// we synthesise it); `accel`/`rotationRate` are passed through for the future
// impact / face-angle work (see plan M5) and are unused by the watch-subset
// state machine.
//
// Mirrors the fields of the Swift `MotionSample` that the ported logic
// actually touches.
struct MotionSample {
    uint32_t t_ms = 0;         // milliseconds, monotonic; the only clock the
                               // state machine reads (matches the phone app's
                               // "drive everything off sample.timestamp").
    float gravity[3] = {0, 0, 0};       // unit vector, sensor frame
    float rotationRate[3] = {0, 0, 0};  // rad/s, sensor frame (raw gyro)
    float accel[3] = {0, 0, 0};         // g, sensor frame (raw accel, gravity NOT removed)

    // Total rotation rate in deg/s - the direct "how fast is it turning right
    // now" signal, used as the at-rest gate. (Ported from
    // MotionSample.rotationMagnitudeDegreesPerSecond.)
    float rotationMagnitudeDegPerSec() const {
        float m = rotationRate[0] * rotationRate[0] + rotationRate[1] * rotationRate[1] +
                  rotationRate[2] * rotationRate[2];
        return (m > 0.0f ? std::sqrt(m) : 0.0f) * 57.2957795131f;
    }
};
