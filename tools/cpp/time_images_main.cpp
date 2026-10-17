// SPDX-License-Identifier: Apache-2.0
// st_time_images: times the C++ library directly on PGM images (no Python, no C API layer).
//   st_time_images --db db.stdb [--reps N] <image.pgm>...
// Per image: centroid extraction (tetra3 defaults, 64 brightest kept) and one lost-in-space
// solve with default options, each the median of N runs. 8-bit PGMs are processed as mono8,
// like the Python bindings do. Output per image:
//   <path> <centroid_ms> <solve_ms> <status> <ra> <dec>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "pgm.hpp"
#include "st/platform/mapped_file.hpp"
#include "st/solver/lost_in_space.hpp"

using namespace st;

namespace {
double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}
double now_ms() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

int main(int argc, char** argv) {
  std::string dbpath;
  int reps = 5;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--db" && i + 1 < argc)
      dbpath = argv[++i];
    else if (a == "--reps" && i + 1 < argc)
      reps = std::max(1, std::stoi(argv[++i]));
    else
      files.push_back(a);
  }
  auto file = MappedFile::open(dbpath.c_str(), true);
  if (!file.ok()) {
    std::cerr << "cannot map " << dbpath << "\n";
    return 1;
  }
  auto db = Database::open(file.value().bytes());
  if (!db.ok()) return 1;
  std::vector<std::byte> mem(LostInSpaceSolver::workspace_bytes(db.value(), SolverLimits{}) + 4096);
  Arena arena(mem);
  auto lis = LostInSpaceSolver::create(db.value(), SolverLimits{}, arena);
  if (!lis.ok()) return 1;
  std::vector<Spot> spots(64);
  std::vector<Centroid> cents(64);
  std::vector<std::byte> cmem;
  for (const auto& path : files) {
    const auto img = tools::read_pgm(path);
    std::vector<std::uint8_t> px8;
    ImageView view = img.view();
    if (img.maxval <= 255) {
      px8.assign(img.pixels.begin(), img.pixels.end());
      view = ImageView{px8.data(), img.width, img.height, 0, PixelFormat::kMono8};
    }
    CentroiderConfig cc;
    cc.max_width = img.width;
    cc.max_height = img.height;
    cmem.assign(Centroider::workspace_bytes(cc), std::byte{0});
    Arena carena(cmem);
    auto cent = Centroider::create(cc, carena);
    if (!cent.ok()) return 1;
    std::vector<double> tc, ts;
    std::uint32_t n = 0;
    SolveResult res;
    // Like the Python side: all centroid runs back to back, then all solve runs back to back.
    for (int r = 0; r < reps; ++r) {
      const double t0 = now_ms();
      auto k = cent.value().extract(view, spots);
      tc.push_back(now_ms() - t0);
      n = k.ok() ? k.value() : 0;
    }
    for (std::uint32_t i = 0; i < n; ++i) cents[i] = {spots[i].x, spots[i].y};
    for (int r = 0; r < reps; ++r) {
      const double t0 = now_ms();
      lis.value().solve(std::span<const Centroid>(cents.data(), n), img.width, img.height,
                        SolveOptions{}, res);
      ts.push_back(now_ms() - t0);
    }
    std::printf("%s %.6f %.6f %s %.9f %.9f\n", path.c_str(), median(tc), median(ts),
                to_string(res.status), res.ra_deg, res.dec_deg);
  }
  return 0;
}
