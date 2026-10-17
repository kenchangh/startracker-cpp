// SPDX-License-Identifier: Apache-2.0
// Checks on the converted ESA tetra3 default database (fixture).
#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>
#include "st/db/database.hpp"
#include "st/platform/mapped_file.hpp"
#include "support/fixtures.hpp"

namespace {
using namespace st;

TEST(EsaDatabase, HeaderAndKeyLookups) {
  ST_REQUIRE_FIXTURE(path, "esa_default.stdb");
  auto f = MappedFile::open(path.c_str());
  ASSERT_TRUE(f.ok());
  auto d = Database::open(f.value().bytes());
  ASSERT_TRUE(d.ok()) << to_string(d.status());
  const Database& db = d.value();
  EXPECT_EQ(db.num_stars(), 8818u);
  EXPECT_EQ(db.num_slots(), 12369092u);
  EXPECT_EQ(db.header().pattern_bins, 50u);
  EXPECT_EQ(db.header().threshold_patterns, 6184546u);
  EXPECT_EQ(db.header().catalog, db::CatalogKind::kHipMain);
  EXPECT_NEAR(db.header().max_fov, 30 * 3.14159265358979 / 180, 1e-9);
  EXPECT_TRUE(db.has_catalog_ids());
  // Recomputing each pattern's key with tetra3's formula must find its slot: proves the hash,
  // magic multiplier and quadratic probing match ESA's.
  std::array<std::uint32_t, 4> p{};
  std::size_t checked = 0, found_all = 0;
  for (std::uint64_t s = 0; s < db.num_slots(); s += 997) {
    if (!db.pattern(s, p)) continue;
    double e[6];
    int k = 0;
    for (int i = 0; i < 4; ++i)
      for (int j = i + 1; j < 4; ++j) {
        const auto a = db.star_vector(p[i]), b = db.star_vector(p[j]);
        double s2 = 0;
        for (int c = 0; c < 3; ++c) {
          const double x = static_cast<double>(a[c]) - b[c];
          s2 += x * x;
        }
        e[k++] = 2 * std::asin(0.5 * std::sqrt(s2));
      }
    std::sort(e, e + 6);
    std::uint32_t key[5];
    for (int i = 0; i < 5; ++i) key[i] = static_cast<std::uint32_t>(e[i] / e[5] * 50);
    const std::uint64_t start = db.start_slot(pattern_key_hash(key, 50));
    for (std::uint32_t c = 0; c <= db.max_probe_chain(); ++c) {
      if (db.probe_slot(start, c) == s) {
        ++found_all;
        break;
      }
    }
    ++checked;
  }
  EXPECT_GT(checked, 5000u);
  EXPECT_EQ(found_all, checked);
}
}  // namespace
