// SPDX-License-Identifier: Apache-2.0
#include "st/camera/camera.hpp"

#include <cmath>

#include "unit/no_alloc_fixture.hpp"

namespace {
using namespace st;
using CameraTest = st::test::NoAllocTest;

TEST_F(CameraTest, IdealCameraGeometry) {
  auto cam = CameraModel::create(CameraModel::ideal(1024, 768, 11.4 * kDegToRad));
  ASSERT_TRUE(cam.ok());
  const CameraModel& c = cam.value();
  EXPECT_NEAR(c.fov_x(), 11.4 * kDegToRad, 1e-12);
  const Vec3 z = c.pixel_to_ray(512, 384);
  EXPECT_NEAR(z.z, 1.0, 1e-15);
  // Right edge of the image is at half the horizontal FOV, along +X.
  const Vec3 r = c.pixel_to_ray(1024, 384);
  EXPECT_NEAR(std::atan2(r.x, r.z), 5.7 * kDegToRad, 1e-12);
  // Down in the image is +Y.
  EXPECT_GT(c.pixel_to_ray(512, 700).y, 0);
}

TEST_F(CameraTest, RoundTripWithDistortionAndMirror) {
  CameraIntrinsics k = CameraModel::ideal(1280, 960, 20 * kDegToRad);
  k.cx += 3.5;
  k.cy -= 2.25;
  k.fy *= 1.001;
  k.k1 = -0.12;
  k.k2 = 0.03;
  k.k3 = -0.004;
  k.p1 = 1e-3;
  k.p2 = -5e-4;
  for (bool mirrored : {false, true}) {
    k.mirrored = mirrored;
    auto cam = CameraModel::create(k);
    ASSERT_TRUE(cam.ok());
    for (double x = 0.5; x < 1280; x += 97.3) {
      for (double y = 0.5; y < 960; y += 61.7) {
        const Vec3 v = cam.value().pixel_to_ray(x, y);
        EXPECT_NEAR(norm(v), 1.0, 1e-14);
        double px = 0, py = 0;
        ASSERT_TRUE(cam.value().ray_to_pixel(v, px, py));
        EXPECT_NEAR(px, x, 1e-8);
        EXPECT_NEAR(py, y, 1e-8);
      }
    }
  }
  k.mirrored = true;
  k.k1 = k.k2 = k.k3 = k.p1 = k.p2 = 0;
  auto m = CameraModel::create(k);
  ASSERT_TRUE(m.ok());
  EXPECT_LT(m.value().pixel_to_ray(1200, 480).x, 0);  // mirrored: right of centre is -X
}

TEST_F(CameraTest, BehindCameraAndInvalid) {
  auto cam = CameraModel::create(CameraModel::ideal(100, 100, 0.2));
  ASSERT_TRUE(cam.ok());
  double x = -1, y = -1;
  EXPECT_FALSE(cam.value().ray_to_pixel({0, 0, -1}, x, y));
  EXPECT_EQ(x, -1);
  CameraIntrinsics bad = CameraModel::ideal(100, 100, 0.2);
  bad.fx = 0;
  EXPECT_EQ(CameraModel::create(bad).status(), Status::kInvalidArgument);
  bad = CameraModel::ideal(0, 100, 0.2);
  EXPECT_EQ(CameraModel::create(bad).status(), Status::kInvalidArgument);
  bad = CameraModel::ideal(100, 100, 0.2);
  bad.k1 = std::nan("");
  EXPECT_EQ(CameraModel::create(bad).status(), Status::kInvalidArgument);
  const auto f = CameraModel::from_focal_length(100, 100, 25.0, 0.005);
  EXPECT_DOUBLE_EQ(f.fx, 5000.0);
}

TEST_F(CameraTest, UndistortPixel) {
  CameraIntrinsics k = CameraModel::ideal(1000, 1000, 0.3);
  k.k1 = -0.2;
  auto cam = CameraModel::create(k);
  ASSERT_TRUE(cam.ok());
  double xu = 0, yu = 0;
  cam.value().undistort_pixel(500, 500, xu, yu);
  EXPECT_NEAR(xu, 500, 1e-12);
  cam.value().undistort_pixel(900, 500, xu, yu);
  EXPECT_GT(xu, 900);  // barrel distortion pulls points inwards; undistorting pushes out
}
}  // namespace
