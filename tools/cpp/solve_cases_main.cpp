// SPDX-License-Identifier: Apache-2.0
// st_solve_cases: solves centroid lists from a text case file (used by the parity harness).
//
// Input (stdin or file), one case per block:
//   case <name>
//   size <width> <height>
//   opt <key> <value> ...     keys: fov_est fov_err (deg, "nan" = unset) pcs radius threshold
//                             dist_mode (none|fixed|range) dist dmin dmax variants (esa|cedar)
//   centroids <n>
//   <y> <x>                   n lines, tetra3 (y, x) order, brightest first
//   end
// Output per case:
//   result <name> <status> <ra> <dec> <roll> <fov_deg> <dist> <rmse> <matches> <prob> <us>
//   matched <catalog_id>... (in tetra3 match order)
//   pattern <i0> <i1> <i2> <i3>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "db_writer.hpp"
#include "st/core/clock.hpp"
#include "st/platform/mapped_file.hpp"
#include "st/solver/lost_in_space.hpp"

using namespace st;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: st_solve_cases <db.stdb> [cases.txt]\n";
    return 2;
  }
  auto file = MappedFile::open(argv[1]);
  if (!file.ok()) {
    std::cerr << "cannot map " << argv[1] << "\n";
    return 1;
  }
  auto db = Database::open(file.value().bytes());
  if (!db.ok()) {
    std::cerr << "db: " << to_string(db.status()) << "\n";
    return 1;
  }
  static std::vector<std::byte> arena_mem(64 << 20);
  Arena arena(arena_mem);
  SolverLimits limits;
  limits.max_centroids =
      std::max<std::uint32_t>(db.value().header().verification_stars_per_fov, 64);
  auto solver = LostInSpaceSolver::create(db.value(), limits, arena);
  if (!solver.ok()) {
    std::cerr << "solver: " << to_string(solver.status()) << "\n";
    return 1;
  }
  std::ifstream fin;
  std::istream* in = &std::cin;
  if (argc > 2) {
    fin.open(argv[2]);
    in = &fin;
  }
  SteadyClock clock;
  std::string tok;
  while (*in >> tok) {
    if (tok != "case") continue;
    std::string name;
    *in >> name;
    std::uint32_t w = 0, h = 0;
    SolveOptions opt;
    std::vector<Centroid> cents;
    while (*in >> tok && tok != "end") {
      if (tok == "size") {
        *in >> w >> h;
      } else if (tok == "opt") {
        std::string line;
        std::getline(*in, line);
        std::istringstream ss(line);
        std::string k, v;
        while (ss >> k >> v) {
          auto num = [&] { return v == "nan" ? std::nan("") : std::stod(v); };
          if (k == "fov_est")
            opt.fov_estimate = num() * kDegToRad;
          else if (k == "fov_err")
            opt.fov_max_error = num() * kDegToRad;
          else if (k == "pcs")
            opt.pattern_checking_stars = static_cast<std::uint32_t>(std::stoul(v));
          else if (k == "radius")
            opt.match_radius = num();
          else if (k == "threshold")
            opt.match_threshold = num();
          else if (k == "dist_mode")
            opt.distortion_mode =
                v == "none" ? DistortionMode::kNone
                            : (v == "range" ? DistortionMode::kRange : DistortionMode::kFixed);
          else if (k == "dist")
            opt.distortion = num();
          else if (k == "dmin")
            opt.distortion_min = num();
          else if (k == "dmax")
            opt.distortion_max = num();
          else if (k == "variants")
            opt.variants = v == "cedar" ? SolverVariants::cedar() : SolverVariants::esa();
        }
      } else if (tok == "centroids") {
        std::size_t n = 0;
        *in >> n;
        cents.resize(n);
        for (auto& c : cents) *in >> c.y >> c.x;
      }
    }
    opt.clock = &clock;
    SolveResult r;
    solver.value().solve(cents, w, h, opt, r);
    std::printf("result %s %s %.12f %.12f %.12f %.12f %.12g %.9g %u %.9g %.1f\n", name.c_str(),
                to_string(r.status), r.ra_deg, r.dec_deg, r.roll_deg, r.fov * kRadToDeg,
                r.distortion, r.rmse_arcsec, r.num_matches, r.probability,
                static_cast<double>(r.solve_ns) / 1000.0);
    std::printf("matched");
    for (const auto& m : r.matches) std::printf(" %u", db.value().catalog_id(m.star_index));
    std::printf("\npattern %u %u %u %u\n", r.pattern[0], r.pattern[1], r.pattern[2], r.pattern[3]);
  }
  return 0;
}
