// SPDX-License-Identifier: Apache-2.0
// st_centroid: prints the centroids of PGM images with tetra3's default settings.
//   st_centroid [--sigma S] [--max-area N] [--min-area N] [--no-open] <image.pgm>...
// Output per image: "image <path> <n>" then "y x sum area" lines (tetra3 (y, x) order).
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "pgm.hpp"

using namespace st;

int main(int argc, char** argv) {
  CentroiderConfig cfg;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--sigma")
      cfg.sigma = std::stof(argv[++i]);
    else if (a == "--max-area")
      cfg.max_area = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    else if (a == "--min-area")
      cfg.min_area = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    else if (a == "--no-open")
      cfg.binary_open = false;
    else
      files.push_back(a);
  }
  std::vector<std::byte> mem;
  std::vector<Spot> spots(100000);
  for (const auto& path : files) {
    const auto img = tools::read_pgm(path);
    cfg.max_width = img.width;
    cfg.max_height = img.height;
    mem.assign(Centroider::workspace_bytes(cfg), std::byte{0});
    Arena arena(mem);
    auto c = Centroider::create(cfg, arena);
    if (!c.ok()) {
      std::cerr << "centroider: " << to_string(c.status()) << "\n";
      return 1;
    }
    CentroidStats st;
    auto n = c.value().extract(img.view(), spots, &st);
    if (!n.ok()) {
      std::cerr << "extract: " << to_string(n.status()) << "\n";
      return 1;
    }
    std::printf("image %s %u\n", path.c_str(), n.value());
    for (std::uint32_t i = 0; i < n.value(); ++i) {
      std::printf("%.17g %.17g %.17g %u\n", spots[i].y, spots[i].x, spots[i].sum, spots[i].area);
    }
  }
  return 0;
}
