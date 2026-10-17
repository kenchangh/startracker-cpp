// SPDX-License-Identifier: Apache-2.0
// Port of ESA tetra3 solve_from_centroids (tetra3/tetra3.py l.1230-1752 @ f9fa2eb) with
// cedar-solve variants (tetra3/tetra3.py l.1584-2301 @ 607e3f8). Comments "E:nnn" / "C:nnn"
// point at the reference lines. Float32 steps mirror numpy's float32 arithmetic in tetra3.
#include "st/solver/lost_in_space.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "st/math/linalg.hpp"

namespace st {

namespace {

struct Pt {
  double y = 0, x = 0;  // tetra3 (y, x) order
};
struct Key {
  std::uint16_t k[5] = {0, 0, 0, 0, 0};
  std::uint32_t dist = 0;
};
struct ChainEntry {
  std::uint32_t stars[4] = {0, 0, 0, 0};
  float edges[6] = {0, 0, 0, 0, 0, 0};  // sorted edge angles (float32, as tetra3)
};
struct Nearby {
  std::uint32_t star = 0;
  Pt c{};
  Vec3 v{};  // float32 catalog vector promoted to double
};

double dist2(const Pt& a, const Pt& b) noexcept {
  const double dy = a.y - b.y, dx = a.x - b.x;
  return dy * dy + dx * dx;
}

// _compute_vectors (E:155): centroids cast to float32, then double arithmetic.
Vec3 compute_vector(const Pt& p, double width, double height, double fov) noexcept {
  const double s = std::tan(fov / 2) / width * 2;
  const auto yf = static_cast<double>(static_cast<float>(p.y));
  const auto xf = static_cast<double>(static_cast<float>(p.x));
  const Vec3 v{1.0, (width / 2 - xf) * s, (height / 2 - yf) * s};
  return v / norm(v);
}

// _undistort_centroids (E:190): float32 arithmetic throughout.
Pt undistort(const Pt& p, double width, double height, double k) noexcept {
  const auto hc = static_cast<float>(height / 2), wc = static_cast<float>(width / 2);
  float y = static_cast<float>(p.y) - hc;
  float x = static_cast<float>(p.x) - wc;
  const float r = std::sqrt(y * y + x * x);
  const float t = r / static_cast<float>(width) * 2.0f;
  const float kf = static_cast<float>(k);
  const float scale = (1.0f - kf * (t * t)) / static_cast<float>(1.0 - k);
  y *= scale;
  x *= scale;
  y += hc;
  x += wc;
  return {static_cast<double>(y), static_cast<double>(x)};
}

// Sorted (ascending) edge angles of 4 unit vectors, 2*asin(|a-b|/2), double (pdist path).
void edge_angles(const Vec3 (&v)[4], double (&e)[6]) noexcept {
  int n = 0;
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) e[n++] = angle_between_unit(v[i], v[j]);
  }
  std::sort(e, e + 6);
}

// Indices 0..3 ordered by distance from the mean vector (stable for ties).
void order_by_radius(const Vec3 (&v)[4], int (&ord)[4]) noexcept {
  const Vec3 c = (v[0] + v[1] + v[2] + v[3]) / 4.0;
  double r[4];
  for (int i = 0; i < 4; ++i) {
    r[i] = norm(v[i] - c);
    ord[i] = i;
  }
  // Stable insertion sort (std::stable_sort may allocate).
  for (int i = 1; i < 4; ++i) {
    const int cur = ord[i];
    int j = i;
    while (j > 0 && r[ord[j - 1]] > r[cur]) {
      ord[j] = ord[j - 1];
      --j;
    }
    ord[j] = cur;
  }
}

Mat3 cross_covariance(const Vec3* img, const Vec3* cat, std::size_t n) noexcept {
  Mat3 h;
  for (std::size_t i = 0; i < n; ++i) h = h + outer(img[i], cat[i]);
  return h;
}

bool budget_hit(std::uint64_t used, std::uint64_t max) noexcept { return max != 0 && used > max; }

}  // namespace

struct LostInSpaceSolver::Workspace {
  std::span<Pt> raw;        // trimmed input centroids (y, x)
  std::span<Pt> und;        // undistorted (kFixed) or per-candidate undistorted (kRange)
  std::span<Pt> preundist;  // kRange: steps * N
  std::span<double> dist_range;
  std::span<double> preundist_ratios;   // kRange: steps * 5
  std::span<std::uint32_t> candidates;  // centroid indices usable in patterns
  std::span<Key> keys;
  std::span<ChainEntry> chain;
  std::span<Nearby> nearby;
  std::span<SolveMatch> matches;
  std::span<std::int32_t> best_for_image;
  std::span<std::int32_t> best_for_catalog;
  std::span<Vec3> vec_a;
  std::span<Vec3> vec_b;
  std::span<double> scratch;
  std::uint32_t cap_n = 0;
  std::uint32_t cap_nearby = 0;
};

std::size_t LostInSpaceSolver::workspace_bytes(const Database& db,
                                               const SolverLimits& limits) noexcept {
  const std::size_t n =
      limits.max_centroids != 0 ? limits.max_centroids : db.header().verification_stars_per_fov;
  const std::size_t m = n * std::max<std::uint32_t>(1, limits.max_nearby_trim_factor);
  const std::size_t steps = limits.max_distortion_steps;
  return sizeof(Workspace) + 2 * n * sizeof(Pt) + steps * n * sizeof(Pt) + steps * 6 * 8 + n * 4 +
         limits.max_keys_per_pattern * sizeof(Key) +
         (db.max_probe_chain() + 1u) * sizeof(ChainEntry) + m * sizeof(Nearby) +
         n * sizeof(SolveMatch) + n * 4 + m * 4 + 2 * n * sizeof(Vec3) + n * n * 8 +
         std::size_t{16} * 64;  // alignment padding
}

