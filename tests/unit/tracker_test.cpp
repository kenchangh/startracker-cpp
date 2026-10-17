// SPDX-License-Identifier: Apache-2.0
#include "st/track/tracker.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <set>
#include <vector>

#include <gtest/gtest.h>
#include "alloc_guard.hpp"
#include "render.hpp"
#include "st/math/attitude.hpp"
#include "support/synthetic_sky.hpp"

namespace {
using namespace st;
using st::test::synthetic_db;

Vec3 vec_of(const Database& db, std::uint32_t i) {
  const auto f = db.star_vector(i);
  return {f[0], f[1], f[2]};
}

// Projected centroids of the database stars (brightest first) with Gaussian noise.
std::vector<Centroid> observe(const Database& db, const Quat& q, const CameraModel& cam,
                              double noise_px, std::mt19937_64& g) {
  std::normal_distribution<double> n(0, noise_px);
  struct S {
    float mag;
    Centroid c;
  };
  std::vector<S> v;
  const double w = cam.intrinsics().width, h = cam.intrinsics().height;
  for (std::uint32_t i = 0; i < db.num_stars(); ++i) {
    double x = 0, y = 0;
    if (!cam.ray_to_pixel(rotate(q, vec_of(db, i)), x, y) || x < 0 || y < 0 || x >= w || y >= h)
      continue;
    v.push_back({db.star_magnitude(i), {x + n(g), y + n(g)}});
  }
  std::stable_sort(v.begin(), v.end(), [](const S& a, const S& b) { return a.mag < b.mag; });
  std::vector<Centroid> out;
  for (const auto& s : v) out.push_back(s.c);
  return out;
}

CameraIntrinsics test_camera() {
  CameraIntrinsics k = CameraModel::ideal(1280, 960, 17 * kDegToRad);
  k.cx += 4.5;
  k.cy -= 3.0;
  k.k1 = -0.05;
  return k;
}

class TrackerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto& s = synthetic_db();
    mem_.resize(64 << 20);
    arena_ = std::make_unique<Arena>(mem_);
    auto idx = SkyIndex::create(s.db, 16, *arena_);
    ASSERT_TRUE(idx.ok());
    index_ = idx.value();
    auto lis = LostInSpaceSolver::create(s.db, SolverLimits{}, *arena_);
    ASSERT_TRUE(lis.ok());
    lis_ = lis.value();
  }
  Tracker make(TrackerConfig cfg) {
    auto t = Tracker::create(index_, &lis_, cfg, *arena_);
    EXPECT_TRUE(t.ok());
    return t.value();
  }
  std::vector<std::byte> mem_;
  std::unique_ptr<Arena> arena_;
  SkyIndex index_;
  LostInSpaceSolver lis_;
};

TEST_F(TrackerTest, SkyIndexMatchesBruteForce) {
  const Database& db = synthetic_db().db;
  std::mt19937_64 g(1);
  std::uniform_real_distribution<double> u(-1, 1);
  for (int t = 0; t < 50; ++t) {
    const Vec3 c = normalized(Vec3{u(g), u(g), u(g)});
    const double r = (0.02 + 0.3 * std::fabs(u(g)));
    std::set<std::uint32_t> a, b;
    index_.query(c, r, [&](std::uint32_t s) { a.insert(s); });
    for (std::uint32_t i = 0; i < db.num_stars(); ++i) {
      if (dot(c, vec_of(db, i)) >= std::cos(r)) b.insert(i);
    }
    EXPECT_EQ(a, b) << "trial " << t;
  }
}

