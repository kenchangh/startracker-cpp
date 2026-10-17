// SPDX-License-Identifier: Apache-2.0
// st_render: renders synthetic star fields from a database.
//   st_render --db db.stdb --out prefix [--count N] [--seed S] [--fov DEG] [--size WxH]
//             [--bits 8|12|16] [--read-noise E] [--background E] [--rate DEG/S] [--exposure S]
//             [--psf PX] [--k1 K] [--clutter N]
// Writes prefix_###.pgm, prefix_###.fov (horizontal FOV, deg) and prefix_###.truth
// (ra dec roll in deg, then "id x y mag" per star).
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <string>

#include "render.hpp"
#include "st/math/attitude.hpp"
#include "st/platform/mapped_file.hpp"

using namespace st;

int main(int argc, char** argv) {
  std::string dbpath, prefix;
  int count = 10;
  std::uint64_t seed = 1;
  double fov = 12, rate = 0, k1 = 0;
  std::uint32_t w = 1280, h = 960;
  std::size_t clutter = 0;
  tools::RenderConfig rc;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&] { return std::string(argv[++i]); };
    if (a == "--db")
      dbpath = next();
    else if (a == "--out")
      prefix = next();
    else if (a == "--count")
      count = std::stoi(next());
    else if (a == "--seed")
      seed = std::stoull(next());
    else if (a == "--fov")
      fov = std::stod(next());
    else if (a == "--size") {
      const auto s = next();
      w = static_cast<std::uint32_t>(std::stoul(s));
      h = static_cast<std::uint32_t>(std::stoul(s.substr(s.find('x') + 1)));
    } else if (a == "--bits")
      rc.bit_depth = std::stoi(next());
    else if (a == "--read-noise")
      rc.read_noise_e = std::stod(next());
    else if (a == "--background")
      rc.background_e = std::stod(next());
    else if (a == "--rate")
      rate = std::stod(next());
    else if (a == "--exposure")
      rc.exposure_s = std::stod(next());
    else if (a == "--psf")
      rc.psf_sigma_px = std::stod(next());
    else if (a == "--k1")
      k1 = std::stod(next());
    else if (a == "--clutter")
      clutter = std::stoul(next());
    else {
      std::cerr << "unknown option " << a << "\n";
      return 2;
    }
  }
  auto f = MappedFile::open(dbpath.c_str());
  if (!f.ok()) {
    std::cerr << "cannot map " << dbpath << "\n";
    return 1;
  }
  auto db = Database::open(f.value().bytes());
  if (!db.ok()) {
    std::cerr << "db: " << to_string(db.status()) << "\n";
    return 1;
  }
  auto stars = tools::stars_from_database(db.value());
  if (clutter > 0) tools::add_random_stars(stars, clutter, 7.0, 9.0, seed ^ 0x5EED);
  std::mt19937_64 g(seed);
  std::uniform_real_distribution<double> u(0, 1);
  for (int k = 0; k < count; ++k) {
    const double ra = 2 * kPi * u(g), dec = std::asin(2 * u(g) - 1), roll = 2 * kPi * u(g);
    rc.camera = CameraModel::ideal(w, h, fov * kDegToRad);
    rc.camera.k1 = k1;
    rc.q_cam_icrs = attitude_from_radec_roll(ra, dec, roll);
    rc.body_rate = Vec3{0, rate * kDegToRad, 0};
    rc.seed = seed * 1000 + static_cast<std::uint64_t>(k);
    const auto r = tools::render(stars, rc);
    char name[512];
    std::snprintf(name, sizeof(name), "%s_%03d", prefix.c_str(), k);
    tools::write_pgm(std::string(name) + ".pgm", r.image);
    std::ofstream(std::string(name) + ".fov") << fov << "\n";
    std::ofstream t(std::string(name) + ".truth");
    t << ra * kRadToDeg << " " << dec * kRadToDeg << " " << roll * kRadToDeg << "\n";
    for (const auto& s : r.stars) t << s.id << " " << s.x << " " << s.y << " " << s.mag << "\n";
  }
  return 0;
}
