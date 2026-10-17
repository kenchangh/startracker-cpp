// SPDX-License-Identifier: Apache-2.0
#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace st::tools {
namespace {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : g_(seed) {}
  double uniform() { return (static_cast<double>(g_() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
  double normal() {
    if (have_) {
      have_ = false;
      return spare_;
    }
    const double u1 = uniform(), u2 = uniform();
    const double r = std::sqrt(-2.0 * std::log(u1));
    spare_ = r * std::sin(2 * kPi * u2);
    have_ = true;
    return r * std::cos(2 * kPi * u2);
  }
  double poisson(double lambda) {
    if (lambda <= 0) return 0;
    if (lambda > 64) return std::max(0.0, std::round(lambda + std::sqrt(lambda) * normal()));
    const double l = std::exp(-lambda);
    double k = 0, p = 1;
    do {
      ++k;
      p *= uniform();
    } while (p > l);
    return k - 1;
  }
  std::uint64_t next() { return g_(); }

 private:
  std::mt19937_64 g_;
  bool have_ = false;
  double spare_ = 0;
};

double pixel_fraction(double lo, double hi, double c, double sigma) {
  const double s = 1.0 / (std::sqrt(2.0) * sigma);
  return 0.5 * (std::erf((hi - c) * s) - std::erf((lo - c) * s));
}

}  // namespace

RenderResult render(const std::vector<RenderStar>& stars, const RenderConfig& cfg) {
  auto cam_r = CameraModel::create(cfg.camera);
  if (!cam_r.ok()) throw std::runtime_error("render: bad camera");
  const CameraModel& cam = cam_r.value();
  const std::uint32_t w = cfg.camera.width, h = cfg.camera.height;
  std::vector<double> e(static_cast<std::size_t>(w) * h, cfg.background_e);
  RenderResult res;
  const double rate = norm(cfg.body_rate);
  const double smear_px = rate * cfg.exposure_s * cfg.camera.fx;
  const int steps = cfg.motion_steps > 0
                        ? cfg.motion_steps
                        : std::max(1, static_cast<int>(std::ceil(smear_px / 0.25)));
  const double margin = 5 * cfg.psf_sigma_px + 2;
  for (const auto& s : stars) {
    const double electrons = cfg.flux_mag0 * std::pow(10.0, -0.4 * s.mag) * cfg.exposure_s;
    // Mid-exposure position for the truth list.
    const Quat qmid = from_rotation_vector(-0.5 * cfg.exposure_s * cfg.body_rate) * cfg.q_cam_icrs;
    double mx = 0, my = 0;
    const bool mid_ok = cam.ray_to_pixel(rotate(qmid, s.v), mx, my);
    bool any = false;
    for (int k = 0; k < steps; ++k) {
      const double t = (k + 0.5) / steps * cfg.exposure_s;
      const Quat q = from_rotation_vector(-t * cfg.body_rate) * cfg.q_cam_icrs;
      double cx = 0, cy = 0;
      if (!cam.ray_to_pixel(rotate(q, s.v), cx, cy)) continue;
      if (cx < -margin || cy < -margin || cx > w + margin || cy > h + margin) continue;
      any = true;
      const double share = electrons / steps;
      const long x0 = std::max<long>(0, static_cast<long>(std::floor(cx - margin)));
      const long x1 = std::min<long>(w - 1, static_cast<long>(std::ceil(cx + margin)));
      const long y0 = std::max<long>(0, static_cast<long>(std::floor(cy - margin)));
      const long y1 = std::min<long>(h - 1, static_cast<long>(std::ceil(cy + margin)));
      for (long y = y0; y <= y1; ++y) {
        const double fy = pixel_fraction(static_cast<double>(y), static_cast<double>(y + 1), cy,
                                         cfg.psf_sigma_px);
        for (long x = x0; x <= x1; ++x) {
          e[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] +=
              share * fy *
              pixel_fraction(static_cast<double>(x), static_cast<double>(x + 1), cx,
                             cfg.psf_sigma_px);
        }
      }
    }
    if (any && mid_ok && mx >= 0 && my >= 0 && mx < w && my < h) {
      res.stars.push_back({s.id, mx, my, s.mag, electrons});
    }
  }
  Rng rng(cfg.seed);
  const std::uint32_t maxval = cfg.bit_depth >= 16 ? 65535u : (1u << cfg.bit_depth) - 1u;
  res.image.width = w;
  res.image.height = h;
  res.image.maxval = cfg.bit_depth <= 8 ? 255u : 65535u;
  res.image.pixels.resize(e.size());
  for (std::size_t i = 0; i < e.size(); ++i) {
    double v = cfg.shot_noise ? rng.poisson(e[i]) : e[i];
    v += cfg.read_noise_e * rng.normal();
    double dn = std::round(v / cfg.gain_e_per_dn + cfg.bias_dn);
    dn = std::clamp(dn, 0.0, static_cast<double>(maxval));
    res.image.pixels[i] = static_cast<std::uint16_t>(dn);
  }
  if (cfg.hot_pixel_fraction > 0) {
    const auto nhot =
        static_cast<std::size_t>(cfg.hot_pixel_fraction * static_cast<double>(e.size()));
    for (std::size_t k = 0; k < nhot; ++k) {
      const auto x = static_cast<std::uint32_t>(rng.next() % w);
      const auto y = static_cast<std::uint32_t>(rng.next() % h);
      res.image.pixels[static_cast<std::size_t>(y) * w + x] =
          static_cast<std::uint16_t>(std::min<double>(maxval, cfg.hot_pixel_dn));
      res.hot_pixels.emplace_back(x, y);
    }
  }
  return res;
}

std::vector<RenderStar> stars_from_database(const Database& db) {
  std::vector<RenderStar> out;
  out.reserve(db.num_stars());
  for (std::uint32_t i = 0; i < db.num_stars(); ++i) {
    const auto f = db.star_vector(i);
    out.push_back({Vec3{f[0], f[1], f[2]}, db.star_magnitude(i), i});
  }
  return out;
}

void add_random_stars(std::vector<RenderStar>& stars, std::size_t count, double mag_lo,
                      double mag_hi, std::uint64_t seed) {
  Rng rng(seed);
  for (std::size_t i = 0; i < count; ++i) {
    const double z = 2 * rng.uniform() - 1, phi = 2 * kPi * rng.uniform();
    const double r = std::sqrt(1 - z * z);
    stars.push_back({Vec3{r * std::cos(phi), r * std::sin(phi), z},
                     mag_lo + (mag_hi - mag_lo) * rng.uniform(), 0xFFFFFFFFu});
  }
}

}  // namespace st::tools