TEST_F(TrackerTest, AcquiresTracksAndEstimatesRate) {
  const Database& db = synthetic_db().db;
  TrackerConfig cfg;
  cfg.camera = test_camera();
  Tracker tr = make(cfg);
  auto cam = CameraModel::create(cfg.camera);
  ASSERT_TRUE(cam.ok());
  std::mt19937_64 g(2);
  const Vec3 omega{0.002, -0.006, 0.004};  // rad/s, camera frame (~0.43 deg/s)
  Quat q = attitude_from_radec_roll(1.2, 0.4, 2.1);
  const std::int64_t dt_ns = 100'000'000;  // 10 Hz
  int tracked = 0;
  for (int k = 0; k < 60; ++k) {
    const std::int64_t t = k * dt_ns;
    const auto cents = observe(db, q, cam.value(), 0.15, g);
    AttitudeSolution s;
    std::uint64_t allocs = 0;
    {
      st::test::AllocGuard guard(st::test::AllocMode::kCount);
      tr.update(t, cents, s);
      allocs = guard.allocations();
    }
    EXPECT_EQ(allocs, 0u);
    ASSERT_EQ(s.status, Status::kOk) << "frame " << k;
    if (k == 0) {
      EXPECT_EQ(s.mode, TrackMode::kLostInSpace);
    } else {
      EXPECT_EQ(s.mode, TrackMode::kTracking);
      ++tracked;
    }
    // Boresight is well determined; roll (about +Z) is the weak axis with a ~17 deg FOV.
    const Vec3 bt = rotate(conjugate(q), Vec3{0, 0, 1}),
               bs = rotate(conjugate(s.q_cam_icrs), Vec3{0, 0, 1});
    EXPECT_LT(angle_between_unit(bt, bs) / kArcsecToRad, 8.0) << "frame " << k;
    // The reported covariance is consistent with the actual error (0.15 px = 7" noise).
    const double err = angle_between(s.q_cam_icrs, q) / kArcsecToRad;
    const double sig =
        std::sqrt(s.covariance.m[0][0] + s.covariance.m[1][1] + s.covariance.m[2][2]) /
        kArcsecToRad;
    EXPECT_GT(sig, 0.5);
    EXPECT_LT(sig, 40.0);
    EXPECT_LT(err, 3.0 * sig) << "frame " << k;
    // Roll dominates: the Z variance is the largest.
    EXPECT_GT(s.covariance.m[2][2], s.covariance.m[0][0]);
    if (k > 20) {
      ASSERT_TRUE(s.rate_valid);
      EXPECT_LT(norm(s.rate - omega), 0.03 * norm(omega)) << "frame " << k;
    }
    q = normalized(from_rotation_vector(-omega * 0.1) * q);
  }
  EXPECT_EQ(tracked, 59);
}

// Coarse pixels (~88"/px): a few centroids paired with the wrong star (3 px off, inside the match
// radius) must be rejected by the median-based cut; an RMS-based cut alone keeps them and the
// fit fails the quality gate. Seen with ROI tracking on real star fields.
TEST_F(TrackerTest, RejectsWrongPairsWithCoarsePixels) {
  const Database& db = synthetic_db().db;
  TrackerConfig cfg;
  cfg.camera = CameraModel::ideal(728, 544, 17.8 * kDegToRad);
  Tracker tr = make(cfg);
  auto cam = CameraModel::create(cfg.camera);
  ASSERT_TRUE(cam.ok());
  std::mt19937_64 g(9);
  const Quat q = attitude_from_radec_roll(2.2, -0.4, 0.9);
  AttitudeSolution s;
  tr.set_attitude(0, q);
  auto cents = observe(db, q, cam.value(), 0.1, g);
  ASSERT_GE(cents.size(), 30u);
  cents.resize(30);
  for (std::size_t i = 20; i < 24; ++i) cents[i] = {cents[i].x + 2.5, cents[i].y - 2.0};
  ASSERT_EQ(tr.update(100'000'000, cents, s), Status::kOk);
  EXPECT_EQ(s.matches, 26u);
  EXPECT_LT(s.rmse_arcsec, 25.0);
  // Error consistent with the reported covariance (0.1 px noise at ~88"/px).
  const double sig =
      std::sqrt(s.covariance.m[0][0] + s.covariance.m[1][1] + s.covariance.m[2][2]) / kArcsecToRad;
  EXPECT_LT(angle_between(s.q_cam_icrs, q) / kArcsecToRad, 3.0 * sig);
}

