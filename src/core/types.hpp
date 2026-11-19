// USA 1:1 Procedural Expansion - core scalar/vector types.
// This header is platform neutral: no AML, no plugin-sdk, no Android headers.
// Everything here must compile on a host compiler for the unit test suite.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <string>

namespace usa1to1 {
namespace core {

// World scale is 1:1 with the real USA, so global coordinates are metres on a
// projected plane and MUST stay in double precision. 4500 km of x-range needs
// ~23 bits of mantissa more than a float can offer before centimetre jitter
// starts eating collision and render stability.
struct Vec2d {
    double x = 0.0;
    double y = 0.0;

    Vec2d() = default;
    Vec2d(double _x, double _y) : x(_x), y(_y) {}

    Vec2d operator+(const Vec2d &o) const { return {x + o.x, y + o.y}; }
    Vec2d operator-(const Vec2d &o) const { return {x - o.x, y - o.y}; }
    Vec2d operator*(double s) const { return {x * s, y * s}; }
    Vec2d operator/(double s) const { return {x / s, y / s}; }
    Vec2d &operator+=(const Vec2d &o) {
        x += o.x;
        y += o.y;
        return *this;
    }
    Vec2d &operator-=(const Vec2d &o) {
        x -= o.x;
        y -= o.y;
        return *this;
    }
    Vec2d operator-() const { return {-x, -y}; }
    bool operator==(const Vec2d &o) const { return x == o.x && y == o.y; }
    bool operator!=(const Vec2d &o) const { return !(*this == o); }

    double Length() const { return std::sqrt(x * x + y * y); }
    double LengthSqr() const { return x * x + y * y; }
    double Distance(const Vec2d &o) const { return (*this - o).Length(); }
    bool IsFinite() const { return std::isfinite(x) && std::isfinite(y); }
};

// RenderWare local space: float is fine, but only because FloatingOrigin keeps
// the whole active grid inside [-1000, +1000].
struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3f() = default;
    Vec3f(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

    Vec3f operator+(const Vec3f &o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3f operator-(const Vec3f &o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3f operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3f &operator+=(const Vec3f &o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }

    Vec2d XY() const { return {static_cast<double>(x), static_cast<double>(y)}; }
    float Length2D() const { return std::sqrt(x * x + y * y); }
    float Distance2D(const Vec3f &o) const { return (*this - o).Length2D(); }
    bool IsFinite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
};

struct Vec2f {
    float x = 0.0f;
    float y = 0.0f;
    Vec2f() = default;
    Vec2f(float _x, float _y) : x(_x), y(_y) {}
    Vec2f operator+(const Vec2f &o) const { return {x + o.x, y + o.y}; }
    Vec2f operator-(const Vec2f &o) const { return {x - o.x, y - o.y}; }
    float Length() const { return std::sqrt(x * x + y * y); }
};

// Clamped conversion double -> float. The floating origin invariant guarantees
// the input already fits, this is the last line of defence against a runaway
// entity teleporting the grid.
inline float SafeLocal(double v) {
    constexpr double kLimit = 4000.0; // hard sanity bound, invariant limit is 1000
    if (v > kLimit)
        v = kLimit;
    if (v < -kLimit)
        v = -kLimit;
    return static_cast<float>(v);
}

inline Vec3f ToLocalVec(const Vec2d &globalPos, const Vec2d &origin, float z) {
    return {SafeLocal(globalPos.x - origin.x), SafeLocal(globalPos.y - origin.y), z};
}

inline Vec2d ToGlobal(const Vec3f &local, const Vec2d &origin) {
    return {origin.x + static_cast<double>(local.x), origin.y + static_cast<double>(local.y)};
}

// Interior pockets live far above the world plane. Anything at or above this Z
// is interior space: it never participates in floating-origin shifts.
constexpr float kInteriorBaseZ = 1000.0f;
constexpr float kInteriorPocketHeight = 60.0f; // vertical spacing between pockets
inline bool IsInteriorZ(float z) { return z >= kInteriorBaseZ - 0.5f; }

inline double WrapAngle(double a) {
    constexpr double kPi = 3.14159265358979323846;
    while (a > kPi)
        a -= 2.0 * kPi;
    while (a < -kPi)
        a += 2.0 * kPi;
    return a;
}

inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline double Lerp(double a, double b, double t) { return a + (b - a) * t; }
inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double Clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace core
} // namespace usa1to1
