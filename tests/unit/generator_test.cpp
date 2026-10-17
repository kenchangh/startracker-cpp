// SPDX-License-Identifier: Apache-2.0
// Invariants of generated databases on a synthetic catalog.
#include <algorithm>
#include <cmath>
#include <random>

#include <gtest/gtest.h>
#include "generator.hpp"
#include "st/db/database.hpp"
#include "st/math/vec.hpp"

namespace {
using namespace st;

tools::Catalog random_catalog(int n, unsigned seed) {
  std::mt19937_64 g(seed);
  std::uniform_real_distribution<double> u(0, 1);
  tools::Catalog c;
  c.kind = db::CatalogKind::kHipMain;
  c.pm_origin = 1991.25;
  for (int i = 0; i < n; ++i) {
    tools::CatalogStar s;
    s.id = static_cast<std::uint32_t>(i + 1);
    s.ra = 360 * u(g);
    s.dec = std::asin(2 * u(g) - 1) * kRadToDeg;
    s.mag = 1 + 6 * u(g);
    s.pm_ra = 50 * (u(g) - 0.5);
    s.pm_dec = 50 * (u(g) - 0.5);
    s.has_pm = true;
    c.stars.push_back(s);
  }
  return c;
}

// Key of a pattern as the generator computes it.
std::uint64_t key_hash_of(const Database& d, const std::array<std::uint32_t, 4>& p,
                          double& largest) {
  double e[6];
  int k = 0;
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j) {
      const auto a = d.star_vector(p[i]);
      const auto b = d.star_vector(p[j]);
      double s2 = 0;
      for (int c = 0; c < 3; ++c) {
        const double x = static_cast<double>(a[c]) - b[c];
        s2 += x * x;
      }
      e[k++] = 2 * std::asin(0.5 * std::sqrt(s2));
    }
  std::sort(e, e + 6);
  largest = e[5];
  std::uint32_t key[5];
  for (int i = 0; i < 5; ++i)
    key[i] = static_cast<std::uint32_t>(e[i] / e[5] * d.header().pattern_bins);
  return pattern_key_hash(key, d.header().pattern_bins);
}

TEST(Generator, InvariantsOnSyntheticCatalog) {
  tools::GeneratorConfig cfg;
  cfg.max_fov_deg = 30;
  cfg.min_fov_deg = 20;
  cfg.star_max_magnitude = 6;
  cfg.epoch_proper_motion = 2025.0;
  cfg.save_largest_edge = true;
  tools::GeneratorReport rep;
  auto blob = tools::build_blob(tools::generate_database(random_catalog(4000, 7), cfg, &rep));
  auto d = Database::open(blob);
  ASSERT_TRUE(d.ok());
  const Database& db = d.value();
  ASSERT_EQ(rep.pattern_fovs_deg.size(), 2u);  // ratio 1.5 -> 2 scales
  EXPECT_GT(rep.patterns, 1000u);
  EXPECT_EQ(db.header().num_patterns, rep.patterns);
  EXPECT_EQ(db.num_slots(), 2 * rep.patterns);
  EXPECT_TRUE(db.has_largest_edge());
  // Stars sorted by magnitude.
  for (std::uint32_t i = 1; i < db.num_stars(); ++i)
    EXPECT_LE(db.star_magnitude(i - 1), db.star_magnitude(i));
  std::size_t checked = 0;
  std::array<std::uint32_t, 4> p{};
  for (std::uint64_t s = 0; s < db.num_slots(); s += 7) {
    if (!db.pattern(s, p)) continue;
    // Every pattern fits in max_fov.
    for (int i = 0; i < 4; ++i)
      for (int j = i + 1; j < 4; ++j) {
        const auto a = db.star_vector(p[i]), b = db.star_vector(p[j]);
        const float dotp = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        EXPECT_GT(static_cast<double>(dotp), std::cos(db.header().max_fov));
      }
    // The slot is reachable from the pattern's own key within the probe bound.
    double largest = 0;
    const std::uint64_t start = db.start_slot(key_hash_of(db, p, largest));
    bool found = false;
    for (std::uint32_t c = 0; c <= db.max_probe_chain() && !found; ++c)
      found = db.probe_slot(start, c) == s;
    EXPECT_TRUE(found) << "slot " << s;
    EXPECT_NEAR(db.largest_edge_mrad(s), largest * 1000, largest * 1000 * 1e-3);
    // Presorted: radii from the centroid are non-decreasing.
    double cen[3] = {0, 0, 0};
    for (int i = 0; i < 4; ++i) {
      const auto v = db.star_vector(p[i]);
      for (int c = 0; c < 3; ++c) cen[c] += v[c] / 4.0;
    }
    double prev = -1;
    for (int i = 0; i < 4; ++i) {
      const auto v = db.star_vector(p[i]);
      const double r = std::hypot(v[0] - cen[0], v[1] - cen[1], v[2] - cen[2]);
      EXPECT_GE(r, prev - 1e-6);
      prev = r;
    }
    ++checked;
  }
  EXPECT_GT(checked, 100u);
}