Result<LostInSpaceSolver> LostInSpaceSolver::create(const Database& db,
                                                    const SolverLimits& limits_in,
                                                    Arena& arena) noexcept {
  SolverLimits limits = limits_in;
  if (limits.max_centroids == 0) limits.max_centroids = db.header().verification_stars_per_fov;
  if (limits.max_centroids < 4 || db.header().pattern_bins == 0 ||
      limits.max_keys_per_pattern == 0 || limits.max_distortion_steps < 2 ||
      limits.max_nearby_trim_factor == 0) {
    return Status::kInvalidArgument;
  }
  const std::uint32_t n = limits.max_centroids;
  const std::uint32_t m = n * limits.max_nearby_trim_factor;
  const std::uint32_t steps = limits.max_distortion_steps;
  ST_TRY_ASSIGN(auto wsp, arena.allocate<Workspace>(1));
  Workspace& ws = wsp[0];
  ST_TRY_ASSIGN(ws.raw, arena.allocate<Pt>(n));
  ST_TRY_ASSIGN(ws.und, arena.allocate<Pt>(n));
  ST_TRY_ASSIGN(ws.preundist, arena.allocate<Pt>(static_cast<std::size_t>(steps) * n));
  ST_TRY_ASSIGN(ws.dist_range, arena.allocate<double>(steps));
  ST_TRY_ASSIGN(ws.preundist_ratios, arena.allocate<double>(static_cast<std::size_t>(steps) * 5));
  ST_TRY_ASSIGN(ws.candidates, arena.allocate<std::uint32_t>(n));
  ST_TRY_ASSIGN(ws.keys, arena.allocate<Key>(limits.max_keys_per_pattern));
  ST_TRY_ASSIGN(ws.chain, arena.allocate<ChainEntry>(db.max_probe_chain() + 1u));
  ST_TRY_ASSIGN(ws.nearby, arena.allocate<Nearby>(m));
  ST_TRY_ASSIGN(ws.matches, arena.allocate<SolveMatch>(n));
  ST_TRY_ASSIGN(ws.best_for_image, arena.allocate<std::int32_t>(n));
  ST_TRY_ASSIGN(ws.best_for_catalog, arena.allocate<std::int32_t>(m));
  ST_TRY_ASSIGN(ws.vec_a, arena.allocate<Vec3>(n));
  ST_TRY_ASSIGN(ws.vec_b, arena.allocate<Vec3>(n));
  ST_TRY_ASSIGN(ws.scratch, arena.allocate<double>(static_cast<std::size_t>(n) * n));
  ws.cap_n = n;
  ws.cap_nearby = m;
  LostInSpaceSolver s;
  s.db_ = &db;
  s.limits_ = limits;
  s.ws_ = &ws;
  return s;
}

