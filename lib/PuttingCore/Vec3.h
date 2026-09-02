#pragma once
#include <cmath>

// Minimal 3-vector for the swing math. Header-only, no Arduino/ESP deps, so
// everything in lib/PuttingCore stays host-compilable for the unit tests.
struct Vec3 {
    float x = 0, y = 0, z = 0;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    explicit Vec3(const float v[3]) : x(v[0]), y(v[1]), z(v[2]) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }

    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float norm() const { return std::sqrt(dot(*this)); }

    Vec3 normalized() const {
        float n = norm();
        return n > 1e-9f ? Vec3{x / n, y / n, z / n} : Vec3{0, 0, 0};
    }

    void store(float out[3]) const {
        out[0] = x;
        out[1] = y;
        out[2] = z;
    }
};

// Angle between two vectors, in degrees. `acos` argument is clamped to
// [-1, 1] first: ordinary float rounding can push the ratio a hair past 1
// for near-identical vectors, which would otherwise make `acos` return NaN
// right when the angle should read as ~0. (Ported from
// CalibrationManager.angleDegrees(between:and:).)
inline float angleBetweenDegrees(const Vec3& a, const Vec3& b) {
    float na = a.norm(), nb = b.norm();
    if (na < 1e-9f || nb < 1e-9f) return 0.0f;
    float c = a.dot(b) / (na * nb);
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return std::acos(c) * 57.2957795131f;  // 180/pi
}
