// SPDX-License-Identifier: Apache-2.0
#pragma once

// Small fixed-size linear algebra in double precision. Attitude math is always double:
// float32 has ~1e-7 relative precision (~0.02 arcsec at unit scale), and rotation composition,
// SVD and the probability test lose several digits more, which would reach arcsecond level.
//
// Conventions:
//   * Mat3 is row-major: m[r][c].
//   * Quat is Hamilton, scalar first (w, x, y, z), unit norm.
//   * An attitude quaternion q_b_a rotates vectors from frame a to frame b:
//     v_b = rotate(q_b_a, v_a) = R(q_b_a) * v_a.
// All functions are O(1) and never allocate.

#include <array>
#include <cmath>

namespace st {

struct Vec3 {
  double x = 0, y = 0, z = 0;

  constexpr double operator[](int i) const noexcept { return i == 0 ? x : (i == 1 ? y : z); }
  constexpr double& operator[](int i) noexcept { return i == 0 ? x : (i == 1 ? y : z); }
};

constexpr Vec3 operator+(const Vec3& a, const Vec3& b) noexcept {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
constexpr Vec3 operator-(const Vec3& a, const Vec3& b) noexcept {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
constexpr Vec3 operator-(const Vec3& a) noexcept { return {-a.x, -a.y, -a.z}; }
constexpr Vec3 operator*(double s, const Vec3& a) noexcept { return {s * a.x, s * a.y, s * a.z}; }
constexpr Vec3 operator*(const Vec3& a, double s) noexcept { return s * a; }
constexpr Vec3 operator/(const Vec3& a, double s) noexcept { return {a.x / s, a.y / s, a.z / s}; }
constexpr double dot(const Vec3& a, const Vec3& b) noexcept {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
constexpr Vec3 cross(const Vec3& a, const Vec3& b) noexcept {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double norm(const Vec3& a) noexcept { return std::sqrt(dot(a, a)); }
// Returns a unit vector; the zero vector is returned unchanged.
inline Vec3 normalized(const Vec3& a) noexcept {
  const double n = norm(a);
  return n > 0 ? a / n : a;
}
// Angle between two unit vectors, accurate for small and large angles:
// 2*asin(|a-b|/2) (the form tetra3 uses), clamped to [0, pi].
inline double angle_between_unit(const Vec3& a, const Vec3& b) noexcept {
  const double h = 0.5 * norm(a - b);
  return 2.0 * std::asin(h > 1.0 ? 1.0 : h);
}

struct Mat3 {
  std::array<std::array<double, 3>, 3> m{};

  static constexpr Mat3 identity() noexcept {
    Mat3 r;
    r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0;
    return r;
  }
  constexpr Vec3 row(int r) const noexcept {
    const auto& v = m[static_cast<unsigned>(r)];
    return {v[0], v[1], v[2]};
  }
  constexpr Vec3 col(int c) const noexcept {
    const auto i = static_cast<unsigned>(c);
    return {m[0][i], m[1][i], m[2][i]};
  }
};

constexpr Vec3 operator*(const Mat3& a, const Vec3& v) noexcept {
  return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
          a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
          a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z};
}
constexpr Mat3 operator*(const Mat3& a, const Mat3& b) noexcept {
  Mat3 r;
  for (unsigned i = 0; i < 3; ++i) {
    for (unsigned j = 0; j < 3; ++j) {
      r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    }
  }
  return r;
}
constexpr Mat3 transpose(const Mat3& a) noexcept {
  Mat3 r;
  for (unsigned i = 0; i < 3; ++i) {
    for (unsigned j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
  }
  return r;
}
constexpr double determinant(const Mat3& a) noexcept {
  return a.m[0][0] * (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) -
         a.m[0][1] * (a.m[1][0] * a.m[2][2] - a.m[1][2] * a.m[2][0]) +
         a.m[0][2] * (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]);
}
// a * b^T (outer product).
constexpr Mat3 outer(const Vec3& a, const Vec3& b) noexcept {
  Mat3 r;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      r.m[static_cast<unsigned>(i)][static_cast<unsigned>(j)] = a[i] * b[j];
    }
  }
  return r;
}
constexpr Mat3 operator+(const Mat3& a, const Mat3& b) noexcept {
  Mat3 r;
  for (unsigned i = 0; i < 3; ++i) {
    for (unsigned j = 0; j < 3; ++j) r.m[i][j] = a.m[i][j] + b.m[i][j];
  }
  return r;
}
constexpr Mat3 operator*(double s, const Mat3& a) noexcept {
  Mat3 r;
  for (unsigned i = 0; i < 3; ++i) {
    for (unsigned j = 0; j < 3; ++j) r.m[i][j] = s * a.m[i][j];
  }
  return r;
}

struct Quat {
  double w = 1, x = 0, y = 0, z = 0;
};

constexpr Quat operator*(const Quat& a, const Quat& b) noexcept {
  return {
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
constexpr Quat conjugate(const Quat& q) noexcept { return {q.w, -q.x, -q.y, -q.z}; }
inline Quat normalized(const Quat& q) noexcept {
  const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  return n > 0 ? Quat{q.w / n, q.x / n, q.y / n, q.z / n} : Quat{};
}
// Sign-canonical form (w >= 0) so equal rotations compare equal.
constexpr Quat canonical(const Quat& q) noexcept {
  return q.w < 0 ? Quat{-q.w, -q.x, -q.y, -q.z} : q;
}

// Rotation matrix of a unit quaternion: rotate(q, v) == to_matrix(q) * v.
constexpr Mat3 to_matrix(const Quat& q) noexcept {
  const double ww = q.w * q.w, xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  Mat3 r;
  r.m[0] = {ww + xx - yy - zz, 2 * (xy - wz), 2 * (xz + wy)};
  r.m[1] = {2 * (xy + wz), ww - xx + yy - zz, 2 * (yz - wx)};
  r.m[2] = {2 * (xz - wy), 2 * (yz + wx), ww - xx - yy + zz};
  return r;
}

// Quaternion of a proper rotation matrix (Shepperd's method, numerically stable).
inline Quat from_matrix(const Mat3& r) noexcept {
  const double t = r.m[0][0] + r.m[1][1] + r.m[2][2];
  Quat q;
  if (t > r.m[0][0] && t > r.m[1][1] && t > r.m[2][2]) {
    const double s = 2.0 * std::sqrt(1.0 + t);
    q = {0.25 * s, (r.m[2][1] - r.m[1][2]) / s, (r.m[0][2] - r.m[2][0]) / s,
         (r.m[1][0] - r.m[0][1]) / s};
  } else if (r.m[0][0] > r.m[1][1] && r.m[0][0] > r.m[2][2]) {
    const double s = 2.0 * std::sqrt(1.0 + r.m[0][0] - r.m[1][1] - r.m[2][2]);
    q = {(r.m[2][1] - r.m[1][2]) / s, 0.25 * s, (r.m[0][1] + r.m[1][0]) / s,
         (r.m[0][2] + r.m[2][0]) / s};
  } else if (r.m[1][1] > r.m[2][2]) {
    const double s = 2.0 * std::sqrt(1.0 + r.m[1][1] - r.m[0][0] - r.m[2][2]);
    q = {(r.m[0][2] - r.m[2][0]) / s, (r.m[0][1] + r.m[1][0]) / s, 0.25 * s,
         (r.m[1][2] + r.m[2][1]) / s};
  } else {
    const double s = 2.0 * std::sqrt(1.0 + r.m[2][2] - r.m[0][0] - r.m[1][1]);
    q = {(r.m[1][0] - r.m[0][1]) / s, (r.m[0][2] + r.m[2][0]) / s, (r.m[1][2] + r.m[2][1]) / s,
         0.25 * s};
  }
  return canonical(normalized(q));
}

inline Vec3 rotate(const Quat& q, const Vec3& v) noexcept { return to_matrix(q) * v; }

// Rotation by angle |rv| about axis rv/|rv| (rotation vector / axis-angle).
inline Quat from_rotation_vector(const Vec3& rv) noexcept {
  const double a = norm(rv);
  if (a < 1e-12) return normalized(Quat{1.0, 0.5 * rv.x, 0.5 * rv.y, 0.5 * rv.z});
  const double s = std::sin(0.5 * a) / a;
  return {std::cos(0.5 * a), s * rv.x, s * rv.y, s * rv.z};
}
// Inverse of from_rotation_vector, angle in [0, pi].
inline Vec3 to_rotation_vector(const Quat& qin) noexcept {
  const Quat q = canonical(qin);
  const double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
  if (s < 1e-12) return {2.0 * q.x, 2.0 * q.y, 2.0 * q.z};
  const double a = 2.0 * std::atan2(s, q.w);
  return {a * q.x / s, a * q.y / s, a * q.z / s};
}
// Rotation angle between two attitudes, radians in [0, pi].
inline double angle_between(const Quat& a, const Quat& b) noexcept {
  return norm(to_rotation_vector(conjugate(a) * b));
}

// Unit vector from right ascension / declination (radians).
inline Vec3 radec_to_vector(double ra, double dec) noexcept {
  return {std::cos(ra) * std::cos(dec), std::sin(ra) * std::cos(dec), std::sin(dec)};
}

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDegToRad = kPi / 180.0;
inline constexpr double kRadToDeg = 180.0 / kPi;
inline constexpr double kArcsecToRad = kPi / (180.0 * 3600.0);

}  // namespace st
