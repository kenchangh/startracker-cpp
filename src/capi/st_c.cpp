// SPDX-License-Identifier: Apache-2.0
// C API over the core. Everything lives in the caller's memory block: the st_tracker object at
// its start, then an arena for every component.
#define ST_C_BUILD 1
#include "st/c/st.h"

#include <cmath>
#include <new>

#include "st/centroid/centroider.hpp"
#include "st/core/arena.hpp"
#include "st/core/clock.hpp"
#include "st/db/database.hpp"
#include "st/solver/lost_in_space.hpp"
#include "st/track/sky_index.hpp"
#include "st/track/tracker.hpp"

#ifndef ST_VERSION_STRING
#define ST_VERSION_STRING "0.0.0"
#endif

using namespace st;

struct st_tracker {
  Database db;
  Centroider centroider;
  LostInSpaceSolver lis;
  SkyIndex index;
  Tracker tracker;
  SteadyClock clock;
  st_config cfg{};
  std::span<Spot> spots;
  std::span<Centroid> cents;
  std::span<Roi> rois;
  std::span<Spot> roi_spots;
  std::span<bool> roi_found;
};

namespace {

constexpr std::uint32_t kGrid = 16;
constexpr std::size_t kHeader = (sizeof(st_tracker) + 63) / 64 * 64;

st_status to_c(Status s) noexcept { return static_cast<st_status>(static_cast<int>(s)); }

st_config resolved(const st_config& in) noexcept {
  st_config c = in;
  if (c.max_centroids == 0) c.max_centroids = 64;
  if (c.roi_half_size == 0) c.roi_half_size = 8;
  if (c.max_rois == 0) c.max_rois = 32;
  if (!(c.centroid_sigma > 0)) c.centroid_sigma = 2.0f;
  if (c.centroid_min_area == 0) c.centroid_min_area = 5;
  if (c.centroid_max_area == 0) c.centroid_max_area = 100;
  if (!(c.match_radius_px > 0)) c.match_radius_px = 4.0;
  if (c.min_matches == 0) c.min_matches = 5;
  if (c.rate_window == 0) c.rate_window = 16;
  return c;
}

// Applies a st_solver_variant preset. CEDAR also uses cedar-solve's default match threshold
// (1e-5) unless the caller set one. Returns false for an unknown variant.
bool apply_variant(std::uint32_t variant, bool threshold_set, SolveOptions& o) noexcept {
  switch (variant) {
    case ST_VARIANT_ESA: o.variants = SolverVariants::esa(); return true;
    case ST_VARIANT_CEDAR:
      o.variants = SolverVariants::cedar();
      if (!threshold_set) o.match_threshold = 1e-5;
      return true;
    default: return false;
  }
}

CameraIntrinsics intrinsics(const st_camera& c) noexcept {
  CameraIntrinsics k;
  k.width = c.width;
  k.height = c.height;
  k.fx = c.fx;
  k.fy = c.fy;
  k.cx = c.cx;
  k.cy = c.cy;
  k.k1 = c.k1;
  k.k2 = c.k2;
  k.k3 = c.k3;
  k.p1 = c.p1;
  k.p2 = c.p2;
  k.mirrored = c.mirrored != 0;
  return k;
}

CentroiderConfig centroider_config(const st_config& c) noexcept {
  CentroiderConfig cc;
  cc.max_width = c.camera.width;
  cc.max_height = c.camera.height;
  cc.sigma = c.centroid_sigma;
  cc.min_area = c.centroid_min_area;
  cc.max_area = c.centroid_max_area;
  return cc;
}

TrackerConfig tracker_config(const st_config& c) noexcept {
  TrackerConfig tc;
  tc.camera = intrinsics(c.camera);
  tc.max_centroids = c.max_centroids;
  tc.match_radius_px = c.match_radius_px;
  tc.min_matches = c.min_matches;
  tc.rate_window = c.rate_window;
  apply_variant(c.variant, false, tc.lis);  // validated in st_tracker_create
  if (tc.max_predicted < tc.min_matches) tc.max_predicted = tc.min_matches;
  return tc;
}

std::size_t arena_bytes(const st_config& c, const Database& db) noexcept {
  return Centroider::workspace_bytes(centroider_config(c)) +
         LostInSpaceSolver::workspace_bytes(db, SolverLimits{}) +
         SkyIndex::workspace_bytes(db, kGrid) + Tracker::workspace_bytes(tracker_config(c)) +
         std::size_t{c.max_centroids} * (sizeof(Spot) + sizeof(Centroid)) +
         std::size_t{c.max_rois} * (sizeof(Roi) + sizeof(Spot) + 1) + std::size_t{16} * 64;
}

ImageView view(const void* p, std::uint32_t w, std::uint32_t h, std::size_t stride,
               st_pixel_format f) noexcept {
  return ImageView{p, w, h, stride, f == ST_MONO8 ? PixelFormat::kMono8 : PixelFormat::kMono16};
}

void fill(const AttitudeSolution& s, st_solution& o) noexcept {
  o.status = to_c(s.status);
  o.mode = static_cast<st_mode>(static_cast<int>(s.mode));
  o.t_ns = s.t_ns;
  o.q_cam_icrs[0] = s.q_cam_icrs.w;
  o.q_cam_icrs[1] = s.q_cam_icrs.x;
  o.q_cam_icrs[2] = s.q_cam_icrs.y;
  o.q_cam_icrs[3] = s.q_cam_icrs.z;
  for (std::size_t i = 0; i < 3; ++i) {
    for (std::size_t j = 0; j < 3; ++j) o.covariance[i * 3 + j] = s.covariance.m[i][j];
  }
  o.rate[0] = s.rate.x;
  o.rate[1] = s.rate.y;
  o.rate[2] = s.rate.z;
  o.rate_valid = s.rate_valid ? 1 : 0;
  o.matches = s.matches;
  o.rmse_arcsec = s.rmse_arcsec;
  o.ra_deg = s.ra_deg;
  o.dec_deg = s.dec_deg;
  o.roll_deg = s.roll_deg;
}

}  // namespace

