// SPDX-License-Identifier: Apache-2.0
// Lost-in-space solver on synthetic data (no fixtures needed).
#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

#include <gtest/gtest.h>
#include "alloc_guard.hpp"
#include "db_writer.hpp"
#include "generator.hpp"
#include "st/camera/camera.hpp"
#include "st/solver/lost_in_space.hpp"
#include "support/synthetic_sky.hpp"

namespace {
using namespace st;

using st::test::synthetic_db;

// Projects the database's stars through an ideal camera; brightest first.
std::vector<Centroid> render(const Database& db, const Quat& q, const CameraModel& cam,
                             double k_tetra3 = 0) {
  struct S {
    float mag;
    Centroid c;
  };
  std::vector<S> v;
  const double w = cam.intrinsics().width, h = cam.intrinsics().height;
  for (std::uint32_t i = 0; i < db.num_stars(); ++i) {
    const auto f = db.star_vector(i);
    const Vec3 c = rotate(q, Vec3{f[0], f[1], f[2]});
    double x = 0, y = 0;
    if (!cam.ray_to_pixel(c, x, y) || x <= 0 || y <= 0 || x >= w || y >= h) continue;
    if (k_tetra3 != 0) {
      // Apply tetra3's distortion model: r_u = r_d (1 - k' r_d^2) / (1 - k); solve for r_d.
      const double ux = x - w / 2, uy = y - h / 2;
      const double ru = std::hypot(ux, uy) / w * 2;
      double rd = ru;
      for (int it = 0; it < 50; ++it)
        rd -= (rd * (1 - k_tetra3 * rd * rd) / (1 - k_tetra3) - ru) /
              ((1 - 3 * k_tetra3 * rd * rd) / (1 - k_tetra3));
      const double s = ru > 0 ? rd / ru : 1;
      x = w / 2 + ux * s;
      y = h / 2 + uy * s;
    }
    v.push_back({db.star_magnitude(i), {x, y}});
  }
  std::stable_sort(v.begin(), v.end(), [](const S& a, const S& b) { return a.mag < b.mag; });
  std::vector<Centroid> out;
  for (const auto& s : v) out.push_back(s.c);
  return out;
}

Vec3 boresight(const Quat& q_cam_icrs) { return rotate(conjugate(q_cam_icrs), {0, 0, 1}); }

class SolverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto& s = synthetic_db();
    arena_mem_.resize(8 << 20);
    arena_ = std::make_unique<Arena>(arena_mem_);
    auto r = LostInSpaceSolver::create(s.db, SolverLimits{}, *arena_);
    ASSERT_TRUE(r.ok()) << to_string(r.status());
    solver_ = r.value();
  }
  std::vector<std::byte> arena_mem_;
  std::unique_ptr<Arena> arena_;
  LostInSpaceSolver solver_;
};

TEST_F(SolverTest, SolvesRandomAttitudesWithoutAllocating) {
  const Database& db = synthetic_db().db;
  auto cam = CameraModel::create(CameraModel::ideal(1280, 960, 17 * kDegToRad));
  ASSERT_TRUE(cam.ok());
  std::mt19937_64 g(5);
  std::uniform_real_distribution<double> u(-kPi, kPi);
  int solved = 0;
  for (int trial = 0; trial < 20; ++trial) {
    const Quat q = canonical(from_rotation_vector({u(g), u(g) / 2, u(g)}));
    const auto cents = render(db, q, cam.value());
    if (cents.size() < 8) continue;
    SolveResult r;
    SolveOptions opt;
    std::uint64_t allocs = 0;
    {
      st::test::AllocGuard guard(st::test::AllocMode::kCount);
      solver_.solve(cents, 1280, 960, opt, r);
      allocs = guard.allocations();
    }
    EXPECT_EQ(allocs, 0u);
    ASSERT_EQ(r.status, Status::kOk) << "trial " << trial << " n=" << cents.size();
    ++solved;
    const double err = angle_between_unit(boresight(r.q_cam_icrs), boresight(q)) / kArcsecToRad;
    // tetra3 computes the final rotation with the coarse (pattern-based) FOV, so exact
    // centroids still give arcsecond-level errors; parity keeps that behaviour.
    EXPECT_LT(err, 15.0) << "trial " << trial;
    EXPECT_LT(angle_between(r.q_cam_icrs, q) / kArcsecToRad, 60.0);
    EXPECT_NEAR(r.fov, 17 * kDegToRad, 17 * kDegToRad * 5e-4);
    EXPECT_GE(r.num_matches, 4u);
    EXPECT_TRUE(r.proper_rotation);
    EXPECT_LT(r.rmse_arcsec, 15.0);
    for (const auto& m : r.matches) {
      // Each matched centroid is the projection of its matched star.
      const auto f = db.star_vector(m.star_index);
      double x = 0, y = 0;
      ASSERT_TRUE(cam.value().ray_to_pixel(rotate(q, Vec3{f[0], f[1], f[2]}), x, y));
      EXPECT_NEAR(cents[m.centroid_index].x, x, 0.5);
      EXPECT_NEAR(cents[m.centroid_index].y, y, 0.5);
    }
  }
  EXPECT_GE(solved, 15);
}

