// SPDX-License-Identifier: Apache-2.0
#include "generator.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "st/db/database.hpp"

namespace st::tools {
namespace {

constexpr double kPi = 3.14159265358979323846;
double deg2rad(double d) { return d * kPi / 180.0; }

bool blank(const std::string& s) {
  return !s.empty() &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
}

using V3f = std::array<float, 3>;

// Uniform grid over the unit sphere for fixed-radius neighbour queries (stands in for scipy's
// KDTree.query_ball_point; returns the same set: squared distance <= r^2 in double).
class Grid {
 public:
  Grid(const std::vector<V3f>& pts, const std::vector<std::uint32_t>& idx, double r)
      : pts_(pts), cell_(r > 0 ? r : 1.0) {
    for (std::uint32_t i : idx) cells_[key(pts[i])].push_back(i);
  }
  template <class F>
  void query(const V3f& q, double r, F&& f) const {
    const auto c = coords(q);
    const double r2 = r * r;
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz) {
          auto it = cells_.find(pack(c[0] + dx, c[1] + dy, c[2] + dz));
          if (it == cells_.end()) continue;
          for (std::uint32_t i : it->second) {
            const double ex = static_cast<double>(pts_[i][0]) - q[0];
            const double ey = static_cast<double>(pts_[i][1]) - q[1];
            const double ez = static_cast<double>(pts_[i][2]) - q[2];
            if (ex * ex + ey * ey + ez * ez <= r2) f(i);
          }
        }
  }

 private:
  std::array<long, 3> coords(const V3f& p) const {
    return {static_cast<long>(std::floor((p[0] + 1.0) / cell_)),
            static_cast<long>(std::floor((p[1] + 1.0) / cell_)),
            static_cast<long>(std::floor((p[2] + 1.0) / cell_))};
  }
  static std::uint64_t pack(long x, long y, long z) {
    return (static_cast<std::uint64_t>(x + 1) << 42) ^ (static_cast<std::uint64_t>(y + 1) << 21) ^
           static_cast<std::uint64_t>(z + 1);
  }
  std::uint64_t key(const V3f& p) const {
    const auto c = coords(p);
    return pack(c[0], c[1], c[2]);
  }
  const std::vector<V3f>& pts_;
  double cell_;
  std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells_;
};

// Greedy separation thinning in index (brightness) order, as tetra3 does with its KD-tree.
void thin(const std::vector<V3f>& vec, double sep, std::size_t first, std::vector<char>& keep) {
  std::vector<std::uint32_t> all(vec.size());
  std::iota(all.begin(), all.end(), 0u);
  Grid g(vec, all, sep);
  for (std::size_t i = first; i < vec.size(); ++i) {
    bool occupied = false;
    g.query(vec[i], sep, [&](std::uint32_t j) { occupied = occupied || keep[j]; });
    if (!occupied) keep[i] = 1;
  }
}

}  // namespace

Catalog read_hip_main(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open " + path);
  Catalog cat;
  cat.kind = db::CatalogKind::kHipMain;
  cat.pm_origin = 1991.25;
  cat.epoch_equinox = 2000;
  auto& out = cat.stars;
  std::string line;
  while (std::getline(f, line)) {
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string tok;
    while (std::getline(ss, tok, '|')) fields.push_back(tok);
    if (fields.size() < 14) continue;
    if (blank(fields[5]) || blank(fields[8]) || blank(fields[9])) continue;
    CatalogStar s;
    s.id = static_cast<std::uint32_t>(std::stoul(fields[1]));
    s.mag = std::stod(fields[5]);
    s.ra = std::stod(fields[8]);
    s.dec = std::stod(fields[9]);
    s.has_pm = !blank(fields[12]) && !blank(fields[13]);
    if (s.has_pm) {
      s.pm_ra = std::stod(fields[12]);
      s.pm_dec = std::stod(fields[13]);
    }
    out.push_back(s);
  }
  return cat;
}

