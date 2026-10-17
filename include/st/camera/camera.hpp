// SPDX-License-Identifier: Apache-2.0
#pragma once

// Pinhole camera with Brown-Conrady distortion and optional mirror parity.
//
// Pixel coordinates: origin at the top-left corner of the top-left pixel, pixel centres at
// +0.5 (tetra3 convention); x to the right, y down.
// Camera frame: +Z boresight, +X towards increasing pixel x, +Y towards increasing pixel y.
// (tetra3's internal frame is i = +Z, j = -X, k = -Y; the solver converts.)
//
// Distortion (OpenCV convention) maps undistorted normalized coordinates (xu, yu) to
// distorted ones:
//   r2 = xu^2 + yu^2,  g = 1 + k1 r2 + k2 r2^2 + k3 r2^3
//   xd = xu g + 2 p1 xu yu + p2 (r2 + 2 xu^2)
//   yd = yu g + p1 (r2 + 2 yu^2) + 2 p2 xu yu
// and pixels are x = cx + s*fx*xd, y = cy + fy*yd with s = -1 when mirrored, else +1.
//
// Precision: double throughout. Pixel coordinates reach ~4000 and need ~1e-4 px resolution
// over chains of trig, beyond float32's 7 digits.

#include <cstdint>

#include "st/core/result.hpp"
#include "st/math/vec.hpp"

namespace st {

struct CameraIntrinsics {
  std::uint32_t width = 0;   // pixels
  std::uint32_t height = 0;  // pixels
  double fx = 0;             // focal length, pixels
  double fy = 0;             // focal length, pixels (fy = fx for square pixels)
  double cx = 0;             // principal point, pixels
  double cy = 0;
  double k1 = 0, k2 = 0, k3 = 0;  // radial
  double p1 = 0, p2 = 0;          // tangential
  bool mirrored = false;          // parity flip of the x axis
};

class CameraModel {
 public:
  // Ideal camera with principal point at the image centre and horizontal FOV `fov_x` (rad).
  static CameraIntrinsics ideal(std::uint32_t width, std::uint32_t height, double fov_x) noexcept;
  // From physical focal length and pixel pitch (same unit, e.g. mm and mm).
  static CameraIntrinsics from_focal_length(std::uint32_t width, std::uint32_t height,
                                            double focal_length, double pixel_pitch) noexcept;

  // Errors: kInvalidArgument for non-positive size/focal length or non-finite values. O(1).
  static Result<CameraModel> create(const CameraIntrinsics& k) noexcept;

  CameraModel() noexcept = default;
  const CameraIntrinsics& intrinsics() const noexcept { return k_; }
  bool has_distortion() const noexcept { return has_distortion_; }

  // Horizontal field of view of the undistorted pinhole, radians. O(1).
  double fov_x() const noexcept;

  // Unit ray in the camera frame through pixel (x, y). Undistortion uses at most
  // kMaxUndistortIterations Newton steps. Worst case: O(kMaxUndistortIterations).
  Vec3 pixel_to_ray(double x, double y) const noexcept;

  // Pixel of a camera-frame direction. Returns false (outputs untouched) if v.z <= 0. O(1).
  bool ray_to_pixel(const Vec3& v, double& x, double& y) const noexcept;

  // Normalized-coordinate distortion and its inverse. O(1) / O(kMaxUndistortIterations).
  void distort(double xu, double yu, double& xd, double& yd) const noexcept;
  void undistort(double xd, double yd, double& xu, double& yu) const noexcept;

  // Removes distortion from a pixel position: the pixel an ideal camera with the same
  // fx, fy, cx, cy and parity would record. O(kMaxUndistortIterations).
  void undistort_pixel(double x, double y, double& xu, double& yu) const noexcept;

  static constexpr int kMaxUndistortIterations = 20;

 private:
  explicit CameraModel(const CameraIntrinsics& k) noexcept;
  CameraIntrinsics k_{};
  bool has_distortion_ = false;
  double sx_ = 1.0;  // -1 when mirrored
};

}  // namespace st
