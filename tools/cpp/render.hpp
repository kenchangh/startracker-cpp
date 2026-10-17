// SPDX-License-Identifier: Apache-2.0
#pragma once
// Synthetic star-field renderer (host tool and tests). Deterministic for a given seed on every
// platform: it uses std::mt19937_64 with its own normal/Poisson samplers (the standard library
// distributions differ between implementations).
#include <cstdint>
#include <vector>

#include "pgm.hpp"
#include "st/camera/camera.hpp"
#include "st/db/database.hpp"
#include "st/math/vec.hpp"

namespace st::tools {

struct RenderStar {
  Vec3 v;  // ICRS unit vector
  double mag = 0;
  std::uint32_t id = 0;  // caller's identifier (e.g. database row)
};

struct RenderConfig {
  CameraIntrinsics camera;
  Quat q_cam_icrs;  // attitude at the start of the exposure
  Vec3 body_rate;   // camera angular rate, rad/s, in the camera frame
  double exposure_s = 0.1;
  int motion_steps = 0;  // 0: one step per 0.25 px of smear
  double psf_sigma_px = 1.0;
  double flux_mag0 = 2.0e7;   // electrons/s for a magnitude-0 star
  double background_e = 100;  // sky + dark electrons per pixel over the exposure
  double read_noise_e = 5;
  double gain_e_per_dn = 2;
  double bias_dn = 50;
  int bit_depth = 12;  // 8, 12 or 16
  bool shot_noise = true;
  std::uint64_t seed = 1;
  double hot_pixel_fraction = 0;
  double hot_pixel_dn = 3000;
};

struct RenderTruth {
  std::uint32_t id = 0;
  double x = 0, y = 0;  // mid-exposure position, pixels
  double mag = 0;
  double electrons = 0;
};

struct RenderResult {
  Image16 image;
  std::vector<RenderTruth> stars;  // stars whose mid-exposure position is in the frame
  std::vector<std::pair<std::uint32_t, std::uint32_t>> hot_pixels;  // (x, y)
};

RenderResult render(const std::vector<RenderStar>& stars, const RenderConfig& cfg);

// All stars of a database (vectors and magnitudes).
std::vector<RenderStar> stars_from_database(const Database& db);

// Adds `count` random faint stars (uniform on the sphere, magnitudes in [mag_lo, mag_hi]).
void add_random_stars(std::vector<RenderStar>& stars, std::size_t count, double mag_lo,
                      double mag_hi, std::uint64_t seed);

}  // namespace st::tools