Catalog read_bsc5(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::int32_t hdr[7];
  f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
  if (!f) throw std::runtime_error("bsc5: short header");
  Catalog cat;
  cat.kind = db::CatalogKind::kBsc5;
  std::int32_t n = hdr[2];
  if (n > 0) {
    cat.epoch_equinox = 1950;
    cat.pm_origin = 1950;
  } else {
    n = -n;
    cat.epoch_equinox = 2000;
    cat.pm_origin = 2000;
  }
  if (hdr[6] != 32) throw std::runtime_error("bsc5: unexpected entry size");
  for (std::int32_t i = 0; i < n; ++i) {
    unsigned char e[32];
    f.read(reinterpret_cast<char*>(e), 32);
    if (!f) throw std::runtime_error("bsc5: truncated");
    float id;
    double ra, dec;
    std::int16_t mag;
    float pmra, pmdec;
    std::memcpy(&id, e, 4);
    std::memcpy(&ra, e + 4, 8);
    std::memcpy(&dec, e + 12, 8);
    std::memcpy(&mag, e + 22, 2);
    std::memcpy(&pmra, e + 24, 4);
    std::memcpy(&pmdec, e + 28, 4);
    CatalogStar s;
    s.id = static_cast<std::uint16_t>(id);  // tetra3 stores np.uint16(ID)
    s.ra = ra;
    s.dec = dec;
    s.mag = mag / 100.0;
    s.pm_ra = pmra;
    s.pm_dec = pmdec;
    s.has_pm = true;
    cat.stars.push_back(s);
  }
  return cat;
}

Catalog read_catalog(const std::string& path) {
  if (path.find("bsc5") != std::string::npos) return read_bsc5(path);
  return read_hip_main(path);
}