namespace {

// State shared by the candidate verification of one solve() call.
struct Ctx {
  const Database* db = nullptr;
  const SolveOptions* opt = nullptr;
  double width = 0, height = 0;
  std::uint32_t n = 0;  // trimmed centroid count
  double fov_initial = 0;
  bool fov_given = false, err_given = false;
  double threshold = 0;
  std::uint32_t nearby_cap = 0;
};

// _get_nearby_stars (E:1754) + _compute_centroids (E:171) + trim (E:1573), streamed: stars are
// visited in index (brightness) order and the first `cap` that land inside the image are kept,
// which yields exactly tetra3's trimmed list without buffering all nearby stars.
std::uint32_t find_nearby(const Ctx& c, const Mat3& r, double fov, std::span<Nearby> out,
                          std::uint32_t cap) noexcept {
  const Vec3 center = r.row(0);
  const double radius = fov * std::sqrt(c.width * c.width + c.height * c.height) / c.width / 2;
  const double max_dist = 2 * std::sin(radius / 2);
  const double cos_r = std::cos(radius);
  const double lo[3] = {center.x - max_dist, center.y - max_dist, center.z - max_dist};
  const double hi[3] = {center.x + max_dist, center.y + max_dist, center.z + max_dist};
  const double scale = -c.width / 2 / std::tan(fov / 2);
  std::uint32_t k = 0;
  const std::uint32_t ns = c.db->num_stars();
  for (std::uint32_t i = 0; i < ns && k < cap; ++i) {
    const auto f = c.db->star_vector(i);
    const auto vx = static_cast<double>(f[0]), vy = static_cast<double>(f[1]),
               vz = static_cast<double>(f[2]);
    if (!(vx > lo[0] && vx < hi[0] && vy > lo[1] && vy < hi[1] && vz > lo[2] && vz < hi[2])) {
      continue;
    }
    const Vec3 v{vx, vy, vz};
    if (!(dot(center, v) > cos_r)) continue;
    const Vec3 d = r * v;
    const Pt p{scale * d.z / d.x + c.height / 2, scale * d.y / d.x + c.width / 2};
    if (!(p.y > 0 && p.x > 0 && p.y < c.height && p.x < c.width)) continue;
    out[k++] = {i, p, v};
  }
  return k;
}

// _find_centroid_matches (E:246 / C:321). Returns the match count; fills `out` in tetra3 order.
std::uint32_t find_matches(std::span<const Pt> img, std::span<const Nearby> cat, double r,
                           MatchDedup mode, std::span<std::int32_t> best_img,
                           std::span<std::int32_t> best_cat, std::span<SolveMatch> out) noexcept {
  const auto ni = static_cast<std::uint32_t>(img.size());
  const auto nc = static_cast<std::uint32_t>(cat.size());
  std::uint32_t k = 0;
  // scipy's cdist(...) < r compares true distances (not squared ones) in double.
  auto within = [&](std::uint32_t i, std::uint32_t j) {
    return std::sqrt(dist2(img[i], cat[j].c)) < r;
  };
  if (mode == MatchDedup::kImageFirst) {
    // For each image star: the smallest catalog index within r.
    for (std::uint32_t i = 0; i < ni; ++i) {
      best_img[i] = -1;
      for (std::uint32_t j = 0; j < nc; ++j) {
        if (within(i, j)) {
          best_img[i] = static_cast<std::int32_t>(j);
          break;
        }
      }
    }
    // For each catalog star: the smallest image index that chose it; output by catalog index.
    for (std::uint32_t j = 0; j < nc; ++j) best_cat[j] = -1;
    for (std::uint32_t i = 0; i < ni; ++i) {
      const std::int32_t j = best_img[i];
      if (j >= 0 && best_cat[static_cast<std::uint32_t>(j)] < 0) {
        best_cat[static_cast<std::uint32_t>(j)] = static_cast<std::int32_t>(i);
      }
    }
    for (std::uint32_t j = 0; j < nc; ++j) {
      if (best_cat[j] >= 0) out[k++] = {static_cast<std::uint32_t>(best_cat[j]), j};
    }
  } else {
    // For each catalog star: the smallest image index within r.
    for (std::uint32_t j = 0; j < nc; ++j) {
      best_cat[j] = -1;
      for (std::uint32_t i = 0; i < ni; ++i) {
        if (within(i, j)) {
          best_cat[j] = static_cast<std::int32_t>(i);
          break;
        }
      }
    }
    // For each image star: the smallest catalog index that chose it; output by image index.
    for (std::uint32_t i = 0; i < ni; ++i) best_img[i] = -1;
    for (std::uint32_t j = 0; j < nc; ++j) {
      const std::int32_t i = best_cat[j];
      if (i >= 0 && best_img[static_cast<std::uint32_t>(i)] < 0) {
        best_img[static_cast<std::uint32_t>(i)] = static_cast<std::int32_t>(j);
      }
    }
    for (std::uint32_t i = 0; i < ni; ++i) {
      if (best_img[i] >= 0) out[k++] = {i, static_cast<std::uint32_t>(best_img[i])};
    }
  }
  return k;
}

// Two-column least squares min |A x - b| for A = [a0 a1] (modified Gram-Schmidt QR).
bool lstsq2(const double* a0, const double* a1, const double* b, std::size_t n, double& x0,
            double& x1) noexcept {
  double r00 = 0;
  for (std::size_t i = 0; i < n; ++i) r00 += a0[i] * a0[i];
  r00 = std::sqrt(r00);
  if (!(r00 > 0)) return false;
  double r01 = 0, qb0 = 0;
  for (std::size_t i = 0; i < n; ++i) {
    r01 += a0[i] / r00 * a1[i];
    qb0 += a0[i] / r00 * b[i];
  }
  double r11 = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double v = a1[i] - r01 * a0[i] / r00;
    r11 += v * v;
  }
  r11 = std::sqrt(r11);
  if (!(r11 > 0)) return false;
  double qb1 = 0;
  for (std::size_t i = 0; i < n; ++i) qb1 += (a1[i] - r01 * a0[i] / r00) / r11 * b[i];
  x1 = qb1 / r11;
  x0 = (qb0 - r01 * x1) / r00;
  return true;
}

void insertion_sort(double* a, std::size_t n) noexcept {
  for (std::size_t i = 1; i < n; ++i) {
    const double v = a[i];
    std::size_t j = i;
    while (j > 0 && a[j - 1] > v) {
      a[j] = a[j - 1];
      --j;
    }
    a[j] = v;
  }
}

// Colex ("breadth-first") successor of a 4-combination over [0, n). False when exhausted.
bool next_colex(std::uint32_t (&c)[4], std::uint32_t n) noexcept {
  for (int i = 0; i < 4; ++i) {
    const std::uint32_t limit = (i == 3) ? n : c[i + 1];
    if (c[i] + 1 < limit) {
      ++c[i];
      for (int j = 0; j < i; ++j) c[j] = static_cast<std::uint32_t>(j);
      return true;
    }
  }
  return false;
}

// Lexicographic successor (itertools.combinations order).
bool next_lex(std::uint32_t (&c)[4], std::uint32_t n) noexcept {
  for (int i = 3; i >= 0; --i) {
    if (c[i] < n - 4 + static_cast<std::uint32_t>(i)) {
      ++c[i];
      for (int j = i + 1; j < 4; ++j) c[j] = c[j - 1] + 1;
      return true;
    }
  }
  return false;
}