TEST(Generator, SingleScaleAndRanges) {
  tools::GeneratorConfig cfg;
  cfg.max_fov_deg = 25;
  cfg.star_max_magnitude = 6;
  cfg.range_dec_deg = std::make_pair(-30.0, 30.0);
  tools::GeneratorReport rep;
  auto contents = tools::generate_database(random_catalog(3000, 3), cfg, &rep);
  ASSERT_EQ(rep.pattern_fovs_deg.size(), 1u);
  for (std::size_t i = 0; i < contents.star_table.size(); i += 6) {
    EXPECT_GT(contents.star_table[i + 1], -30 * kDegToRad - 1e-6);
    EXPECT_LT(contents.star_table[i + 1], 30 * kDegToRad + 1e-6);
  }
  EXPECT_EQ(contents.header.epoch_proper_motion, 1991.25);  // no propagation by default
}
}  // namespace

namespace {
TEST(Generator, LatticeVariantInvariants) {
  tools::GeneratorConfig cfg;
  cfg.max_fov_deg = 30;
  cfg.min_fov_deg = 20;
  cfg.star_max_magnitude = 6;
  cfg.lattice = true;
  cfg.linear_probe = true;
  cfg.verification_stars_per_fov = 50;
  cfg.lattice_field_oversampling = 10;
  cfg.patterns_per_lattice_field = 20;
  cfg.pattern_max_error = 0.001;
  tools::GeneratorReport rep;
  auto blob = tools::build_blob(tools::generate_database(random_catalog(3000, 11), cfg, &rep));
  auto d = Database::open(blob);
  ASSERT_TRUE(d.ok());
  const Database& db = d.value();
  EXPECT_EQ(db.header().probe_mode, db::ProbeMode::kLinear);
  EXPECT_TRUE(db.has_key_hash());
  EXPECT_TRUE(db.has_largest_edge());
  EXPECT_EQ(db.header().pattern_bins, 250u);
  EXPECT_EQ(db.header().threshold_patterns, rep.patterns);
  EXPECT_EQ(db.num_stars(), rep.catalog_stars);  // no verification thinning in this variant
  EXPECT_GE(db.num_slots(), 3 * rep.patterns);   // next prime above 3N
  std::array<std::uint32_t, 4> p{};
  std::size_t checked = 0;
  for (std::uint64_t s = 0; s < db.num_slots(); s += 5) {
    if (!db.pattern(s, p)) continue;
    double largest = 0;
    const std::uint64_t kh = key_hash_of(db, p, largest);
    EXPECT_EQ(db.key_hash(s), static_cast<std::uint16_t>(kh & 0xFFFF));
    const std::uint64_t start = db.start_slot(kh);
    bool found = false;
    for (std::uint32_t c = 0; c <= db.max_probe_chain() && !found; ++c)
      found = db.probe_slot(start, c) == s;
    EXPECT_TRUE(found);
    ++checked;
  }
  EXPECT_GT(checked, 100u);
}
}  // namespace
