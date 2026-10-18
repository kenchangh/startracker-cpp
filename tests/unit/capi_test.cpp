// SPDX-License-Identifier: Apache-2.0
#include "st/c/st.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>
#include "alloc_guard.hpp"
#include "sequence.hpp"
#include "st/math/attitude.hpp"
#include "st/solver/lost_in_space.hpp"
#include "support/synthetic_sky.hpp"

namespace {
using namespace st;
using st::test::synthetic_db;

struct Freer {
  void operator()(void* p) const { std::free(p); }
};

TEST(CApi, StatusAndVersion) {
  EXPECT_STREQ(st_status_string(ST_NO_MATCH), "ST_NO_MATCH");
  EXPECT_STREQ(st_status_string(static_cast<st_status>(Status::kIoError)), "ST_IO_ERROR");
  EXPECT_STRNE(st_version(), "");
  st_config c;
  st_config_init(&c);
  EXPECT_EQ(c.max_centroids, 64u);
  EXPECT_EQ(c.roi_tracking, 1);
}

TEST(CApi, TracksRenderedSequenceWithoutAllocating) {
  const auto& blob = synthetic_db().blob;
  st_config cfg;
  st_config_init(&cfg);
  cfg.camera = st_camera_ideal(640, 480, 17);
  const std::size_t need = st_tracker_memory_size(&cfg, blob.data(), blob.size());
  ASSERT_GT(need, 0u);
  std::unique_ptr<void, Freer> mem(std::aligned_alloc(64, (need + 63) / 64 * 64));
  st_tracker* t = nullptr;
  EXPECT_EQ(st_tracker_create(&cfg, blob.data(), blob.size(), mem.get(), need / 2, &t),
            ST_ARENA_EXHAUSTED);
  EXPECT_EQ(st_tracker_create(&cfg, blob.data(), blob.size(), static_cast<char*>(mem.get()) + 8,
                              need, &t),
            ST_INVALID_ARGUMENT);
  std::vector<std::byte> junk(blob.begin(), blob.end());
  junk[300] ^= std::byte{1};
  EXPECT_NE(st_tracker_create(&cfg, junk.data(), junk.size(), mem.get(), need, &t), ST_OK);
  ASSERT_EQ(st_tracker_create(&cfg, blob.data(), blob.size(), mem.get(), need, &t), ST_OK);

  tools::RenderConfig rc;
  rc.camera = CameraModel::ideal(640, 480, 17 * kDegToRad);
  rc.exposure_s = 0.05;
  rc.q_cam_icrs = attitude_from_radec_roll(2.0, -0.3, 1.0);
  const auto seq = tools::render_sequence(tools::stars_from_database(synthetic_db().db), rc,
                                          Vec3{0.001, 0.004, 0.0}, 6, 100'000'000);
  const std::size_t fb = seq.format.bytes();
  std::uint64_t allocs = 0;
  st_solution out[6];
  {
    st::test::AllocGuard g(st::test::AllocMode::kCount);
    for (std::size_t k = 0; k < 6; ++k) {
      st_track_image(t, seq.t_ns[k], seq.pixels.data() + k * fb, 640, 480, 0, ST_MONO16, &out[k]);
    }
    allocs = g.allocations();
  }
  EXPECT_EQ(allocs, 0u);
  for (std::size_t k = 0; k < 6; ++k) {
    ASSERT_EQ(out[k].status, ST_OK) << k;
    EXPECT_EQ(out[k].mode, k == 0 ? ST_MODE_LOST_IN_SPACE : ST_MODE_TRACKING);
    EXPECT_EQ(out[k].used_rois, k == 0 ? 0 : 1);
    const Quat q{out[k].q_cam_icrs[0], out[k].q_cam_icrs[1], out[k].q_cam_icrs[2],
                 out[k].q_cam_icrs[3]};
    EXPECT_LT(angle_between(q, seq.truth[k]) / kArcsecToRad, 40.0) << k;
    EXPECT_GT(out[k].covariance[8], 0.0);
  }

  // Centroids + lost-in-space through the C API agree with tracking.
  st_centroid c[64];
  std::uint32_t n = 0;
  ASSERT_EQ(st_extract_centroids(t, seq.pixels.data(), 640, 480, 0, ST_MONO16, c, 64, &n), ST_OK);
  EXPECT_GE(n, 8u);
  st_lis_options o{};
  o.fov_estimate_deg = 17;
  o.fov_max_error_deg = 1;
  st_lis_result r;
  ASSERT_EQ(st_solve_lost_in_space(t, c, n, 640, 480, &o, &r), ST_OK);
  EXPECT_NEAR(r.fov_deg, 17, 0.3);
  // Reset -> lost-in-space again; set_attitude -> tracking.
  st_tracker_reset(t);
  st_solution s;
  ASSERT_EQ(st_track_centroids(t, 0, c, n, &s), ST_OK);
  EXPECT_EQ(s.mode, ST_MODE_LOST_IN_SPACE);
  st_tracker_reset(t);
  st_tracker_set_attitude(t, 0, s.q_cam_icrs);
  ASSERT_EQ(st_track_centroids(t, 0, c, n, &s), ST_OK);
  EXPECT_EQ(s.mode, ST_MODE_TRACKING);
  // Argument checks.
  EXPECT_EQ(st_track_image(nullptr, 0, seq.pixels.data(), 640, 480, 0, ST_MONO16, &s),
            ST_INVALID_ARGUMENT);
  EXPECT_EQ(st_track_image(t, 0, seq.pixels.data(), 1280, 480, 0, ST_MONO16, &s),
            ST_INVALID_ARGUMENT);
  EXPECT_EQ(st_tracker_memory_size(&cfg, junk.data(), junk.size()), 0u);
}

// ST_VARIANT_CEDAR runs the same search as the C++ SolverVariants::cedar() with cedar-solve's
// 1e-5 threshold, for single solves and for the tracker's lost-in-space step. ESA stays the
// default, and unknown variants are rejected.
TEST(CApi, CedarVariantMatchesCxx) {
  const auto& s = synthetic_db();
  st_config cfg;
  st_config_init(&cfg);
  EXPECT_EQ(cfg.variant, static_cast<uint32_t>(ST_VARIANT_ESA));
  cfg.camera = st_camera_ideal(640, 480, 17);
  const std::size_t need = st_tracker_memory_size(&cfg, s.blob.data(), s.blob.size());
  ASSERT_GT(need, 0u);
  std::unique_ptr<void, Freer> mem(std::aligned_alloc(64, (need + 63) / 64 * 64));
  st_tracker* t = nullptr;
  cfg.variant = 7;
  EXPECT_EQ(st_tracker_create(&cfg, s.blob.data(), s.blob.size(), mem.get(), need, &t),
            ST_INVALID_ARGUMENT);
  cfg.variant = ST_VARIANT_CEDAR;
  EXPECT_EQ(st_tracker_memory_size(&cfg, s.blob.data(), s.blob.size()), need);
  ASSERT_EQ(st_tracker_create(&cfg, s.blob.data(), s.blob.size(), mem.get(), need, &t), ST_OK);

  tools::RenderConfig rc;
  rc.camera = CameraModel::ideal(640, 480, 17 * kDegToRad);
  rc.exposure_s = 0.05;
  rc.q_cam_icrs = attitude_from_radec_roll(1.1, 0.4, -0.7);
  const auto seq = tools::render_sequence(tools::stars_from_database(s.db), rc,
                                          Vec3{0.0, 0.0, 0.0}, 1, 0);
  st_centroid c[64];
  std::uint32_t n = 0;
  ASSERT_EQ(st_extract_centroids(t, seq.pixels.data(), 640, 480, 0, ST_MONO16, c, 64, &n), ST_OK);
  ASSERT_GE(n, 8u);

  std::vector<std::byte> arena_mem(8 << 20);
  Arena arena(arena_mem);
  auto lis = LostInSpaceSolver::create(s.db, SolverLimits{}, arena);
  ASSERT_TRUE(lis.ok());
  std::vector<Centroid> cents(n);
  for (std::uint32_t i = 0; i < n; ++i) cents[i] = {c[i].x, c[i].y};

  for (const uint32_t variant : {ST_VARIANT_ESA, ST_VARIANT_CEDAR}) {
    SCOPED_TRACE(variant);
    st_lis_options o{};
    o.variant = variant;
    st_lis_result r;
    ASSERT_EQ(st_solve_lost_in_space(t, c, n, 640, 480, &o, &r), ST_OK);
    SolveOptions so;
    if (variant == ST_VARIANT_CEDAR) {
      so.variants = SolverVariants::cedar();
      so.match_threshold = 1e-5;
    }
    SolveResult want;
    ASSERT_EQ(lis.value().solve(cents, 640, 480, so, want), Status::kOk);
    EXPECT_EQ(r.num_matches, want.num_matches);
    EXPECT_DOUBLE_EQ(r.ra_deg, want.ra_deg);
    EXPECT_DOUBLE_EQ(r.dec_deg, want.dec_deg);
    EXPECT_DOUBLE_EQ(r.roll_deg, want.roll_deg);
    EXPECT_LT(angle_between(Quat{r.q_cam_icrs[0], r.q_cam_icrs[1], r.q_cam_icrs[2],
                                 r.q_cam_icrs[3]},
                            seq.truth[0]) /
                  kArcsecToRad,
              40.0);
  }
  st_lis_options bad{};
  bad.variant = 2;
  st_lis_result r;
  EXPECT_EQ(st_solve_lost_in_space(t, c, n, 640, 480, &bad, &r), ST_INVALID_ARGUMENT);
  EXPECT_EQ(r.status, ST_INVALID_ARGUMENT);

  // The tracker's lost-in-space step uses the configured variant.
  st_solution sol;
  ASSERT_EQ(st_track_image(t, 0, seq.pixels.data(), 640, 480, 0, ST_MONO16, &sol), ST_OK);
  EXPECT_EQ(sol.mode, ST_MODE_LOST_IN_SPACE);
  EXPECT_LT(angle_between(Quat{sol.q_cam_icrs[0], sol.q_cam_icrs[1], sol.q_cam_icrs[2],
                               sol.q_cam_icrs[3]},
                          seq.truth[0]) /
                kArcsecToRad,
            40.0);
}

}  // namespace