TEST_F(TrackerTest, LosesAndReacquires) {
  const Database& db = synthetic_db().db;
  TrackerConfig cfg;
  cfg.camera = test_camera();
  cfg.lost_after_failures = 2;
  Tracker tr = make(cfg);
  auto cam = CameraModel::create(cfg.camera);
  std::mt19937_64 g(3);
  std::uniform_real_distribution<double> ux(1, 1279), uy(1, 959);
  Quat q = attitude_from_radec_roll(4.0, -0.7, 0.3);
  AttitudeSolution s;
  ASSERT_EQ(tr.update(0, observe(db, q, cam.value(), 0.1, g), s), Status::kOk);
  std::vector<Centroid> junk(30);
  for (auto& c : junk) c = {ux(g), uy(g)};
  EXPECT_EQ(tr.update(100'000'000, junk, s), Status::kNoMatch);
  EXPECT_EQ(tr.mode(), TrackMode::kTracking);  // coasting after one failure
  EXPECT_NE(tr.update(200'000'000, junk, s), Status::kOk);
  EXPECT_EQ(tr.mode(), TrackMode::kLost);
  // A different part of the sky: tracking cannot continue, lost-in-space must reacquire.
  q = attitude_from_radec_roll(1.0, 0.9, 5.0);
  ASSERT_EQ(tr.update(300'000'000, observe(db, q, cam.value(), 0.1, g), s), Status::kOk);
  EXPECT_EQ(s.mode, TrackMode::kLostInSpace);
  EXPECT_LT(angle_between(s.q_cam_icrs, q) / kArcsecToRad, 30.0);
}

// A perfect gyro lets the tracker follow 5 deg/s from the first tracked frame.
struct PerfectGyro final : RateSource {
  Vec3 omega;
  bool rotation_between(std::int64_t t0, std::int64_t t1, Quat& dq) noexcept override {
    dq = from_rotation_vector(-omega * (static_cast<double>(t1 - t0) * 1e-9));
    return true;
  }
};

TEST_F(TrackerTest, GyroPropagation) {
  const Database& db = synthetic_db().db;
  TrackerConfig cfg;
  cfg.camera = test_camera();
  cfg.rate_uncertainty = 0;  // the gyro is exact; keep the gate tight
  Tracker tr = make(cfg);
  PerfectGyro gyro;
  gyro.omega = Vec3{0.05, 0.06, -0.02};  // ~4.6 deg/s: 21 px per frame at 10 Hz
  tr.set_rate_source(&gyro);
  auto cam = CameraModel::create(cfg.camera);
  std::mt19937_64 g(4);
  Quat q = attitude_from_radec_roll(2.5, 0.1, 1.0);
  for (int k = 0; k < 10; ++k) {
    AttitudeSolution s;
    ASSERT_EQ(tr.update(k * 100'000'000LL, observe(db, q, cam.value(), 0.1, g), s), Status::kOk)
        << k;
    EXPECT_EQ(s.mode, k == 0 ? TrackMode::kLostInSpace : TrackMode::kTracking);
    EXPECT_LT(angle_between(s.q_cam_icrs, q) / kArcsecToRad, 30.0);
    q = normalized(from_rotation_vector(-gyro.omega * 0.1) * q);
  }
}

