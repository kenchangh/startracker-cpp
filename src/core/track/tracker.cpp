// SPDX-License-Identifier: Apache-2.0
#include "st/track/tracker.hpp"

#include <algorithm>
#include <cmath>

#include "st/math/attitude.hpp"
#include "st/math/linalg.hpp"

namespace st {
namespace {

Vec3 to_vec(const std::array<float, 3>& f) noexcept {
  return {static_cast<double>(f[0]), static_cast<double>(f[1]), static_cast<double>(f[2])};
}

// Inverse of a symmetric positive-definite 3x3 matrix (adjugate / determinant). False if
// singular.
bool invert3(const Mat3& a, Mat3& out) noexcept {
  const double det = determinant(a);
  if (!(std::fabs(det) > 1e-300)) return false;
  const auto& m = a.m;
  out.m[0] = {(m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det,
              (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det,
              (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det};
  out.m[1] = {(m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det,
              (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det,
              (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det};
  out.m[2] = {(m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det,
              (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det,
              (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det};
  return true;
}

}  // namespace

std::size_t Tracker::workspace_bytes(const TrackerConfig& c) noexcept {
  const std::size_t p = c.max_predicted, n = c.max_centroids;
  return p * (sizeof(Predicted) + sizeof(Pair) + 2 * sizeof(Vec3) + sizeof(double)) +
         n * (1 + sizeof(Centroid)) + std::size_t{8} * 64;
}

Result<Tracker> Tracker::create(const SkyIndex& index, LostInSpaceSolver* lis,
                                const TrackerConfig& cfg, Arena& arena) noexcept {
  ST_TRY_ASSIGN(auto cam, CameraModel::create(cfg.camera));
  if (cfg.max_predicted < 3 || cfg.max_centroids == 0 || cfg.min_matches < 3 ||
      cfg.rate_window < 2 || cfg.rate_window > 32 || cfg.min_matches > cfg.max_predicted ||
      !(cfg.match_radius_px > 0)) {
    return Status::kInvalidArgument;
  }
  Tracker t;
  t.index_ = &index;
  t.lis_ = lis;
  t.cfg_ = cfg;
  t.cam_ = cam;
  ST_TRY_ASSIGN(t.pred_, arena.allocate<Predicted>(cfg.max_predicted));
  ST_TRY_ASSIGN(t.pairs_, arena.allocate<Pair>(cfg.max_predicted));
  ST_TRY_ASSIGN(t.used_, arena.allocate<std::uint8_t>(cfg.max_centroids));
  ST_TRY_ASSIGN(t.meas_, arena.allocate<Vec3>(cfg.max_predicted));
  ST_TRY_ASSIGN(t.ref_, arena.allocate<Vec3>(cfg.max_predicted));
  ST_TRY_ASSIGN(t.resid_, arena.allocate<double>(cfg.max_predicted));
  ST_TRY_ASSIGN(t.lis_buf_, arena.allocate<Centroid>(cfg.max_centroids));
  // Half diagonal FOV from the corners of the calibrated camera.
  const double w = cfg.camera.width, h = cfg.camera.height;
  const Vec3 z{0, 0, 1};
  for (double x : {0.0, w}) {
    for (double y : {0.0, h})
      t.half_diag_fov_ = std::max(t.half_diag_fov_, angle_between_unit(cam.pixel_to_ray(x, y), z));
  }
  return t;
}

void Tracker::reset() noexcept {
  history_.clear();
  mode_ = TrackMode::kLost;
  have_attitude_ = false;
  rate_valid_ = false;
  failures_ = 0;
  last_ok_ = false;
}

void Tracker::set_attitude(std::int64_t t_ns, const Quat& q) noexcept {
  t_last_ = t_ns;
  q_last_ = normalized(q);
  have_attitude_ = true;
  rate_valid_ = false;
  last_ok_ = true;
  failures_ = 0;
  mode_ = TrackMode::kTracking;
  history_.clear();
  history_.push_overwrite({t_ns, q_last_});
}

Quat Tracker::predict(std::int64_t t_ns) noexcept {
  Quat dq;
  if (gyro_ != nullptr && gyro_->rotation_between(t_last_, t_ns, dq))
    return normalized(dq * q_last_);
  if (rate_valid_) {
    const double dt = static_cast<double>(t_ns - t_last_) * 1e-9;
    return normalized(from_rotation_vector(-dt * rate_) * q_last_);
  }
  return q_last_;
}

std::uint32_t Tracker::predict_stars(const Quat& q) noexcept {
  const Vec3 bore = rotate(conjugate(q), Vec3{0, 0, 1});
  const double w = cfg_.camera.width, h = cfg_.camera.height;
  const Mat3 r = to_matrix(q);
  std::uint32_t n = 0;
  const auto cap = static_cast<std::uint32_t>(pred_.size());
  const Database& db = index_->database();
  index_->query(bore, half_diag_fov_ * cfg_.fov_margin, [&](std::uint32_t s) {
    const Vec3 v = to_vec(db.star_vector(s));
    double x = 0, y = 0;
    if (!cam_.ray_to_pixel(r * v, x, y) || x < 0 || y < 0 || x >= w || y >= h) return;
    const float mag = db.star_magnitude(s);
    // Keep the `cap` brightest (ties by star index) in a sorted buffer.
    std::uint32_t pos = n;
    if (n == cap) {
      const Predicted& last = pred_[n - 1];
      if (!(mag < last.mag || (mag == last.mag && s < last.star))) return;
      pos = n - 1;
    } else {
      ++n;
    }
    while (pos > 0 &&
           (mag < pred_[pos - 1].mag || (mag == pred_[pos - 1].mag && s < pred_[pos - 1].star))) {
      pred_[pos] = pred_[pos - 1];
      --pos;
    }
    pred_[pos] = {s, mag, x, y, v};
  });
  return n;
}

Status Tracker::refine(const Quat& q_pred, double radius_px, std::span<const Centroid> cents,
                       AttitudeSolution& out) noexcept {
  npred_ = predict_stars(q_pred);
  const std::size_t nc = std::min<std::size_t>(cents.size(), used_.size());
  for (std::size_t i = 0; i < nc; ++i) used_[i] = 0;
  const double r2 = radius_px * radius_px;
  std::uint32_t np = 0;
  for (std::uint32_t p = 0; p < npred_; ++p) {
    std::int64_t best = -1;
    double best_d2 = r2;
    for (std::size_t i = 0; i < nc; ++i) {
      if (used_[i] != 0) continue;
      const double dx = cents[i].x - pred_[p].x, dy = cents[i].y - pred_[p].y;
      const double d2 = dx * dx + dy * dy;
      if (d2 <= best_d2) {
        best_d2 = d2;
        best = static_cast<std::int64_t>(i);
      }
    }
    if (best >= 0) {
      used_[static_cast<std::size_t>(best)] = 1;
      pairs_[np++] = {p, static_cast<std::uint32_t>(best)};
    }
  }
  if (np < cfg_.min_matches) return Status::kNoMatch;
  for (std::uint32_t k = 0; k < np; ++k) {
    const Centroid& c = cents[pairs_[k].centroid];
    meas_[k] = cam_.pixel_to_ray(c.x, c.y);
    ref_[k] = pred_[pairs_[k].predicted].v;
  }
  // Wahba (SVD with reflection correction), then one outlier-rejection pass.
  auto solve = [&](std::uint32_t n, Mat3& rot, double& rms) {
    Mat3 b;
    for (std::uint32_t k = 0; k < n; ++k) b = b + outer(meas_[k], ref_[k]);
    rot = proper_rotation(b);
    double s2 = 0;
    for (std::uint32_t k = 0; k < n; ++k) {
      resid_[k] = angle_between_unit(meas_[k], rot * ref_[k]);
      s2 += resid_[k] * resid_[k];
    }
    rms = std::sqrt(s2 / n);
  };
  Mat3 rot;
  double rms = 0;
  solve(np, rot, rms);
  const double pix_angle = 1.0 / cfg_.camera.fx;
  const double cut = std::max(cfg_.outlier_sigma * rms, 3.0 * cfg_.centroid_sigma_px * pix_angle);
  std::uint32_t keep = 0;
  for (std::uint32_t k = 0; k < np; ++k) {
    if (resid_[k] <= cut) {
      meas_[keep] = meas_[k];
      ref_[keep] = ref_[k];
      pairs_[keep] = pairs_[k];
      ++keep;
    }
  }
  if (keep < cfg_.min_matches) return Status::kNoMatch;
  if (keep < np) solve(keep, rot, rms);
  if (rms / kArcsecToRad > cfg_.max_rmse_arcsec) return Status::kNoMatch;
  // Covariance: sigma^2 * (sum (I - b b^T))^-1 in the camera frame (QUEST approximation).
  const double sigma = std::max(rms, cfg_.centroid_sigma_px * pix_angle);
  Mat3 f;
  for (std::uint32_t k = 0; k < keep; ++k)
    f = f + (Mat3::identity() + (-1.0) * outer(meas_[k], meas_[k]));
  Mat3 inv;
  if (!invert3(f, inv)) return Status::kNoMatch;
  out.covariance = (sigma * sigma) * inv;
  out.q_cam_icrs = from_matrix(rot);
  out.matches = keep;
  out.rmse_arcsec = rms / kArcsecToRad;
  return Status::kOk;
}

void Tracker::finish(std::int64_t t_ns, AttitudeSolution& out) noexcept {
  out.t_ns = t_ns;
  double ra = 0, dec = 0, roll = 0;
  radec_roll_from_attitude(out.q_cam_icrs, ra, dec, roll);
  out.ra_deg = ra * kRadToDeg;
  out.dec_deg = dec * kRadToDeg;
  out.roll_deg = roll * kRadToDeg;
  // Rate: least-squares slope of the rotation vectors of recent consecutive attitudes relative
  // to the newest one. With q_i = exp(-w (t_i - t_ref)) q_ref, r_i = rotvec(q_i q_ref^-1)
  // = -w tau_i, so w = -cov(tau, r) / var(tau). O(rate_window).
  if (!(last_ok_ && have_attitude_ && t_ns > t_last_)) history_.clear();
  history_.push_overwrite({t_ns, out.q_cam_icrs});
  while (history_.size() > cfg_.rate_window) history_.pop_front();
  if (history_.size() >= 2) {
    const std::size_t n = history_.size();
    double mt = 0;
    Vec3 mr;
    for (std::size_t i = 0; i < n; ++i) {
      const double tau = static_cast<double>(history_[i].t_ns - t_ns) * 1e-9;
      mt += tau;
      mr = mr + to_rotation_vector(canonical(history_[i].q * conjugate(out.q_cam_icrs)));
    }
    mt /= static_cast<double>(n);
    mr = mr / static_cast<double>(n);
    double var = 0;
    Vec3 cov;
    for (std::size_t i = 0; i < n; ++i) {
      const double tau = static_cast<double>(history_[i].t_ns - t_ns) * 1e-9 - mt;
      const Vec3 r = to_rotation_vector(canonical(history_[i].q * conjugate(out.q_cam_icrs))) - mr;
      var += tau * tau;
      cov = cov + tau * r;
    }
    if (var > 0) {
      rate_ = cov * (-1.0 / var);
      rate_valid_ = true;
    }
  }
  q_last_ = out.q_cam_icrs;
  t_last_ = t_ns;
  have_attitude_ = true;
  last_ok_ = true;
  failures_ = 0;
  out.rate = rate_;
  out.rate_valid = rate_valid_;
  out.status = Status::kOk;
}

Status Tracker::update(std::int64_t t_ns, std::span<const Centroid> cents,
                       AttitudeSolution& out) noexcept {
  out = AttitudeSolution{};
  out.t_ns = t_ns;
  if (have_attitude_ && mode_ != TrackMode::kLost) {
    const Quat q = predict(t_ns);
    const double dt = std::fabs(static_cast<double>(t_ns - t_last_) * 1e-9);
    const double radius = cfg_.match_radius_px + cfg_.rate_uncertainty * dt * cfg_.camera.fx;
    if (refine(q, radius, cents, out) == Status::kOk) {
      mode_ = TrackMode::kTracking;
      out.mode = mode_;
      finish(t_ns, out);
      return out.status;
    }
    last_ok_ = false;
    if (++failures_ < cfg_.lost_after_failures) {
      out.status = Status::kNoMatch;
      out.mode = mode_;
      out.q_cam_icrs = q;  // prediction, flagged by status
      out.rate = rate_;
      out.rate_valid = rate_valid_;
      return out.status;
    }
    mode_ = TrackMode::kLost;
    rate_valid_ = false;
  }
  out.mode = TrackMode::kLost;
  if (lis_ == nullptr) {
    out.status = Status::kNoMatch;
    return out.status;
  }
  // Lost in space: undistort with the calibrated model to the ideal pinhole the solver assumes.
  const std::size_t n = std::min(cents.size(), lis_buf_.size());
  for (std::size_t i = 0; i < n; ++i) {
    double xu = 0, yu = 0;
    cam_.undistort_pixel(cents[i].x, cents[i].y, xu, yu);
    // Shift the principal point to the image centre, as tetra3's camera model assumes.
    lis_buf_[i] = {xu - cfg_.camera.cx + cfg_.camera.width / 2.0,
                   yu - cfg_.camera.cy + cfg_.camera.height / 2.0};
  }
  SolveOptions opt = cfg_.lis;
  if (!std::isfinite(opt.fov_estimate)) {
    opt.fov_estimate = cam_.fov_x();
    if (!std::isfinite(opt.fov_max_error)) opt.fov_max_error = 0.5 * kDegToRad;
  }
  SolveResult r;
  if (lis_->solve(lis_buf_.first(n), cfg_.camera.width, cfg_.camera.height, opt, r) !=
          Status::kOk ||
      !r.proper_rotation) {
    out.status = r.status == Status::kOk ? Status::kNoMatch : r.status;
    return out.status;
  }
  // Refine with the calibrated camera (dt = 0).
  last_ok_ = false;  // no rate across a reacquisition
  const double radius = std::max(3 * cfg_.match_radius_px, 0.004 * cfg_.camera.width);
  if (refine(r.q_cam_icrs, radius, cents, out) != Status::kOk) {
    out.status = Status::kNoMatch;
    return out.status;
  }
  mode_ = TrackMode::kLostInSpace;
  out.mode = mode_;
  rate_valid_ = false;
  have_attitude_ = false;
  finish(t_ns, out);
  mode_ = TrackMode::kTracking;  // next frame tracks
  return out.status;
}

std::uint32_t Tracker::predict_rois(std::int64_t t_ns, std::uint32_t half_size,
                                    std::span<Roi> rois) noexcept {
  if (!have_attitude_ || mode_ == TrackMode::kLost) return 0;
  const std::uint32_t n = std::min<std::uint32_t>(predict_stars(predict(t_ns)),
                                                  static_cast<std::uint32_t>(rois.size()));
  for (std::uint32_t k = 0; k < n; ++k) {
    const auto x = static_cast<long>(std::floor(pred_[k].x)) - static_cast<long>(half_size);
    const auto y = static_cast<long>(std::floor(pred_[k].y)) - static_cast<long>(half_size);
    rois[k] = {static_cast<std::uint32_t>(std::max<long>(0, x)),
               static_cast<std::uint32_t>(std::max<long>(0, y)), 2 * half_size + 1,
               2 * half_size + 1};
  }
  return n;
}

}  // namespace st
