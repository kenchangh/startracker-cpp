// SPDX-License-Identifier: Apache-2.0
#pragma once
// Shared synthetic sky for tests: a random 9000-star catalog and a 15-20 degree database built
// once per test binary.
#include <cmath>
#include <cstdlib>
#include <random>
#include <vector>

#include "db_writer.hpp"
#include "generator.hpp"
#include "st/db/database.hpp"
#include "st/math/vec.hpp"

namespace st::test {

struct SyntheticDb {
  std::vector<std::byte> blob;
  Database db;
};

inline SyntheticDb& synthetic_db() {
  static SyntheticDb* s = [] {
    auto* d = new SyntheticDb;
    std::mt19937_64 g(42);
    std::uniform_real_distribution<double> u(0, 1);
    tools::Catalog cat;
    cat.kind = db::CatalogKind::kHipMain;
    cat.pm_origin = 1991.25;
    for (int i = 0; i < 9000; ++i) {
      tools::CatalogStar s;
      s.id = static_cast<std::uint32_t>(i + 1);
      s.ra = 360 * u(g);
      s.dec = std::asin(2 * u(g) - 1) * kRadToDeg;
      s.mag = 1 + 6 * std::pow(u(g), 0.3);
      cat.stars.push_back(s);
    }
    tools::GeneratorConfig cfg;
    cfg.max_fov_deg = 20;
    cfg.min_fov_deg = 15;
    cfg.star_max_magnitude = 7;
    d->blob = tools::build_blob(tools::generate_database(cat, cfg));
    auto r = Database::open(d->blob);
    if (!r.ok()) std::abort();
    d->db = r.value();
    return d;
  }();
  return *s;
}

}  // namespace st::test