extern "C" {

const char* st_version(void) { return ST_VERSION_STRING; }

const char* st_status_string(st_status s) {
  switch (static_cast<int>(s)) {  // the value may be outside the enum (from C or Python)
    case ST_OK: return "ST_OK";
    case ST_INVALID_ARGUMENT: return "ST_INVALID_ARGUMENT";
    case ST_CAPACITY_EXCEEDED: return "ST_CAPACITY_EXCEEDED";
    case ST_ARENA_EXHAUSTED: return "ST_ARENA_EXHAUSTED";
    case ST_NOT_INITIALIZED: return "ST_NOT_INITIALIZED";
    case ST_CORRUPT_DATA: return "ST_CORRUPT_DATA";
    case ST_CHECKSUM_MISMATCH: return "ST_CHECKSUM_MISMATCH";
    case ST_VERSION_MISMATCH: return "ST_VERSION_MISMATCH";
    case ST_UNSUPPORTED: return "ST_UNSUPPORTED";
    case ST_TOO_FEW_STARS: return "ST_TOO_FEW_STARS";
    case ST_NO_MATCH: return "ST_NO_MATCH";
    case ST_TIMEOUT: return "ST_TIMEOUT";
    case ST_WORK_BUDGET_EXHAUSTED: return "ST_WORK_BUDGET_EXHAUSTED";
    case ST_CANCELLED: return "ST_CANCELLED";
    case ST_IO_ERROR: return "ST_IO_ERROR";
    default: return "ST_UNKNOWN";
  }
}

void st_config_init(st_config* c) {
  if (c == nullptr) return;
  *c = st_config{};
  c->roi_tracking = 1;
  *c = resolved(*c);
}

st_camera st_camera_ideal(uint32_t width, uint32_t height, double fov_deg) {
  const CameraIntrinsics k = CameraModel::ideal(width, height, fov_deg * kDegToRad);
  st_camera c{};
  c.width = k.width;
  c.height = k.height;
  c.fx = k.fx;
  c.fy = k.fy;
  c.cx = k.cx;
  c.cy = k.cy;
  return c;
}

size_t st_tracker_memory_size(const st_config* config, const void* db, size_t db_size) {
  if (config == nullptr || db == nullptr) return 0;
  auto d = Database::open(std::span<const std::byte>(static_cast<const std::byte*>(db), db_size));
  if (!d.ok()) return 0;
  return kHeader + arena_bytes(resolved(*config), d.value());
}

st_status st_tracker_create(const st_config* config, const void* db, size_t db_size, void* memory,
                            size_t memory_size, st_tracker** out) {
  if (config == nullptr || db == nullptr || memory == nullptr || out == nullptr ||
      reinterpret_cast<std::uintptr_t>(memory) % 64 != 0) {
    return ST_INVALID_ARGUMENT;
  }
  *out = nullptr;
  if (memory_size < kHeader) return ST_ARENA_EXHAUSTED;
  const st_config c = resolved(*config);
  if (c.camera.width == 0 || c.camera.height == 0 || !(c.camera.fx > 0)) return ST_INVALID_ARGUMENT;
  if (c.variant != ST_VARIANT_ESA && c.variant != ST_VARIANT_CEDAR) return ST_INVALID_ARGUMENT;
  auto d = Database::open(std::span<const std::byte>(static_cast<const std::byte*>(db), db_size));
  if (!d.ok()) return to_c(d.status());
  auto* t = ::new (memory) st_tracker{};
  t->cfg = c;
  t->db = d.value();
  Arena arena(
      std::span<std::byte>(static_cast<std::byte*>(memory) + kHeader, memory_size - kHeader));
  auto cent = Centroider::create(centroider_config(c), arena);
  if (!cent.ok()) return to_c(cent.status());
  t->centroider = cent.value();
  auto lis = LostInSpaceSolver::create(t->db, SolverLimits{}, arena);
  if (!lis.ok()) return to_c(lis.status());
  t->lis = lis.value();
  auto idx = SkyIndex::create(t->db, kGrid, arena);
  if (!idx.ok()) return to_c(idx.status());
  t->index = idx.value();
  auto tr = Tracker::create(t->index, &t->lis, tracker_config(c), arena);
  if (!tr.ok()) return to_c(tr.status());
  t->tracker = tr.value();
  auto spots = arena.allocate<Spot>(c.max_centroids);
  auto cents = arena.allocate<Centroid>(c.max_centroids);
  auto rois = arena.allocate<Roi>(c.max_rois);
  auto rspots = arena.allocate<Spot>(c.max_rois);
  auto found = arena.allocate<bool>(c.max_rois);
  if (!spots.ok() || !cents.ok() || !rois.ok() || !rspots.ok() || !found.ok())
    return ST_ARENA_EXHAUSTED;
  t->spots = spots.value();
  t->cents = cents.value();
  t->rois = rois.value();
  t->roi_spots = rspots.value();
  t->roi_found = found.value();
  *out = t;
  return ST_OK;
}

st_status st_extract_centroids(st_tracker* t, const void* pixels, uint32_t width, uint32_t height,
                               size_t stride_bytes, st_pixel_format format, st_centroid* out,
                               uint32_t capacity, uint32_t* count) {
  if (t == nullptr || pixels == nullptr || count == nullptr || (out == nullptr && capacity > 0)) {
    return ST_INVALID_ARGUMENT;
  }
  *count = 0;
  const std::uint32_t cap = capacity < t->cfg.max_centroids ? capacity : t->cfg.max_centroids;
  if (cap == 0 || out == nullptr) return ST_OK;
  auto r =
      t->centroider.extract(view(pixels, width, height, stride_bytes, format), t->spots.first(cap));
  if (!r.ok()) return to_c(r.status());
  for (std::uint32_t i = 0; i < r.value(); ++i) out[i] = {t->spots[i].x, t->spots[i].y};
  *count = r.value();
  return ST_OK;
}

st_status st_track_centroids(st_tracker* t, int64_t t_ns, const st_centroid* centroids,
                             uint32_t count, st_solution* out) {
  if (t == nullptr || out == nullptr || (centroids == nullptr && count > 0))
    return ST_INVALID_ARGUMENT;
  const std::uint32_t n = count < t->cfg.max_centroids ? count : t->cfg.max_centroids;
  for (std::uint32_t i = 0; i < n; ++i) t->cents[i] = {centroids[i].x, centroids[i].y};
  AttitudeSolution s;
  t->tracker.update(t_ns, t->cents.first(n), s);
  *out = st_solution{};
  fill(s, *out);
  out->num_centroids = n;
  return out->status;
}

st_status st_track_image(st_tracker* t, int64_t t_ns, const void* pixels, uint32_t width,
                         uint32_t height, size_t stride_bytes, st_pixel_format format,
                         st_solution* out) {
  if (t == nullptr || pixels == nullptr || out == nullptr) return ST_INVALID_ARGUMENT;
  *out = st_solution{};
  const ImageView img = view(pixels, width, height, stride_bytes, format);
  std::uint32_t n = 0;
  bool used_rois = false;
  if (t->cfg.roi_tracking != 0 && t->tracker.mode() == TrackMode::kTracking) {
    const std::uint32_t nr = t->tracker.predict_rois(t_ns, t->cfg.roi_half_size, t->rois);
    if (nr >= 4 &&
        t->centroider.extract_rois(img, t->rois.first(nr), RoiConfig{}, t->roi_spots.first(nr),
                                   t->roi_found.first(nr)) == Status::kOk) {
      for (std::uint32_t i = 0; i < nr && n < t->cents.size(); ++i) {
        if (t->roi_found[i]) t->cents[n++] = {t->roi_spots[i].x, t->roi_spots[i].y};
      }
      used_rois = true;
    }
  }
  if (!used_rois) {
    auto r = t->centroider.extract(img, t->spots);
    if (!r.ok()) {
      out->status = to_c(r.status());
      return out->status;
    }
    for (std::uint32_t i = 0; i < r.value(); ++i) t->cents[n++] = {t->spots[i].x, t->spots[i].y};
  }
  AttitudeSolution s;
  t->tracker.update(t_ns, t->cents.first(n), s);
  fill(s, *out);
  out->num_centroids = n;
  out->used_rois = used_rois ? 1 : 0;
  return out->status;
}

st_status st_solve_lost_in_space(st_tracker* t, const st_centroid* centroids, uint32_t count,
                                 uint32_t width, uint32_t height, const st_lis_options* options,
                                 st_lis_result* out) {
  if (t == nullptr || out == nullptr || (centroids == nullptr && count > 0))
    return ST_INVALID_ARGUMENT;
  *out = st_lis_result{};
  const std::uint32_t n = count < t->cfg.max_centroids ? count : t->cfg.max_centroids;
  for (std::uint32_t i = 0; i < n; ++i) t->cents[i] = {centroids[i].x, centroids[i].y};
  SolveOptions o;
  if (options != nullptr) {
    if (options->fov_estimate_deg > 0) o.fov_estimate = options->fov_estimate_deg * kDegToRad;
    if (options->fov_max_error_deg > 0) o.fov_max_error = options->fov_max_error_deg * kDegToRad;
    if (options->pattern_checking_stars > 0)
      o.pattern_checking_stars = options->pattern_checking_stars;
    if (options->match_radius > 0) o.match_radius = options->match_radius;
    if (options->match_threshold > 0) o.match_threshold = options->match_threshold;
    o.distortion = options->distortion;
    if (!apply_variant(options->variant, options->match_threshold > 0, o)) {
      out->status = ST_INVALID_ARGUMENT;
      return ST_INVALID_ARGUMENT;
    }
  }
  SolveResult r;
  const std::int64_t t0 = t->clock.now_ns();
  t->lis.solve(t->cents.first(n), width, height, o, r);
  out->solve_ms = static_cast<double>(t->clock.now_ns() - t0) / 1e6;
  out->status = to_c(r.status);
  out->ra_deg = r.ra_deg;
  out->dec_deg = r.dec_deg;
  out->roll_deg = r.roll_deg;
  out->fov_deg = r.fov / kDegToRad;
  out->rmse_arcsec = r.rmse_arcsec;
  out->p90e_arcsec = r.p90e_arcsec;
  out->maxe_arcsec = r.maxe_arcsec;
  out->probability = r.probability;
  out->num_matches = r.num_matches;
  out->q_cam_icrs[0] = r.q_cam_icrs.w;
  out->q_cam_icrs[1] = r.q_cam_icrs.x;
  out->q_cam_icrs[2] = r.q_cam_icrs.y;
  out->q_cam_icrs[3] = r.q_cam_icrs.z;
  return out->status;
}

void st_tracker_set_attitude(st_tracker* t, int64_t t_ns, const double q[4]) {
  if (t == nullptr || q == nullptr) return;
  t->tracker.set_attitude(t_ns, normalized(Quat{q[0], q[1], q[2], q[3]}));
}

void st_tracker_reset(st_tracker* t) {
  if (t != nullptr) t->tracker.reset();
}

}  // extern "C"
