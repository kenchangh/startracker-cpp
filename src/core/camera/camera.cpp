// SPDX-License-Identifier: Apache-2.0
#include "st/camera/camera.hpp"

#include <cmath>

namespace st {

CameraIntrinsics CameraModel::ideal(std::uint32_t width, std::uint32_t height,
                                    double fov_x) noexcept {
  CameraIntrinsics k;
  k.width = width;
  k.height = height;
  k.fx = k.fy = static_cast<double>(width) / 2.0 / std::tan(fov_x / 2.0);
  k.cx = static_cast<double>(width) / 2.0;
  k.cy = static_cast<double>(height) / 2.0;
  return k;
}

CameraIntrinsics CameraModel::from_focal_length(std::uint32_t width, std::uint32_t height,
                                                double focal_length, double pixel_pitch) noexcept {
  CameraIntrinsics k;
  k.width = width;
  k.height = height;
  k.fx = k.fy = pixel_pitch > 0 ? focal_length / pixel_pitch : 0.0;
  k.cx = static_cast<double>(width) / 2.0;
  k.cy = static_cast<double>(height) / 2.0;
  return k;
}

Result<CameraModel> CameraModel::create(const CameraIntrinsics& k) noexcept {
  const double vals[] = {k.fx, k.fy, k.cx, k.cy, k.k1, k.k2, k.k3, k.p1, k.p2};
  for (double v : vals) {
    if (!std::isfinite(v)) return Status::kInvalidArgument;
  }
  if (k.width == 0 || k.height == 0 || !(k.fx > 0) || !(k.fy > 0)) {
    return Status::kInvalidArgument;
  }
  return CameraModel(k);
}

CameraModel::CameraModel(const CameraIntrinsics& k) noexcept
    : k_(k),
      has_distortion_(k.k1 != 0 || k.k2 != 0 || k.k3 != 0 || k.p1 != 0 || k.p2 != 0),
      sx_(k.mirrored ? -1.0 : 1.0) {}

double CameraModel::fov_x() const noexcept {
  return 2.0 * std::atan(static_cast<double>(k_.width) / 2.0 / k_.fx);
}

void CameraModel::distort(double xu, double yu, double& xd, double& yd) const noexcept {
  const double r2 = xu * xu + yu * yu;
  const double g = 1.0 + r2 * (k_.k1 + r2 * (k_.k2 + r2 * k_.k3));
  xd = xu * g + 2.0 * k_.p1 * xu * yu + k_.p2 * (r2 + 2.0 * xu * xu);
  yd = yu * g + k_.p1 * (r2 + 2.0 * yu * yu) + 2.0 * k_.p2 * xu * yu;
}

void CameraModel::undistort(double xd, double yd, double& xu, double& yu) const noexcept {
  xu = xd;
  yu = yd;
  if (!has_distortion_) return;
  for (int it = 0; it < kMaxUndistortIterations; ++it) {
    double fx = 0, fy = 0;
    distort(xu, yu, fx, fy);
    const double ex = fx - xd, ey = fy - yd;
    if (std::fabs(ex) < 1e-15 && std::fabs(ey) < 1e-15) break;
    // Jacobian of distort().
    const double r2 = xu * xu + yu * yu;
    const double g = 1.0 + r2 * (k_.k1 + r2 * (k_.k2 + r2 * k_.k3));
    const double dg = k_.k1 + r2 * (2.0 * k_.k2 + 3.0 * k_.k3 * r2);  // dg/d(r2)
    const double j00 = g + 2.0 * xu * xu * dg + 2.0 * k_.p1 * yu + 6.0 * k_.p2 * xu;
    const double j01 = 2.0 * xu * yu * dg + 2.0 * k_.p1 * xu + 2.0 * k_.p2 * yu;
    const double j10 = 2.0 * xu * yu * dg + 2.0 * k_.p1 * xu + 2.0 * k_.p2 * yu;
    const double j11 = g + 2.0 * yu * yu * dg + 6.0 * k_.p1 * yu + 2.0 * k_.p2 * xu;
    const double det = j00 * j11 - j01 * j10;
    if (std::fabs(det) < 1e-300) break;
    xu -= (j11 * ex - j01 * ey) / det;
    yu -= (j00 * ey - j10 * ex) / det;
  }
}

void CameraModel::undistort_pixel(double x, double y, double& xu, double& yu) const noexcept {
  const double xd = sx_ * (x - k_.cx) / k_.fx;
  const double yd = (y - k_.cy) / k_.fy;
  double xn = 0, yn = 0;
  undistort(xd, yd, xn, yn);
  xu = k_.cx + sx_ * k_.fx * xn;
  yu = k_.cy + k_.fy * yn;
}

Vec3 CameraModel::pixel_to_ray(double x, double y) const noexcept {
  const double xd = sx_ * (x - k_.cx) / k_.fx;
  const double yd = (y - k_.cy) / k_.fy;
  double xn = 0, yn = 0;
  undistort(xd, yd, xn, yn);
  return normalized(Vec3{xn, yn, 1.0});
}

bool CameraModel::ray_to_pixel(const Vec3& v, double& x, double& y) const noexcept {
  if (!(v.z > 0)) return false;
  double xd = 0, yd = 0;
  distort(v.x / v.z, v.y / v.z, xd, yd);
  x = k_.cx + sx_ * k_.fx * xd;
  y = k_.cy + k_.fy * yd;
  return true;
}

}  // namespace st
