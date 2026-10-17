// SPDX-License-Identifier: Apache-2.0
// st_sweep: synthetic star-field sweeps (FOV, sensitivity, read noise, rotation rate) through
// the full lost-in-space path (render -> centroider -> solver), scored against truth.
//   st_sweep --db esa_default.stdb [--trials N] [--out report.md] [--check]
// A solve counts as correct when the boresight error is below 60 arcsec. --check exits non-zero
// unless the nominal point solves >= 95% of trials with median boresight error < 20 arcsec.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "render.hpp"
#include "st/centroid/centroider.hpp"
#include "st/math/attitude.hpp"
#include "st/platform/mapped_file.hpp"
#include "st/solver/lost_in_space.hpp"

using namespace st;

namespace {

struct Point {
  std::string sweep, value;
  double fov_deg = 14, flux_scale = 1, read_noise = 5, rate_dps = 0;
};

struct Outcome {
  int trials = 0, solved = 0, correct = 0;
  std::vector<double> bore, roll, ms;
};

double pct(std::vector<double> v, double q) {
  if (v.empty()) return std::nan("");
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1,
                    static_cast<std::size_t>(q * static_cast<double>(v.size() - 1) + 0.5))];
}

}  // namespace

int main(int argc, char** argv) {
  std::string dbpath, out_md;
  int trials = 20;
  bool check = false;
  bool quick = false;  // nominal point only
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--db")
      dbpath = argv[++i];
    else if (a == "--trials")
      trials = std::stoi(argv[++i]);
    else if (a == "--out")
      out_md = argv[++i];
    else if (a == "--check")
      check = true;
    else if (a == "--quick")
      quick = true;
  }
  auto file = MappedFile::open(dbpath.c_str());
  if (!file.ok()) {
    std::cerr << "cannot map " << dbpath << " (fixture missing?); skipping\n";
    return 77;  // ctest SKIP_RETURN_CODE
  }
  auto db = Database::open(file.value().bytes());
  if (!db.ok()) return 2;
  auto stars = tools::stars_from_database(db.value());
  tools::add_random_stars(stars, 100000, 7.0, 9.0, 99);

  const std::uint32_t w = 1280, h = 960;
  CentroiderConfig ccfg;
  ccfg.max_width = w;
  ccfg.max_height = h;
  std::vector<std::byte> mem(Centroider::workspace_bytes(ccfg) + (8 << 20));
  Arena arena(mem);
  auto cent = Centroider::create(ccfg, arena);
  auto solver = LostInSpaceSolver::create(db.value(), SolverLimits{}, arena);
  if (!cent.ok() || !solver.ok()) return 2;
  std::vector<Spot> spots(256);
  std::vector<Centroid> cents(256);

  std::vector<Point> points;
  points.push_back({"nominal", "-"});
  if (!quick) {
    for (double f : {8.0, 11.0, 18.0, 24.0})
      points.push_back({"fov_deg", std::to_string(f).substr(0, 4), f});
    for (double s : {0.1, 0.3, 3.0, 10.0}) {
      Point p{"sensitivity", std::to_string(s).substr(0, 4)};
      p.flux_scale = s;
      points.push_back(p);
    }
    for (double n : {20.0, 50.0, 100.0}) {
      Point p{"read_noise_e", std::to_string(n).substr(0, 5)};
      p.read_noise = n;
      points.push_back(p);
    }
    for (double r : {0.2, 0.5, 1.0, 2.0}) {
      Point p{"rate_deg_s", std::to_string(r).substr(0, 4)};
      p.rate_dps = r;
      points.push_back(p);
    }
  }
  std::ostringstream md;
  md << "| sweep | value | trials | solved | correct | boresight median / p95 (\") | roll median "
        "(\") | time median / max (ms) |\n";
  md << "|---|---|---|---|---|---|---|---|\n";
  int rc = 0;
  SteadyClock clock;
  for (const auto& p : points) {
    Outcome o;
    std::mt19937_64 g(12345);
    std::uniform_real_distribution<double> u(0, 1);
    for (int t = 0; t < trials; ++t) {
      const double ra = 2 * kPi * u(g), dec = std::asin(2 * u(g) - 1), roll = 2 * kPi * u(g);
      tools::RenderConfig r;
      r.camera = CameraModel::ideal(w, h, p.fov_deg * kDegToRad);
      r.q_cam_icrs = attitude_from_radec_roll(ra, dec, roll);
      r.flux_mag0 *= p.flux_scale;
      r.read_noise_e = p.read_noise;
      r.body_rate = Vec3{0.3 * p.rate_dps * kDegToRad, p.rate_dps * kDegToRad, 0};
      r.seed = static_cast<std::uint64_t>(t) + 1;
      const auto img = tools::render(stars, r);
      const std::int64_t t0 = clock.now_ns();
      auto n = cent.value().extract(img.image.view(), spots);
      if (!n.ok()) continue;
      for (std::uint32_t i = 0; i < n.value(); ++i) cents[i] = {spots[i].x, spots[i].y};
      SolveResult res;
      solver.value().solve(std::span<const Centroid>(cents.data(), n.value()), w, h, SolveOptions{},
                           res);
      const double ms = static_cast<double>(clock.now_ns() - t0) / 1e6;
      ++o.trials;
      o.ms.push_back(ms);
      if (res.status != Status::kOk) continue;
      ++o.solved;
      // A smeared image shows the mid-exposure attitude.
      const Quat q_mid = from_rotation_vector(-0.5 * r.exposure_s * r.body_rate) * r.q_cam_icrs;
      double era = 0, edec = 0, eroll = 0;
      radec_roll_from_attitude(q_mid, era, edec, eroll);
      const Vec3 bt = radec_to_vector(era, edec),
                 bs = radec_to_vector(res.ra_deg * kDegToRad, res.dec_deg * kDegToRad);
      const double be = angle_between_unit(bt, bs) / kArcsecToRad;
      if (be > 60) continue;
      ++o.correct;
      o.bore.push_back(be);
      o.roll.push_back(std::fabs(std::remainder(res.roll_deg * kDegToRad - eroll, 2 * kPi)) /
                       kArcsecToRad);
    }
    char line[512];
    std::snprintf(line, sizeof(line),
                  "| %s | %s | %d | %d | %d | %.1f / %.1f | %.1f | %.1f / %.1f |\n",
                  p.sweep.c_str(), p.value.c_str(), o.trials, o.solved, o.correct, pct(o.bore, .5),
                  pct(o.bore, .95), pct(o.roll, .5), pct(o.ms, .5), pct(o.ms, 1.0));
    md << line;
    std::cout << line << std::flush;
    if (check && p.sweep == "nominal" &&
        (o.correct * 100 < 95 * o.trials || !(pct(o.bore, .5) < 20))) {
      std::cerr << "nominal point below threshold\n";
      rc = 1;
    }
  }
  if (!out_md.empty()) std::ofstream(out_md) << md.str();
  return rc;
}