// Enumerates, in lexicographic order and without storage, exactly the distinct sorted tuples of
// itertools.product(range(lo[0], hi[0]+1), ..., range(lo[4], hi[4]+1)) -- i.e. numpy's
// np.unique(np.sort(product, axis=1), axis=0) as tetra3 builds it (E:1425-1429). A sorted tuple
// is reachable iff its values can be assigned to distinct dimensions whose ranges contain them
// (checked greedily: ascending values, each to the open range ending soonest). Recursion depth
// is at most 5. Work: O(5 * (max hi - min lo) * keys * 25).
class SortedKeys {
 public:
  SortedKeys(const int (&lo)[5], const int (&hi)[5]) noexcept {
    vmin_ = lo[0];
    vmax_ = hi[0];
    for (int i = 0; i < 5; ++i) {
      lo_[i] = lo[i];
      hi_[i] = hi[i];
      vmin_ = std::min(vmin_, lo[i]);
      vmax_ = std::max(vmax_, hi[i]);
    }
  }
  bool next(int (&out)[5]) noexcept {
    bool ok = false;
    if (first_) {
      first_ = false;
      ok = fill(0);
    } else {
      for (int pos = 4; pos >= 0 && !ok; --pos) {
        for (int v = t_[pos] + 1; v <= vmax_ && !ok; ++v) {
          t_[pos] = v;
          ok = feasible(pos + 1) && fill(pos + 1);
        }
      }
    }
    if (ok) {
      for (int i = 0; i < 5; ++i) out[i] = t_[i];
    }
    return ok;
  }

 private:
  bool fill(int pos) noexcept {
    if (pos == 5) return true;
    for (int v = pos == 0 ? vmin_ : t_[pos - 1]; v <= vmax_; ++v) {
      t_[pos] = v;
      if (feasible(pos + 1) && fill(pos + 1)) return true;
    }
    return false;
  }
  bool feasible(int len) const noexcept {
    bool used[5] = {false, false, false, false, false};
    for (int i = 0; i < len; ++i) {
      int best = -1;
      for (int d = 0; d < 5; ++d) {
        if (!used[d] && lo_[d] <= t_[i] && t_[i] <= hi_[d] && (best < 0 || hi_[d] < hi_[best])) {
          best = d;
        }
      }
      if (best < 0) return false;
      used[best] = true;
    }
    return true;
  }
  int lo_[5] = {}, hi_[5] = {};
  int vmin_ = 0, vmax_ = 0;
  int t_[5] = {};
  bool first_ = true;
};

bool key_less(const Key& a, const Key& b) noexcept {
  for (int i = 0; i < 5; ++i) {
    if (a.k[i] != b.k[i]) return a.k[i] < b.k[i];
  }
  return false;
}

}  // namespace

