// SPDX-License-Identifier: Apache-2.0
#pragma once

// Conversions between tetra3's (RA, Dec, Roll) / rotation-matrix convention and our camera-frame
// quaternion. tetra3's rotation R maps ICRS vectors to its camera frame (i = boresight,
// j = -X, k = -Y in our frame); RA = atan2(R01, R00), Dec = atan2(R02, |(R12, R22)|),
// Roll = atan2(R12, R22). All O(1), double precision.

#include <cmath>

#include "st/math/vec.hpp"

namespace st {

// tetra3 frame -> ours (+X right, +Y down, +Z boresight).
inline Mat3 tetra3_to_camera() noexcept {
  Mat3 m;
  m.m[0] = {0, -1, 0};
  m.m[1] = {0, 0, -1};
  m.m[2] = {1, 0, 0};
  return m;
}

// R = Rx(roll) * Ry(-dec) * Rz(ra) (passive rotations), angles in radians.
inline Mat3 tetra3_rotation(double ra, double dec, double roll) noexcept {
  const double ca = std::cos(ra), sa = std::sin(ra), cd = std::cos(dec), sd = std::sin(dec);
  const double cr = std::cos(roll), sr = std::sin(roll);
  Mat3 rz, ry, rx;
  rz.m[0] = {ca, sa, 0};
  rz.m[1] = {-sa, ca, 0};
  rz.m[2] = {0, 0, 1};
  ry.m[0] = {cd, 0, sd};
  ry.m[1] = {0, 1, 0};
  ry.m[2] = {-sd, 0, cd};
  rx.m[0] = {1, 0, 0};
  rx.m[1] = {0, cr, sr};
  rx.m[2] = {0, -sr, cr};
  return rx * ry * rz;
}

// Attitude q_cam_icrs (our frame) for a tetra3 RA/Dec/Roll in radians.
inline Quat attitude_from_radec_roll(double ra, double dec, double roll) noexcept {
  return from_matrix(tetra3_to_camera() * tetra3_rotation(ra, dec, roll));
}

// RA/Dec/Roll (radians; RA and Roll in [0, 2pi)) of an attitude, tetra3 conventions.
inline void radec_roll_from_attitude(const Quat& q_cam_icrs, double& ra, double& dec,
                                     double& roll) noexcept {
  const Mat3 r = transpose(tetra3_to_camera()) * to_matrix(q_cam_icrs);
  ra = std::atan2(r.m[0][1], r.m[0][0]);
  if (ra < 0) ra += 2 * kPi;
  dec = std::atan2(r.m[0][2], std::hypot(r.m[1][2], r.m[2][2]));
  roll = std::atan2(r.m[1][2], r.m[2][2]);
  if (roll < 0) roll += 2 * kPi;
}

}  // namespace st