DbContents generate_database(const Catalog& catalog, const GeneratorConfig& cfg,
                             GeneratorReport* report,
                             std::vector<std::array<std::uint32_t, 4>>* sets_by_id) {
  if (!(cfg.max_fov_deg > 0)) throw std::runtime_error("max_fov must be > 0");
  const double max_fov = deg2rad(cfg.max_fov_deg);
  const double min_fov = cfg.min_fov_deg > 0 ? deg2rad(cfg.min_fov_deg) : max_fov;
  const int bins = static_cast<int>(std::nearbyint(1.0 / 4.0 / cfg.pattern_max_error));

  // 1. Load, propagate proper motion, magnitude cut (tetra3 l.772-864).
  struct Row {
    float ra, dec, mag;
    std::uint32_t id;
  };
  std::vector<Row> rows;
  const double pm_origin = catalog.pm_origin;
  const double epoch = cfg.epoch_proper_motion.value_or(pm_origin);
  const bool propagate = epoch != pm_origin;
  for (const auto& s : catalog.stars) {
    double ra = 0, dec = 0;
    if (catalog.kind == db::CatalogKind::kBsc5) {
      if (s.mag > cfg.star_max_magnitude) continue;
      const double cos_d = std::cos(s.dec);
      double mu_a = 0, mu_d = 0;
      if (cos_d > 0.1) {
        mu_a = s.pm_ra / cos_d;
        mu_d = s.pm_dec;
      }
      ra = s.ra + mu_a * (epoch - pm_origin);
      dec = s.dec + mu_d * (epoch - pm_origin);
    } else {
      if (propagate && !s.has_pm) continue;
      if (s.mag > cfg.star_max_magnitude) continue;
      double mu_a = 0, mu_d = 0;
      if (propagate) {
        const double cos_d = std::cos(deg2rad(s.dec));
        if (cos_d > 0.1) {
          mu_a = s.pm_ra / 1000 / 60 / 60 / cos_d;
          mu_d = s.pm_dec / 1000 / 60 / 60;
        }
      }
      ra = deg2rad(s.ra + mu_a * (epoch - pm_origin));
      dec = deg2rad(s.dec + mu_d * (epoch - pm_origin));
    }
    Row r{static_cast<float>(ra), static_cast<float>(dec), static_cast<float>(s.mag), s.id};
    if (r.ra == 0.0f && r.dec == 0.0f) continue;  // tetra3 drops rows with ra == dec == 0
    rows.push_back(r);
  }
  std::stable_sort(rows.begin(), rows.end(),
                   [](const Row& a, const Row& b) { return a.mag < b.mag; });
  auto clip = [&](const std::optional<std::pair<double, double>>& range, bool is_ra) {
    if (!range) return;
    const double lo = deg2rad(range->first), hi = deg2rad(range->second);
    std::vector<Row> kept;
    for (const auto& r : rows) {
      const double v = is_ra ? r.ra : r.dec;
      const bool in = lo < hi ? (v > lo && v < hi) : (v > lo || v < hi);
      if (in) kept.push_back(r);
    }
    rows.swap(kept);
  };
  clip(cfg.range_ra_deg, true);
  clip(cfg.range_dec_deg, false);
  const std::size_t n = rows.size();
  if (n < 4) throw std::runtime_error("too few stars after filtering");
  std::vector<V3f> vec(n);
  for (std::size_t i = 0; i < n; ++i) {  // float32 trig, as numpy does on float32 scalars
    const float ra = rows[i].ra, dec = rows[i].dec;
    vec[i] = {std::cos(ra) * std::cos(dec), std::sin(ra) * std::cos(dec), std::sin(dec)};
  }

  // 2. FOV scales (l.933-941).
  const double ratio = max_fov / min_fov;
  const int divisions =
      static_cast<int>(std::ceil(std::log(ratio) / std::log(cfg.multiscale_step))) + 1;
  std::vector<double> fovs;
  if (ratio < std::sqrt(cfg.multiscale_step)) {
    fovs = {max_fov};
  } else {
    const double a = std::log2(min_fov), b = std::log2(max_fov);
    const double step = (b - a) / (divisions - 1);
    for (int i = 0; i < divisions; ++i) fovs.push_back(std::exp2(i * step + a));
    fovs.back() = std::exp2(b);
  }

  // 3. Patterns per scale (l.947-1011).
  std::vector<char> keep_for_patterns(n, 0);
  keep_for_patterns[0] = 1;
  std::vector<std::array<std::uint32_t, 4>> patterns;  // indices into rows, sorted 4-sets
  for (auto it = fovs.rbegin(); it != fovs.rend(); ++it) {
    const double fov = *it;
    std::vector<char> keep_at_fov(n, 0);
    const double sep = divisions == 1 ? 0.6 * min_fov / std::sqrt(cfg.pattern_stars_per_fov)
                                      : 0.6 * fov / std::sqrt(cfg.pattern_stars_per_fov);
    thin(vec, sep, 0, keep_at_fov);
    std::vector<std::uint32_t> pidx;
    for (std::uint32_t i = 0; i < n; ++i) {
      if (keep_at_fov[i]) {
        keep_for_patterns[i] = 1;
        pidx.push_back(i);
      }
    }
    const double radius = cfg.simplify_pattern ? fov / 2 : fov;
    Grid g(vec, pidx, radius);
    std::vector<char> available(n, 1);
    const double cos_fov = std::cos(fov);
    std::vector<std::uint32_t> nb;
    for (std::uint32_t p0 : pidx) {
      available[p0] = 0;
      nb.clear();
      g.query(vec[p0], radius, [&](std::uint32_t j) {
        if (available[j]) nb.push_back(j);
      });
      std::sort(nb.begin(), nb.end());
      for (std::size_t a = 0; a < nb.size(); ++a)
        for (std::size_t b = a + 1; b < nb.size(); ++b)
          for (std::size_t c = b + 1; c < nb.size(); ++c) {
            const std::uint32_t q[4] = {p0, nb[a], nb[b], nb[c]};
            bool ok = true;
            if (!cfg.simplify_pattern) {
              for (int i = 0; i < 4 && ok; ++i)
                for (int j = i + 1; j < 4 && ok; ++j) {
                  const V3f& u = vec[q[i]];
                  const V3f& v = vec[q[j]];
                  const float d = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
                  ok = static_cast<double>(d) > cos_fov;
                }
            }
            if (ok) {
              std::array<std::uint32_t, 4> s = {q[0], q[1], q[2], q[3]};
              std::sort(s.begin(), s.end());
              patterns.push_back(s);
            }
          }
    }
  }
  std::sort(patterns.begin(), patterns.end());
  patterns.erase(std::unique(patterns.begin(), patterns.end()), patterns.end());

  // 4. Verification stars (l.1015-1025).
  std::vector<char> keep_ver = keep_for_patterns;
  thin(vec, 0.6 * min_fov / std::sqrt(cfg.verification_stars_per_fov), 1, keep_ver);
  std::vector<std::uint32_t> remap(n, 0);
  DbContents out;
  std::uint32_t m = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (!keep_ver[i]) continue;
    remap[i] = m++;
    const V3f& v = vec[i];
    out.star_table.insert(out.star_table.end(),
                          {rows[i].ra, rows[i].dec, v[0], v[1], v[2], rows[i].mag});
    out.catalog_ids.push_back(rows[i].id);
  }
  if (sets_by_id) {
    sets_by_id->clear();
    for (const auto& p : patterns) {
      std::array<std::uint32_t, 4> s = {rows[p[0]].id, rows[p[1]].id, rows[p[2]].id, rows[p[3]].id};
      std::sort(s.begin(), s.end());
      sets_by_id->push_back(s);
    }
  }

  // 5. Hash table (l.1039-1084): 2N slots, quadratic probing, presorted stars.
  const std::size_t slots = 2 * patterns.size();
  out.slots.assign(slots, {0, 0, 0, 0});
  if (cfg.save_largest_edge) out.largest_edge_half.assign(slots, 0);
  db::Header& h = out.header;
  h.num_slots = static_cast<std::uint32_t>(slots);
  h.probe_mode = db::ProbeMode::kQuadraticMagic;
  const std::uint64_t magic = 2654435761ULL;
  for (const auto& p : patterns) {
    std::array<std::uint32_t, 4> idx = {remap[p[0]], remap[p[1]], remap[p[2]], remap[p[3]]};
    double vv[4][3];
    for (int i = 0; i < 4; ++i)
      for (int k = 0; k < 3; ++k) vv[i][k] = out.star_table[idx[i] * 6 + 2 + k];
    double edges[6];
    int e = 0;
    for (int i = 0; i < 4; ++i)
      for (int j = i + 1; j < 4; ++j) {
        const double dx = vv[i][0] - vv[j][0], dy = vv[i][1] - vv[j][1], dz = vv[i][2] - vv[j][2];
        edges[e++] = 2.0 * std::asin(0.5 * std::sqrt(dx * dx + dy * dy + dz * dz));
      }
    std::sort(edges, edges + 6);
    std::uint32_t key[5];
    for (int i = 0; i < 5; ++i) key[i] = static_cast<std::uint32_t>(edges[i] / edges[5] * bins);
    const std::uint64_t kh = pattern_key_hash(key, static_cast<std::uint32_t>(bins));
    const std::uint64_t start = (kh * magic) % slots;
    if (cfg.presort_patterns) {
      float c[3] = {0, 0, 0};
      for (int k = 0; k < 3; ++k) {
        for (int i = 0; i < 4; ++i) c[k] += out.star_table[idx[i] * 6 + 2 + k];
        c[k] /= 4.0f;
      }
      double rad[4];
      for (int i = 0; i < 4; ++i) {
        double s2 = 0;
        for (int k = 0; k < 3; ++k) {
          const double d = vv[i][k] - static_cast<double>(c[k]);
          s2 += d * d;
        }
        rad[i] = std::sqrt(s2);
      }
      std::array<int, 4> ord = {0, 1, 2, 3};
      std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) { return rad[a] < rad[b]; });
      idx = {idx[ord[0]], idx[ord[1]], idx[ord[2]], idx[ord[3]]};
    }
    std::uint64_t c = 0;
    for (;; ++c) {
      if (c > slots) throw std::runtime_error("quadratic probing found no empty slot");
      const std::uint64_t s = (start + c * c) % slots;
      auto& row = out.slots[s];
      if ((row[0] | row[1] | row[2] | row[3]) == 0) {
        row = idx;
        if (cfg.save_largest_edge)
          out.largest_edge_half[s] = db::float_to_half(static_cast<float>(edges[5] * 1000));
        break;
      }
    }
  }
  // An all-zero pattern can only be "star 0 four times", which cannot occur.

  h.pattern_bins = static_cast<std::uint16_t>(bins);
  h.pattern_max_error = cfg.pattern_max_error;
  h.max_fov = max_fov;
  h.min_fov = min_fov;
  h.star_max_magnitude = cfg.star_max_magnitude;
  h.epoch_proper_motion = epoch;
  h.epoch_equinox = catalog.epoch_equinox;
  h.range_ra[0] = cfg.range_ra_deg ? deg2rad(cfg.range_ra_deg->first) : std::nan("");
  h.range_ra[1] = cfg.range_ra_deg ? deg2rad(cfg.range_ra_deg->second) : std::nan("");
  h.range_dec[0] = cfg.range_dec_deg ? deg2rad(cfg.range_dec_deg->first) : std::nan("");
  h.range_dec[1] = cfg.range_dec_deg ? deg2rad(cfg.range_dec_deg->second) : std::nan("");
  h.pattern_stars_per_fov = static_cast<std::uint16_t>(cfg.pattern_stars_per_fov);
  h.verification_stars_per_fov = static_cast<std::uint16_t>(cfg.verification_stars_per_fov);
  h.num_patterns = static_cast<std::uint32_t>(patterns.size());
  h.threshold_patterns = static_cast<std::uint32_t>(slots / 2);
  h.catalog = catalog.kind;
  h.flags = (cfg.presort_patterns ? db::kFlagPresorted : 0u) |
            (cfg.simplify_pattern ? db::kFlagSimplifyPattern : 0u);

  if (report) {
    report->catalog_stars = n;
    report->pattern_stars =
        static_cast<std::size_t>(std::count(keep_for_patterns.begin(), keep_for_patterns.end(), 1));
    report->verification_stars = m;
    report->patterns = patterns.size();
    report->pattern_fovs_deg.clear();
    for (double f : fovs) report->pattern_fovs_deg.push_back(f * 180.0 / kPi);
  }
  return out;
}

}  // namespace st::tools
