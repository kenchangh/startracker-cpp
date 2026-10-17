// SPDX-License-Identifier: Apache-2.0
#include "st/centroid/centroider.hpp"

#include <cmath>
#include <memory>
#include <random>
#include <vector>

#include <gtest/gtest.h>
#include "alloc_guard.hpp"
#include "render.hpp"

namespace {
using namespace st;

struct Frame {
  std::uint32_t w, h;
  std::vector<std::uint16_t> px;
  ImageView view() const { return {px.data(), w, h, 0, PixelFormat::kMono16}; }
};

// Gaussian spot (pixel-integrated) on a flat noisy background.
Frame make_frame(std::uint32_t w, std::uint32_t h, const std::vector<std::array<double, 3>>& spots,
                 double sigma, unsigned seed, double noise = 3, double bg = 100) {
  Frame f{w, h, std::vector<std::uint16_t>(static_cast<std::size_t>(w) * h)};
  std::mt19937 g(seed);
  std::normal_distribution<double> n(0, noise);
  const double k = 1 / (std::sqrt(2.0) * sigma);
  for (std::uint32_t y = 0; y < h; ++y)
    for (std::uint32_t x = 0; x < w; ++x) {
      double v = bg + n(g);
      for (const auto& s : spots) {
        const double fx = 0.5 * (std::erf((x + 1 - s[0]) * k) - std::erf((x - s[0]) * k));
        const double fy = 0.5 * (std::erf((y + 1 - s[1]) * k) - std::erf((y - s[1]) * k));
        v += s[2] * fx * fy;
      }
      f.px[static_cast<std::size_t>(y) * w + x] =
          static_cast<std::uint16_t>(std::clamp(std::round(v), 0.0, 65535.0));
    }
  return f;
}

class CentroiderTest : public ::testing::Test {
 protected:
  Centroider make(CentroiderConfig cfg) {
    mem_.assign(Centroider::workspace_bytes(cfg), std::byte{0});
    arena_ = std::make_unique<Arena>(mem_);
    auto c = Centroider::create(cfg, *arena_);
    EXPECT_TRUE(c.ok());
    return c.value();
  }
  std::vector<std::byte> mem_;
  std::unique_ptr<Arena> arena_;
};

TEST_F(CentroiderTest, SubPixelAccuracyAndOrder) {
  CentroiderConfig cfg;
  cfg.max_width = 320;
  cfg.max_height = 240;
  auto c = make(cfg);
  const std::vector<std::array<double, 3>> stars = {
      {50.3, 60.7, 20000}, {200.81, 120.25, 60000}, {150.5, 200.5, 9000}, {270.13, 30.92, 30000}};
  const Frame f = make_frame(320, 240, stars, 1.2, 1);
  std::vector<Spot> out(16);
  CentroidStats st;
  std::uint64_t allocs = 0;
  Result<std::uint32_t> n = Status::kNotInitialized;
  {
    st::test::AllocGuard guard(st::test::AllocMode::kCount);
    n = c.extract(f.view(), out, &st);
    allocs = guard.allocations();
  }
  EXPECT_EQ(allocs, 0u);
  ASSERT_TRUE(n.ok());
  ASSERT_EQ(n.value(), 4u);
  // Brightest first.
  EXPECT_NEAR(out[0].x, 200.81, 0.05);
  EXPECT_NEAR(out[0].y, 120.25, 0.05);
  EXPECT_NEAR(out[1].x, 270.13, 0.05);
  EXPECT_NEAR(out[2].x, 50.3, 0.05);
  EXPECT_NEAR(out[3].x, 150.5, 0.05);
  EXPECT_GT(out[0].sum, out[1].sum);
  EXPECT_GT(st.threshold, 0.0f);
  EXPECT_EQ(st.accepted, 4u);
}

TEST_F(CentroiderTest, EightBitStrideAndMaxReturned) {
  CentroiderConfig cfg;
  cfg.max_width = 200;
  cfg.max_height = 100;
  cfg.max_returned = 2;
  auto c = make(cfg);
  const Frame f16 =
      make_frame(160, 100, {{40, 50, 3000}, {80, 50, 5000}, {120, 50, 4000}}, 1.0, 2, 1.5, 20);
  // Pack into 8-bit with a padded stride.
  const std::size_t stride = 176;
  std::vector<std::uint8_t> b(stride * 100, 0);
  for (std::uint32_t y = 0; y < 100; ++y)
    for (std::uint32_t x = 0; x < 160; ++x)
      b[y * stride + x] = static_cast<std::uint8_t>(std::min<int>(255, f16.px[y * 160 + x]));
  std::vector<Spot> out(8);
  auto n = c.extract({b.data(), 160, 100, stride, PixelFormat::kMono8}, out);
  ASSERT_TRUE(n.ok());
  ASSERT_EQ(n.value(), 2u);
  EXPECT_NEAR(out[0].x, 80, 0.1);
  EXPECT_NEAR(out[1].x, 120, 0.1);
}

TEST_F(CentroiderTest, HotPixelsAndTrails) {
  CentroiderConfig cfg;
  cfg.max_width = 200;
  cfg.max_height = 200;
  cfg.use_hot_pixel_map = true;
  cfg.max_axis_ratio = 3.0;
  cfg.min_area = 3;
  cfg.max_area = 400;
  auto c = make(cfg);
  Frame f = make_frame(200, 200, {{100.4, 100.6, 15000}}, 1.1, 3);
  // A 3x3 hot cluster would pass the area filter; the map must remove it.
  for (std::uint32_t y = 40; y < 43; ++y)
    for (std::uint32_t x = 40; x < 43; ++x) {
      f.px[y * 200 + x] = 5000;
      c.hot_pixels().set(static_cast<std::size_t>(y) * 200 + x);
    }
  // A trail: elongated streak.
  for (std::uint32_t x = 20; x < 60; ++x) {
    for (std::uint32_t y = 150; y < 153; ++y) f.px[y * 200 + x] = 3000;
  }
  std::vector<Spot> out(8);
  CentroidStats st;
  auto n = c.extract(f.view(), out, &st);
  ASSERT_TRUE(n.ok());
  ASSERT_EQ(n.value(), 1u);
  EXPECT_NEAR(out[0].x, 100.4, 0.05);
  EXPECT_GE(st.rejected, 1u);  // the trail
}

TEST_F(CentroiderTest, BlobOverflowIsReported) {
  CentroiderConfig cfg;
  cfg.max_width = 64;
  cfg.max_height = 64;
  cfg.max_blobs = 4;
  cfg.binary_open = false;
  cfg.min_area = 1;
  cfg.background = BackgroundMode::kNone;
  cfg.threshold = 10;
  auto c = make(cfg);
  Frame f{64, 64, std::vector<std::uint16_t>(64 * 64, 0)};
  for (std::uint32_t k = 0; k < 20; ++k) f.px[(3 * k % 64) * 64 + (5 * k + 3) % 64] = 100;
  std::vector<Spot> out(64);
  CentroidStats st;
  auto n = c.extract(f.view(), out, &st);
  ASSERT_TRUE(n.ok());
  EXPECT_TRUE(st.blob_overflow);
  EXPECT_LE(n.value(), 4u);
}

TEST_F(CentroiderTest, ModesAndInvalidInput) {
  for (auto bg :
       {BackgroundMode::kGlobalMean, BackgroundMode::kGlobalMedian, BackgroundMode::kNone}) {
    for (auto sm :
         {SigmaMode::kGlobalRootSquare, SigmaMode::kGlobalMedianAbs, SigmaMode::kLocalRootSquare}) {
      CentroiderConfig cfg;
      cfg.max_width = 128;
      cfg.max_height = 128;
      cfg.background = bg;
      cfg.sigma_mode = sm;
      // Local RMS includes the star's own flux, so it needs a lower sigma (as in tetra3).
      cfg.sigma =
          bg == BackgroundMode::kNone ? 50.0f : (sm == SigmaMode::kLocalRootSquare ? 2.0f : 5.0f);
      if (bg == BackgroundMode::kNone) cfg.threshold = 400.0f;
      cfg.filter_size = 15;
      auto c = make(cfg);
      const Frame f = make_frame(128, 128, {{64.2, 64.7, 20000}}, 1.2, 4);
      std::vector<Spot> out(4);
      auto n = c.extract(f.view(), out);
      ASSERT_TRUE(n.ok());
      ASSERT_GE(n.value(), 1u) << int(bg) << " " << int(sm);
      EXPECT_NEAR(out[0].x, 64.2, 0.2);  // mode coverage; low thresholds pull in noise pixels
    }
  }
  CentroiderConfig cfg;
  cfg.max_width = 32;
  cfg.max_height = 32;
  auto c = make(cfg);
  std::vector<Spot> out(4);
  std::vector<std::uint16_t> big(64 * 64);
  EXPECT_EQ(c.extract({big.data(), 64, 64, 0, PixelFormat::kMono16}, out).status(),
            Status::kInvalidArgument);
  EXPECT_EQ(c.extract({nullptr, 32, 32, 0, PixelFormat::kMono16}, out).status(),
            Status::kInvalidArgument);
  cfg.filter_size = 24;
  std::vector<std::byte> m(Centroider::workspace_bytes(cfg));
  Arena a(m);
  EXPECT_EQ(Centroider::create(cfg, a).status(), Status::kInvalidArgument);
}

TEST_F(CentroiderTest, RoiExtraction) {
  CentroiderConfig cfg;
  cfg.max_width = 300;
  cfg.max_height = 300;
  auto c = make(cfg);
  const Frame f = make_frame(300, 300, {{50.3, 40.8, 8000}, {220.6, 250.1, 12000}}, 1.1, 5);
  const Roi rois[3] = {{35, 25, 32, 32}, {205, 235, 32, 32}, {120, 120, 32, 32}};
  Spot out[3];
  bool found[3];
  std::uint64_t allocs = 0;
  {
    st::test::AllocGuard guard(st::test::AllocMode::kCount);
    ASSERT_EQ(c.extract_rois(f.view(), rois, RoiConfig{}, out, found), Status::kOk);
    allocs = guard.allocations();
  }
  EXPECT_EQ(allocs, 0u);
  ASSERT_TRUE(found[0]);
  ASSERT_TRUE(found[1]);
  EXPECT_FALSE(found[2]);
  EXPECT_NEAR(out[0].x, 50.3, 0.1);
  EXPECT_NEAR(out[0].y, 40.8, 0.1);
  EXPECT_NEAR(out[1].x, 220.6, 0.1);
}

TEST(NumpySum, MatchesPairwiseOnSmallInputs) {
  std::vector<float> v(20000);
  std::mt19937 g(1);
  std::uniform_real_distribution<float> u(0, 1);
  double ref = 0;
  for (auto& x : v) {
    x = u(g);
    ref += x;
  }
  EXPECT_NEAR(numpy_sum_f32(v), ref, ref * 1e-6);
  EXPECT_EQ(numpy_sum_f32({}), 0.0f);
  const float few[3] = {1, 2, 3};
  EXPECT_EQ(numpy_sum_f32(few), 6.0f);
}
}  // namespace