TEST_F(TrackerTest, RoiTrackingOnRenderedFrames) {
  const Database& db = synthetic_db().db;
  TrackerConfig cfg;
  cfg.camera = test_camera();
  Tracker tr = make(cfg);
  CentroiderConfig ccfg;
  ccfg.max_width = 1280;
  ccfg.max_height = 960;
  auto cent = Centroider::create(ccfg, *arena_);
  ASSERT_TRUE(cent.ok());
  const auto stars = tools::stars_from_database(db);
  tools::RenderConfig rc;
  rc.camera = cfg.camera;
  rc.exposure_s = 0.05;
  Quat q = attitude_from_radec_roll(0.5, 0.2, 0.7);
  const Vec3 omega{0.0, 0.003, 0.001};
  std::vector<Spot> spots(64);
  std::vector<Centroid> cents(64);
  std::vector<Roi> rois(32);
  std::vector<Spot> rspots(32);
  std::vector<std::uint8_t> found_storage(32);
  for (int k = 0; k < 6; ++k) {
    rc.q_cam_icrs = q;
    rc.body_rate = omega;
    rc.seed = static_cast<std::uint64_t>(k + 1);
    const auto img = tools::render(stars, rc);
    const std::int64_t t = k * 100'000'000LL;
    std::uint32_t n = 0;
    if (k == 0) {
      auto r = cent.value().extract(img.image.view(), spots);
      ASSERT_TRUE(r.ok());
      for (std::uint32_t i = 0; i < r.value(); ++i) cents[n++] = {spots[i].x, spots[i].y};
    } else {
      const std::uint32_t nr = tr.predict_rois(t, 8, rois);
      ASSERT_GE(nr, 5u);
      bool found[32];
      ASSERT_EQ(cent.value().extract_rois(img.image.view(), std::span<const Roi>(rois.data(), nr),
                                          RoiConfig{}, std::span<Spot>(rspots.data(), nr),
                                          std::span<bool>(found, nr)),
                Status::kOk);
      for (std::uint32_t i = 0; i < nr; ++i) {
        if (found[i]) cents[n++] = {rspots[i].x, rspots[i].y};
      }
    }
    AttitudeSolution s;
    ASSERT_EQ(tr.update(t, std::span<const Centroid>(cents.data(), n), s), Status::kOk)
        << "frame " << k;
    // Truth is the mid-exposure attitude.
    const Quat qmid = from_rotation_vector(-0.5 * rc.exposure_s * omega) * q;
    EXPECT_LT(angle_between(s.q_cam_icrs, qmid) / kArcsecToRad, 30.0) << "frame " << k;
    q = normalized(from_rotation_vector(-omega * 0.1) * q);
  }
}
}  // namespace

namespace {
// Earth-fixed camera: the sky turns about the celestial pole at the sidereal rate. The tracker's
// rate estimate must recover 7.2921e-5 rad/s about the pole direction.
TEST_F(TrackerTest, SiderealRotation) {
  const Database& db = synthetic_db().db;
  TrackerConfig cfg;
  cfg.camera = test_camera();
  cfg.rate_window = 32;
  Tracker tr = make(cfg);
  auto cam = CameraModel::create(cfg.camera);
  std::mt19937_64 g(7);
  constexpr double kSidereal = 7.2921150e-5;  // rad/s
  const Quat q0 = attitude_from_radec_roll(3.3, 0.6, 1.4);
  AttitudeSolution s;
  for (int k = 0; k < 120; ++k) {
    const double t = k;  // 1 Hz
    const Quat q = q0 * from_rotation_vector({0, 0, -kSidereal * t});
    ASSERT_EQ(
        tr.update(static_cast<std::int64_t>(t * 1e9), observe(db, q, cam.value(), 0.05, g), s),
        Status::kOk)
        << k;
  }
  ASSERT_TRUE(s.rate_valid);
  const Vec3 pole_cam = rotate(s.q_cam_icrs, Vec3{0, 0, 1});
  const Vec3 expected = kSidereal * pole_cam;
  EXPECT_NEAR(norm(s.rate), kSidereal, 0.05 * kSidereal);
  EXPECT_LT(angle_between_unit(normalized(s.rate), pole_cam), 5 * kDegToRad);
  EXPECT_LT(norm(s.rate - expected), 0.08 * kSidereal);
}
}  // namespace