Status LostInSpaceSolver::solve(std::span<const Centroid> centroids, std::uint32_t width_px,
                                std::uint32_t height_px, const SolveOptions& opt,
                                SolveResult& out) noexcept {
  out = SolveResult{};
  auto finish = [&](Status s) {
    out.status = s;
    return s;
  };
  if (ws_ == nullptr || db_ == nullptr) return finish(Status::kNotInitialized);
  if (width_px == 0 || height_px == 0 || !(opt.match_radius > 0) || !(opt.match_threshold > 0)) {
    return finish(Status::kInvalidArgument);
  }
  Workspace& ws = *ws_;
  const Database& db = *db_;
  const db::Header& h = db.header();
  const SolverVariants& var = opt.variants;
  if (var.nearby_trim_factor == 0 || var.nearby_trim_factor > limits_.max_nearby_trim_factor) {
    return finish(Status::kCapacityExceeded);
  }
  const std::int64_t t0 = opt.clock != nullptr ? opt.clock->now_ns() : 0;
  auto elapsed = [&] { return opt.clock != nullptr ? opt.clock->now_ns() - t0 : 0; };

  Ctx c;
  c.db = &db;
  c.opt = &opt;
  c.width = width_px;
  c.height = height_px;
  c.fov_given = std::isfinite(opt.fov_estimate);
  c.err_given = std::isfinite(opt.fov_max_error);
  // E:1326-1331: unknown FOV starts at the middle of the database range.
  c.fov_initial = c.fov_given ? opt.fov_estimate : (h.max_fov + h.min_fov) / 2;
  c.threshold = opt.match_threshold / static_cast<double>(h.threshold_patterns);

  // E:1360: trim to the verification density.
  std::uint32_t n = static_cast<std::uint32_t>(
      std::min<std::size_t>(centroids.size(), h.verification_stars_per_fov));
  if (n > ws.cap_n) n = ws.cap_n;
  c.n = n;
  c.nearby_cap = std::min(ws.cap_nearby, n * var.nearby_trim_factor);
  for (std::uint32_t i = 0; i < n; ++i) ws.raw[i] = {centroids[i].y, centroids[i].x};
  if (n < 4) return finish(Status::kTooFewStars);

  const double p_max_err =
      var.match_max_error > h.pattern_max_error ? var.match_max_error : h.pattern_max_error;
  const auto bins = static_cast<double>(h.pattern_bins);

  // E:1365-1378: distortion preparation.
  std::span<const Pt> img = ws.raw;  // tetra3's image_centroids after this block
  std::uint32_t steps = 0;
  if (opt.distortion_mode == DistortionMode::kFixed) {
    for (std::uint32_t i = 0; i < n; ++i) {
      ws.und[i] = undistort(ws.raw[i], c.width, c.height, opt.distortion);
    }
    img = ws.und.first(n);
  } else if (opt.distortion_mode == DistortionMode::kRange) {
    const double dmin = std::min(opt.distortion_min, opt.distortion_max);
    const double dmax = std::max(opt.distortion_min, opt.distortion_max);
    const double span_r = std::round((dmax - dmin) * 1e6) / 1e6;
    steps = static_cast<std::uint32_t>(std::ceil(span_r * 10) + 1);
    if (steps < 2) steps = 2;  // linspace(min, max, 1) would degenerate; tetra3 gets >= 1
    if (steps > limits_.max_distortion_steps) return finish(Status::kCapacityExceeded);
    const double step = (dmax - dmin) / static_cast<double>(steps - 1);
    for (std::uint32_t s = 0; s < steps; ++s) {
      ws.dist_range[s] = (s + 1 == steps) ? dmax : dmin + step * static_cast<double>(s);
      for (std::uint32_t i = 0; i < n; ++i) {
        ws.preundist[s * n + i] = undistort(ws.raw[i], c.width, c.height, ws.dist_range[s]);
      }
    }
    img = ws.raw.first(n);
  }

  // Pattern candidates (cedar cluster buster restricted to the trimmed set; see design note).
  std::uint32_t ncand = 0;
  if (var.centroid_cluster_buster) {
    const double sep = c.width * 0.6 / std::sqrt(static_cast<double>(h.verification_stars_per_fov));
    for (std::uint32_t i = 0; i < n; ++i) {
      bool occupied = false;
      for (std::uint32_t j = 0; j < ncand && !occupied; ++j) {
        occupied = std::sqrt(dist2(ws.raw[i], ws.raw[ws.candidates[j]])) <= sep;
      }
      if (!occupied) ws.candidates[ncand++] = i;
    }
  } else {
    for (std::uint32_t i = 0; i < n; ++i) ws.candidates[i] = i;
    ncand = n;
  }
  const std::uint32_t npat = var.pattern_order == PatternOrder::kLexicographic
                                 ? std::min(ncand, opt.pattern_checking_stars)
                                 : ncand;
  if (npat < 4) return finish(Status::kNoMatch);

  std::uint32_t comb[4] = {0, 1, 2, 3};
  bool more = true;
  for (; more; more = var.pattern_order == PatternOrder::kLexicographic ? next_lex(comb, npat)
                                                                        : next_colex(comb, npat)) {
    // E:1384: timeout (and cancellation) checked once per image pattern.
    if (opt.clock != nullptr && opt.timeout_ns > 0 && elapsed() > opt.timeout_ns) {
      return finish(Status::kTimeout);
    }
    if (opt.cancel != nullptr && opt.cancel->load(std::memory_order_relaxed)) {
      return finish(Status::kCancelled);
    }
    ++out.stats.image_patterns;
    if (budget_hit(out.stats.image_patterns, opt.budget.max_image_patterns)) {
      return finish(Status::kWorkBudgetExhausted);
    }
    const std::uint32_t pidx[4] = {ws.candidates[comb[0]], ws.candidates[comb[1]],
                                   ws.candidates[comb[2]], ws.candidates[comb[3]]};
    double largest_distance = -1;  // E:1391 cache
    double ratio_min[5], ratio_max[5], image_ratio[5];
    double image_largest_edge = 0;
    if (opt.distortion_mode != DistortionMode::kRange) {
      Vec3 v[4];
      for (int i = 0; i < 4; ++i)
        v[i] = compute_vector(img[pidx[i]], c.width, c.height, c.fov_initial);
      double e[6];
      edge_angles(v, e);
      image_largest_edge = e[5];
      for (int i = 0; i < 5; ++i) {
        image_ratio[i] = e[i] / image_largest_edge;
        ratio_min[i] = image_ratio[i] - p_max_err;
        ratio_max[i] = image_ratio[i] + p_max_err;
      }
    } else {
      for (int i = 0; i < 5; ++i) {
        ratio_min[i] = 1.0;
        ratio_max[i] = 0.0;
      }
      for (std::uint32_t s = 0; s < steps; ++s) {
        Vec3 v[4];
        for (int i = 0; i < 4; ++i) {
          v[i] = compute_vector(ws.preundist[s * n + pidx[i]], c.width, c.height, c.fov_initial);
        }
        double e[6];
        edge_angles(v, e);
        image_largest_edge = e[5];  // tetra3 keeps the last one (E:1416)
        for (int i = 0; i < 5; ++i) {
          const double r = e[i] / e[5];
          ws.preundist_ratios[s * 5 + static_cast<std::uint32_t>(i)] = r;
          ratio_min[i] = s == 0 ? r : std::min(ratio_min[i], r);
          ratio_max[i] = s == 0 ? r : std::max(ratio_max[i], r);
        }
      }
      for (int i = 0; i < 5; ++i) image_ratio[i] = (ratio_min[i] + ratio_max[i]) / 2;
    }

    // E:1422-1429: key space.
    int lo[5], hi[5];
    std::uint64_t count = 1;
    for (int i = 0; i < 5; ++i) {
      lo[i] = static_cast<int>(std::max(0.0, ratio_min[i] * bins));
      hi[i] = static_cast<int>(std::min(bins, ratio_max[i] * bins));
      if (hi[i] < lo[i]) {
        count = 0;
        break;
      }
      count *= static_cast<std::uint64_t>(hi[i] - lo[i] + 1);
    }
    if (count == 0) continue;
    std::uint32_t nkeys = 0;
    SortedKeys sorted_keys(lo, hi);
    if (var.key_order == KeyOrder::kByDistance) {
      // cedar: product order tagged with the squared distance to the measured key; tuples that
      // are not non-decreasing can never index a database pattern (database keys are sorted
      // ratios), so they are skipped (documented divergence Q8).
      if (count > ws.keys.size()) {
        ++out.stats.skipped_patterns;  // more keys than max_keys_per_pattern
        continue;
      }
      int image_key[5];
      for (int i = 0; i < 5; ++i) image_key[i] = static_cast<int>(image_ratio[i] * bins);
      int cur[5] = {lo[0], lo[1], lo[2], lo[3], lo[4]};
      for (;;) {
        bool sorted = true;
        for (int i = 1; i < 5; ++i) sorted = sorted && cur[i - 1] <= cur[i];
        if (sorted) {
          Key k;
          std::uint32_t d = 0;
          for (int i = 0; i < 5; ++i) {
            const int diff = cur[i] - image_key[i];
            d += static_cast<std::uint32_t>(diff * diff);
            k.k[i] = static_cast<std::uint16_t>(cur[i]);
          }
          k.dist = d;
          ws.keys[nkeys++] = k;
        }
        int dd = 4;
        while (dd >= 0 && cur[dd] == hi[dd]) {
          cur[dd] = lo[dd];
          --dd;
        }
        if (dd < 0) break;
        ++cur[dd];
      }
      // Keys are distinct, so (dist, key) is a total order and std::sort (which never
      // allocates, unlike std::stable_sort) gives Python's sort order.
      std::sort(ws.keys.begin(), ws.keys.begin() + nkeys, [](const Key& a, const Key& b) {
        if (a.dist != b.dist) return a.dist < b.dist;
        return key_less(a, b);
      });
    }
    for (std::uint32_t ki = 0;; ++ki) {
      std::uint32_t kk[5];
      if (var.key_order == KeyOrder::kByDistance) {
        if (ki >= nkeys) break;
        for (int i = 0; i < 5; ++i) kk[i] = ws.keys[ki].k[i];
      } else {
        int t[5];
        if (!sorted_keys.next(t)) break;
        for (int i = 0; i < 5; ++i) kk[i] = static_cast<std::uint32_t>(t[i]);
      }
      ++out.stats.keys;
      const std::uint64_t key_hash = pattern_key_hash(kk, h.pattern_bins);
      const std::uint64_t start = db.start_slot(key_hash);
      // E:124: probe until an empty slot (bounded by the database's max_probe_chain).
      std::uint32_t nchain = 0;
      std::array<std::uint32_t, 4> p{};
      for (std::uint32_t cprobe = 0; cprobe <= db.max_probe_chain(); ++cprobe) {
        const std::uint64_t slot = db.probe_slot(start, cprobe);
        ++out.stats.slot_reads;
        if (!db.pattern(slot, p)) break;
        if (var.use_key_hash_filter && db.has_key_hash() &&
            db.key_hash(slot) != static_cast<std::uint16_t>(key_hash & 0xFFFFu)) {
          continue;
        }
        if (db.has_largest_edge() && c.fov_given && c.err_given) {
          // E:1440 (computed in float32 as cedar-solve does; ESA's float16 math is quirk j).
          const float fov2 = db.largest_edge_mrad(slot) / static_cast<float>(image_largest_edge) *
                             static_cast<float>(c.fov_initial) / 1000.0f;
          if (!(std::fabs(static_cast<double>(fov2) - opt.fov_estimate) < opt.fov_max_error)) {
            continue;
          }
        }
        ChainEntry& ce = ws.chain[nchain++];
        std::memcpy(ce.stars, p.data(), sizeof(ce.stars));
        // E:1453-1460: float32 edge lengths, sorted, then 2*arcsin(0.5*d) in float32.
        int ne = 0;
        std::array<float, 3> vf[4];
        for (int i = 0; i < 4; ++i) vf[i] = db.star_vector(p[static_cast<std::size_t>(i)]);
        for (int i = 0; i < 4; ++i) {
          for (int j = i + 1; j < 4; ++j) {
            const float dx = vf[i][0] - vf[j][0], dy = vf[i][1] - vf[j][1],
                        dz = vf[i][2] - vf[j][2];
            ce.edges[ne++] = std::sqrt(dx * dx + dy * dy + dz * dz);
          }
        }
        std::sort(ce.edges, ce.edges + 6);
        for (float& e : ce.edges) e = 2.0f * std::asin(0.5f * e);
      }
      if (budget_hit(out.stats.slot_reads, opt.budget.max_slot_reads)) {
        return finish(Status::kWorkBudgetExhausted);
      }

      for (std::uint32_t ci = 0; ci < nchain; ++ci) {
        const ChainEntry& ce = ws.chain[ci];
        const float cat_largest = ce.edges[5];
        float cat_ratio[5];
        bool valid = true;
        for (int i = 0; i < 5; ++i) {
          cat_ratio[i] = ce.edges[i] / cat_largest;
          const auto r = static_cast<double>(cat_ratio[i]);
          valid = valid && ratio_min[i] < r && ratio_max[i] > r;
        }
        if (!valid) continue;
        ++out.stats.candidates;
        if (budget_hit(out.stats.candidates, opt.budget.max_candidates)) {
          return finish(Status::kWorkBudgetExhausted);
        }

        // E:1473-1511: coarse distortion for kRange.
        std::span<const Pt> img_und = img;
        if (opt.distortion_mode == DistortionMode::kRange) {
          std::uint32_t low = 0, high = 1;
          auto err = [&](std::uint32_t s, int i) {
            return static_cast<double>(cat_ratio[i]) -
                   ws.preundist_ratios[s * 5 + static_cast<std::uint32_t>(i)];
          };
          if (steps > 2) {
            std::uint32_t closest = 0;
            double best = 0;
            for (std::uint32_t s = 0; s < steps; ++s) {
              double sum = 0;
              for (int i = 0; i < 5; ++i) sum += err(s, i) * err(s, i);
              ws.scratch[s] = sum;
              if (s == 0 || sum < best) {
                best = sum;
                closest = s;
              }
            }
            if (closest == 0) {
              low = 0;
              high = 1;
            } else if (closest == steps - 1) {
              low = steps - 2;
              high = steps - 1;
            } else if (ws.scratch[closest + 1] > ws.scratch[closest - 1]) {
              low = closest;
              high = closest + 1;
            } else {
              low = closest - 1;
              high = closest;
            }
          }
          double xm = 0;
          for (int i = 0; i < 5; ++i) xm += err(low, i) / (err(low, i) - err(high, i));
          xm /= 5;
          const double dist_est =
              ws.dist_range[low] + xm * (ws.dist_range[high] - ws.dist_range[low]);
          for (std::uint32_t i = 0; i < n; ++i)
            ws.und[i] = undistort(ws.raw[i], c.width, c.height, dist_est);
          img_und = ws.und.first(n);
        }

        // E:1513-1534: coarse FOV.
        double fov = 0;
        if (c.fov_given && opt.distortion_mode != DistortionMode::kRange) {
          fov = static_cast<double>(cat_largest) / image_largest_edge * c.fov_initial;
        } else {
          if (largest_distance < 0 || opt.distortion_mode == DistortionMode::kRange) {
            largest_distance = 0;
            for (int i = 0; i < 4; ++i) {
              for (int j = i + 1; j < 4; ++j) {
                largest_distance = std::max(largest_distance,
                                            std::sqrt(dist2(img_und[pidx[i]], img_und[pidx[j]])));
              }
            }
          }
          const double f = largest_distance / 2 / std::tan(static_cast<double>(cat_largest) / 2);
          fov = 2 * std::atan(c.width / 2 / f);
        }
        if (c.fov_given && c.err_given && std::fabs(fov - opt.fov_estimate) > opt.fov_max_error) {
          continue;
        }

        // E:1536-1559: pattern rotation.
        Vec3 iv[4], cv[4];
        for (int i = 0; i < 4; ++i)
          iv[i] = compute_vector(img_und[pidx[i]], c.width, c.height, fov);
        int iord[4];
        order_by_radius(iv, iord);
        for (int i = 0; i < 4; ++i) {
          const auto f = db.star_vector(ce.stars[i]);
          cv[i] = {static_cast<double>(f[0]), static_cast<double>(f[1]), static_cast<double>(f[2])};
        }
        Vec3 ivs[4], cvs[4];
        for (int i = 0; i < 4; ++i) ivs[i] = iv[iord[i]];
        if ((h.flags & db::kFlagPresorted) != 0) {
          for (int i = 0; i < 4; ++i) cvs[i] = cv[i];
        } else {
          // Unsorted database: order by radius from the float32 centroid (E:1549-1555).
          int cord[4];
          float cen[3] = {0, 0, 0};
          for (int k2 = 0; k2 < 3; ++k2) {
            for (int i = 0; i < 4; ++i) cen[k2] += static_cast<float>(cv[i][k2]);
            cen[k2] /= 4.0f;
          }
          double rr[4];
          for (int i = 0; i < 4; ++i) {
            rr[i] = norm(cv[i] - Vec3{static_cast<double>(cen[0]), static_cast<double>(cen[1]),
                                      static_cast<double>(cen[2])});
            cord[i] = i;
          }
          for (int i = 1; i < 4; ++i) {  // stable insertion sort
            const int v = cord[i];
            int j = i;
            while (j > 0 && rr[cord[j - 1]] > rr[v]) {
              cord[j] = cord[j - 1];
              --j;
            }
            cord[j] = v;
          }
          for (int i = 0; i < 4; ++i) cvs[i] = cv[cord[i]];
        }
        Mat3 rot = orthogonal_polar(cross_covariance(ivs, cvs, 4));
        if (var.reject_reflection && determinant(rot) < 0) continue;

        // E:1561-1578: nearby stars and matches.
        const std::uint32_t nn = find_nearby(c, rot, fov, ws.nearby, c.nearby_cap);
        const std::uint32_t nm =
            find_matches(img_und.first(n), std::span<const Nearby>(ws.nearby.data(), nn),
                         c.width * opt.match_radius, var.match_dedup, ws.best_for_image,
                         ws.best_for_catalog, ws.matches);
        // E:1585-1591: mismatch probability.
        const double p_single = static_cast<double>(nn) * opt.match_radius * opt.match_radius;
        const double prob = binomial_cdf(static_cast<long>(n) - (static_cast<long>(nm) - 2),
                                         static_cast<long>(n), 1 - p_single);
        if (!(prob < c.threshold)) continue;

        // E:1601-1660: refine on all matches.
        // ESA uses image_centroids (raw for kNone/kRange, pre-undistorted for kFixed); cedar's
        // total-k variant uses undistorted for the rotation and raw for the k fit.
        std::span<const Pt> base = img;
        if (var.distortion_total_k && opt.distortion_mode == DistortionMode::kFixed) base = img_und;
        for (std::uint32_t m = 0; m < nm; ++m) {
          ws.vec_a[m] = compute_vector(base[ws.matches[m].centroid_index], c.width, c.height, fov);
          ws.vec_b[m] = ws.nearby[ws.matches[m].star_index].v;
        }
        rot = orthogonal_polar(cross_covariance(ws.vec_a.data(), ws.vec_b.data(), nm));
        out.ra_deg = std::fmod(std::atan2(rot.m[0][1], rot.m[0][0]) * kRadToDeg + 360.0, 360.0);
        out.dec_deg = std::atan2(rot.m[0][2], std::hypot(rot.m[1][2], rot.m[2][2])) * kRadToDeg;
        out.roll_deg = std::fmod(std::atan2(rot.m[1][2], rot.m[2][2]) * kRadToDeg + 360.0, 360.0);

        double k_fit = std::numeric_limits<double>::quiet_NaN();
        if (opt.distortion_mode == DistortionMode::kNone) {
          // E:1619-1621: mean ratio of catalogue to camera angles over all pairs.
          double sum = 0;
          std::size_t cnt = 0;
          for (std::uint32_t i = 0; i < nm; ++i) {
            for (std::uint32_t j = i + 1; j < nm; ++j) {
              sum += angle_between_unit(ws.vec_b[i], ws.vec_b[j]) /
                     angle_between_unit(ws.vec_a[i], ws.vec_a[j]);
              ++cnt;
            }
          }
          fov *= sum / static_cast<double>(cnt);
          for (std::uint32_t m = 0; m < nm; ++m) {
            ws.vec_a[m] =
                compute_vector(base[ws.matches[m].centroid_index], c.width, c.height, fov);
          }
        } else {
          // E:1627-1649: least squares r = f*tan(theta) + k*r^3, then f /= (1 - k).
          std::span<const Pt> kbase = img;  // ESA: image_centroids; cedar: raw centroids
          if (var.distortion_total_k) kbase = ws.raw.first(n);
          double* tanv = ws.scratch.data();
          double* r3 = tanv + nm;
          double* rv = r3 + nm;
          for (std::uint32_t m = 0; m < nm; ++m) {
            const Vec3 d = rot * ws.vec_b[m];
            tanv[m] = std::hypot(d.y, d.z) / d.x;
            const Pt& q = kbase[ws.matches[m].centroid_index];
            const double r = std::hypot(q.y - c.height / 2, q.x - c.width / 2) / c.width * 2;
            r3[m] = r * r * r;
            rv[m] = r;
          }
          double f = 0, k = 0;
          if (!lstsq2(tanv, r3, rv, nm, f, k)) continue;
          f = f / (1 - k);
          fov = 2 * std::atan(1 / f);
          k_fit = k;
          for (std::uint32_t m = 0; m < nm; ++m) {
            const Pt q = undistort(kbase[ws.matches[m].centroid_index], c.width, c.height, k);
            ws.vec_a[m] = compute_vector(q, c.width, c.height, fov);
          }
        }

        // E:1651-1660: residuals with the (coarse-FOV) rotation.
        const Mat3 rt = transpose(rot);
        double sum2 = 0;
        double* dists = ws.scratch.data();
        for (std::uint32_t m = 0; m < nm; ++m) {
          const Vec3 sky = rt * ws.vec_a[m];
          const double dd = norm(sky - ws.vec_b[m]);
          dists[m] = dd;
          const double a = 2 * std::asin(0.5 * dd);
          sum2 += a * a;
        }
        insertion_sort(dists, nm);
        const auto to_arcsec = [](double d) { return 2 * std::asin(0.5 * d) * kRadToDeg * 3600; };
        out.rmse_arcsec = std::sqrt(sum2 / nm) * kRadToDeg * 3600;
        out.p90e_arcsec = to_arcsec(dists[static_cast<std::size_t>(0.9 * (nm - 1))]);
        out.maxe_arcsec = to_arcsec(dists[nm - 1]);
        out.fov = fov;
        out.distortion = k_fit;
        out.probability = prob * static_cast<double>(h.threshold_patterns);
        out.num_matches = nm;
        out.rotation_tetra3 = rot;
        out.proper_rotation = determinant(rot) > 0;
        // tetra3 camera frame (boresight, -X, -Y) -> ours (X, Y, Z boresight).
        Mat3 to_cv;
        to_cv.m[0] = {0, -1, 0};
        to_cv.m[1] = {0, 0, -1};
        to_cv.m[2] = {1, 0, 0};
        out.q_cam_icrs = from_matrix(to_cv * rot);
        // Report matches against the caller's centroid indices and database star rows.
        for (std::uint32_t m = 0; m < nm; ++m) {
          ws.matches[m].star_index = ws.nearby[ws.matches[m].star_index].star;
        }
        out.matches = std::span<const SolveMatch>(ws.matches.data(), nm);
        for (int i = 0; i < 4; ++i) out.pattern[i] = pidx[i];
        out.solve_ns = elapsed();
        return finish(Status::kOk);
      }
    }
  }
  out.solve_ns = elapsed();
  return finish(Status::kNoMatch);
}

}  // namespace st