TEST_F(SolverTest, VariantsAndFovHint) {
  const Database& db = synthetic_db().db;
  auto cam = CameraModel::create(CameraModel::ideal(1024, 1024, 16 * kDegToRad));
  const Quat q = canonical(from_rotation_vector({0.3, -1.1, 2.0}));
  const auto cents = render(db, q, cam.value());
  for (int mode = 0; mode < 4; ++mode) {
    SolveOptions opt;
    if (mode == 1) {
      opt.fov_estimate = 16.2 * kDegToRad;
      opt.fov_max_error = 1 * kDegToRad;
    }
    if (mode == 2) opt.distortion_mode = DistortionMode::kNone;
    if (mode == 3) opt.variants = SolverVariants::cedar();
    SolveResult r;
    ASSERT_EQ(solver_.solve(cents, 1024, 1024, opt, r), Status::kOk) << "mode " << mode;
    EXPECT_LT(angle_between_unit(boresight(r.q_cam_icrs), boresight(q)) / kArcsecToRad, 30.0);
    if (mode == 2) {
      EXPECT_TRUE(std::isnan(r.distortion));
    } else {
      EXPECT_NEAR(r.distortion, 0, 1e-3);
    }
  }
}

TEST_F(SolverTest, RecoversDistortionWithRangeSearch) {
  const Database& db = synthetic_db().db;
  auto cam = CameraModel::create(CameraModel::ideal(1280, 960, 18 * kDegToRad));
  const Quat q = canonical(from_rotation_vector({-0.7, 0.4, 0.9}));
  const auto cents = render(db, q, cam.value(), -0.08);
  SolveOptions opt;
  opt.distortion_mode = DistortionMode::kRange;
  opt.distortion_min = -0.2;
  opt.distortion_max = 0.1;
  SolveResult r;
  ASSERT_EQ(solver_.solve(cents, 1280, 960, opt, r), Status::kOk);
  EXPECT_NEAR(r.distortion, -0.08, 0.01);
  // Range mode fits the rotation on the raw (distorted) centroids, as tetra3 does.
  EXPECT_LT(angle_between_unit(boresight(r.q_cam_icrs), boresight(q)) / kArcsecToRad, 200.0);
}

TEST_F(SolverTest, NoFalseMatchOnRandomCentroids) {
  std::mt19937_64 g(9);
  std::uniform_real_distribution<double> ux(0.5, 1279.5), uy(0.5, 959.5);
  for (int trial = 0; trial < 5; ++trial) {
    std::vector<Centroid> c(30);
    for (auto& p : c) p = {ux(g), uy(g)};
    SolveResult r;
    EXPECT_EQ(solver_.solve(c, 1280, 960, SolveOptions{}, r), Status::kNoMatch);
    EXPECT_GT(r.stats.image_patterns, 0u);
  }
}

TEST_F(SolverTest, StatusesForLimits) {
  std::vector<Centroid> c = {{1, 1}, {2, 2}, {3, 3}};
  SolveResult r;
  EXPECT_EQ(solver_.solve(c, 100, 100, SolveOptions{}, r), Status::kTooFewStars);
  EXPECT_EQ(solver_.solve(c, 0, 100, SolveOptions{}, r), Status::kInvalidArgument);

  std::mt19937_64 g(3);
  std::uniform_real_distribution<double> ux(0.5, 999.5);
  std::vector<Centroid> junk(20);
  for (auto& p : junk) p = {ux(g), ux(g)};
  SolveOptions opt;
  opt.budget.max_image_patterns = 3;
  EXPECT_EQ(solver_.solve(junk, 1000, 1000, opt, r), Status::kWorkBudgetExhausted);
  EXPECT_EQ(r.stats.image_patterns, 4u);
  opt = SolveOptions{};
  opt.budget.max_slot_reads = 10;
  EXPECT_EQ(solver_.solve(junk, 1000, 1000, opt, r), Status::kWorkBudgetExhausted);

  struct TickClock final : Clock {
    mutable std::int64_t t = 0;
    std::int64_t now_ns() const noexcept override { return t += 1'000'000; }
  } tick;
  opt = SolveOptions{};
  opt.clock = &tick;
  opt.timeout_ns = 5'000'000;
  EXPECT_EQ(solver_.solve(junk, 1000, 1000, opt, r), Status::kTimeout);

  std::atomic<bool> cancel{true};
  opt = SolveOptions{};
  opt.cancel = &cancel;
  EXPECT_EQ(solver_.solve(junk, 1000, 1000, opt, r), Status::kCancelled);

  opt = SolveOptions{};
  opt.variants.nearby_trim_factor = 5;  // above SolverLimits::max_nearby_trim_factor
  EXPECT_EQ(solver_.solve(junk, 1000, 1000, opt, r), Status::kCapacityExceeded);
}

TEST(SolverCreate, ArenaTooSmall) {
  auto& s = synthetic_db();
  std::vector<std::byte> tiny(512);
  Arena a(tiny);
  EXPECT_EQ(LostInSpaceSolver::create(s.db, SolverLimits{}, a).status(), Status::kArenaExhausted);
  // The stated bound is enough.
  const std::size_t need = LostInSpaceSolver::workspace_bytes(s.db, SolverLimits{});
  std::vector<std::byte> exact(need);
  Arena b(exact);
  EXPECT_TRUE(LostInSpaceSolver::create(s.db, SolverLimits{}, b).ok());
  EXPECT_LE(b.used(), need);
}
}  // namespace
