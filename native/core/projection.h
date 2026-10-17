#pragma once

// Pinhole projection of a point through an Unreal camera (FRotator in degrees, horizontal FOV),
// used to know where Lily's feet are in the exported EL frame and how much world the frame covers.

#include <cmath>

namespace lie::proj {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

struct Basis {
    Vec3 forward, right, up;
};

inline constexpr double kPi = 3.14159265358979323846;

// [UE] FRotationMatrix axes for (Pitch, Yaw, Roll)
inline Basis basis(double pitch, double yaw, double roll) {
    const double p = pitch * kPi / 180.0, y = yaw * kPi / 180.0, r = roll * kPi / 180.0;
    const double sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr = std::cos(r);
    Basis b;
    b.forward = {cp * cy, cp * sy, sp};
    b.right = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
    b.up = {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
    return b;
}

inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Screen {
    bool valid = false;
    double u = 0.5, v = 0.5; // 0..1, v down
    double depth = 0.0;
};

inline Screen project(const Vec3& p, const Vec3& cam, double pitch, double yaw, double roll, double fov_deg, double aspect) {
    Screen s;
    if (fov_deg <= 1.0 || fov_deg >= 179.0 || aspect <= 0.0) return s;
    const Basis b = basis(pitch, yaw, roll);
    const Vec3 d{p.x - cam.x, p.y - cam.y, p.z - cam.z};
    const double z = dot(d, b.forward);
    if (z <= 1.0) return s;
    const double tx = std::tan(fov_deg * 0.5 * kPi / 180.0);
    const double ty = tx / aspect;
    s.valid = true;
    s.depth = z;
    s.u = 0.5 + 0.5 * dot(d, b.right) / (z * tx);
    s.v = 0.5 - 0.5 * dot(d, b.up) / (z * ty);
    return s;
}

// World width / height covered by the frame at a given depth
inline double view_width(double depth, double fov_deg) { return 2.0 * depth * std::tan(fov_deg * 0.5 * kPi / 180.0); }

} // namespace lie::proj
