// SPDX-License-Identifier: Apache-2.0
#pragma once
// Offline database generator (host tool). Follows ESA tetra3 generate_database (f9fa2eb),
// with documented divergences:
//   * stars are sorted by magnitude with a stable sort (numpy's argsort is unstable and
//     CPU-dependent for ties),
//   * patterns are deduplicated as unordered 4-sets (tetra3 deduplicates ordered tuples, so the
//     same 4 stars found at two FOV scales can occupy two slots),
//   * patterns are inserted in sorted order instead of Python set order (slot layout differs;
//     the set of patterns is the same).
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "db_writer.hpp"

namespace st::tools {

// One catalog entry in the catalog's native units:
//   hip_main: ra/dec in degrees, pm in mas/yr (pmRA = mu_alpha*cos(delta)), origin 1991.25
//   bsc5:     ra/dec in radians, pm in rad/yr (treated as mu_alpha*cos(delta) like tetra3),
//             origin 2000 (J2000 files) or 1950
struct CatalogStar {
  std::uint32_t id = 0;
  double ra = 0, dec = 0, mag = 0;
  double pm_ra = 0, pm_dec = 0;
  bool has_pm = false;
};

struct Catalog {
  db::CatalogKind kind = db::CatalogKind::kUnknown;
  double pm_origin = 0;
  double epoch_equinox = 2000;
  std::vector<CatalogStar> stars;
};

// CDS I/239 hip_main.dat. Entries missing magnitude or position are skipped (as tetra3 does).
Catalog read_hip_main(const std::string& path);
// Yale Bright Star Catalogue, binary "bsc5" format (as read by tetra3).
Catalog read_bsc5(const std::string& path);
// Dispatch on the file name (contains "bsc5" -> bsc5, else hip_main).
Catalog read_catalog(const std::string& path);

struct GeneratorConfig {
  double max_fov_deg = 0;
  double min_fov_deg = 0;  // 0 = same as max_fov_deg
  int pattern_stars_per_fov = 10;
  int verification_stars_per_fov = 30;
  double star_max_magnitude = 7;
  double pattern_max_error = 0.005;
  bool simplify_pattern = false;
  bool presort_patterns = true;
  bool save_largest_edge = false;
  double multiscale_step = 1.5;
  std::optional<double> epoch_proper_motion;  // nullopt: do not propagate (tetra3 'none')
  std::optional<std::pair<double, double>> range_ra_deg;
  std::optional<std::pair<double, double>> range_dec_deg;

  // cedar-solve lattice-field generation (variant; tetra3 behaviour when false). Uses cedar's
  // catalog rules (cos(dec) > 0.05 for proper motion, no range filters, all stars <= magnitude
  // kept for verification), Fibonacci lattice fields, breadth-first combinations, a prime-sized
  // table, 16-bit key hashes and largest edges. star_max_magnitude <= 0 selects cedar's
  // histogram-based limit.
  bool lattice = false;
  int lattice_field_oversampling = 100;
  int patterns_per_lattice_field = 50;
  bool linear_probe = false;
};

struct GeneratorReport {
  std::size_t catalog_stars = 0;
  std::size_t pattern_stars = 0;
  std::size_t verification_stars = 0;
  std::size_t patterns = 0;
  std::vector<double> pattern_fovs_deg;
};

DbContents generate_database_lattice(
    const Catalog& catalog, const GeneratorConfig& cfg, GeneratorReport* report = nullptr,
    std::vector<std::array<std::uint32_t, 4>>* pattern_sets_by_id = nullptr);

DbContents generate_database(
    const Catalog& catalog, const GeneratorConfig& cfg, GeneratorReport* report = nullptr,
    std::vector<std::array<std::uint32_t, 4>>* pattern_sets_by_id = nullptr);

}  // namespace st::tools
