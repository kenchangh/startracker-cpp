// SPDX-License-Identifier: Apache-2.0
#pragma once

// Tracking mode: predicts the attitude from the previous solution (estimated body rate, or a
// gyro through RateSource), matches predicted catalog stars to centroids with the calibrated
// camera model, and solves Wahba's problem. Falls back to lost-in-space after
// `lost_after_failures` consecutive failures (or when no attitude is known).
//
// Conventions: attitude q_cam_icrs (camera frame +Z boresight, +X right, +Y down). Body rate
// omega is the camera's angular velocity in the camera frame: over dt,
//   q(t + dt) = from_rotation_vector(-omega * dt) * q(t).
//
// All memory from an Arena at create(); update() never allocates. Worst case per update:
// O(index cells + stars in the predicted cone + P*C) for P predicted stars and C centroids,
// plus one lost-in-space solve when falling back (see lost_in_space.hpp).

#include <cstdint>
#include <span>

#include "st/camera/camera.hpp"
#include "st/centroid/centroider.hpp"
#include "st/containers/ring_buffer.hpp"
#include "st/core/arena.hpp"
#include "st/core/result.hpp"
#include "st/solver/lost_in_space.hpp"
#include "st/track/sky_index.hpp"

namespace st {

// Gyro hook. Implementations must be real-time safe (no allocation, bounded time).
class RateSource {
 public:
  // Camera rotation between two times: q_cam(t1) = dq * q_cam(t0) (dq maps frame t0 to t1).
  // Returns false when unavailable; the tracker then uses its own rate estimate.
  virtual bool rotation_between(std::int64_t t0_ns, std::int64_t t1_ns, Quat& dq) noexcept = 0;

 protected:
  RateSource() = default;
  RateSource(const RateSource&) = default;
  RateSource& operator=(const RateSource&) = default;
  ~RateSource() = default;
};

enum class TrackMode : std::uint8_t {
  kLost,         // no valid attitude
  kLostInSpace,  // attitude from a lost-in-space solve this frame
  kTracking,     // attitude from tracking
};

struct TrackerConfig {
  CameraIntrinsics camera;
  std::uint32_t max_predicted = 48;   // brightest predicted stars used
  std::uint32_t max_centroids = 256;  // centroids considered per frame
  double match_radius_px = 4.0;       // base search radius
  double rate_uncertainty = 0.002;    // rad/s; widens the radius by rate_uncertainty*dt*f
  std::uint32_t min_matches = 5;
  double max_rmse_arcsec = 60.0;
  double outlier_sigma = 3.0;      // residual cut for the second Wahba pass
  double centroid_sigma_px = 0.2;  // floor of the per-star noise for the covariance
  std::uint32_t lost_after_failures = 2;
  std::uint32_t rate_window = 16;  // attitudes in the rate least-squares fit (2..32)
  double fov_margin = 1.05;        // predicted cone = margin * half diagonal FOV
  // Lost-in-space fallback options (FOV hint is filled from the camera when unset).
  SolveOptions lis{};
};

struct AttitudeSolution {
  Status status = Status::kNotInitialized;  // kOk when q is valid this frame
  TrackMode mode = TrackMode::kLost;
  std::int64_t t_ns = 0;
  Quat q_cam_icrs{};
  Mat3 covariance{};  // small-angle attitude error covariance, camera frame, rad^2
  Vec3 rate{};        // estimated body rate, camera frame, rad/s
  bool rate_valid = false;
  std::uint32_t matches = 0;
  double rmse_arcsec = 0;
  double ra_deg = 0, dec_deg = 0, roll_deg = 0;  // tetra3 conventions, for convenience
};

namespace detail {
struct RateSample {
  std::int64_t t_ns = 0;
  Quat q{};
};
}  // namespace detail

class Tracker {
 public:
  // `lis` may be null (tracking only; initialise with set_attitude()). Errors:
  // kInvalidArgument (camera or limits), kArenaExhausted.
  static Result<Tracker> create(const SkyIndex& index, LostInSpaceSolver* lis,
                                const TrackerConfig& config, Arena& arena) noexcept;
  // Upper bound of the arena bytes create() takes (alignment padding included). O(1).
  static std::size_t workspace_bytes(const TrackerConfig& config) noexcept;

  Tracker() noexcept = default;

  // Processes one frame's centroids (brightest first, pixel coordinates).
  Status update(std::int64_t t_ns, std::span<const Centroid> centroids,
                AttitudeSolution& out) noexcept;

  // Predicted stars for ROI centroiding at time t: writes up to rois.size() windows of
  // `half_size` around the brightest predicted stars. Returns the number written (0 when lost).
  std::uint32_t predict_rois(std::int64_t t_ns, std::uint32_t half_size,
                             std::span<Roi> rois) noexcept;

  // Seeds the tracker (e.g. from an external solution). O(1).
  void set_attitude(std::int64_t t_ns, const Quat& q_cam_icrs) noexcept;
  void set_rate_source(RateSource* source) noexcept { gyro_ = source; }
  void reset() noexcept;
  [[nodiscard]] TrackMode mode() const noexcept { return mode_; }
  [[nodiscard]] const CameraModel& camera() const noexcept { return cam_; }

 private:
  struct Predicted {
    std::uint32_t star = 0;
    float mag = 0;
    double x = 0, y = 0;
    Vec3 v{};  // ICRS
  };
  struct Pair {
    std::uint32_t predicted = 0;
    std::uint32_t centroid = 0;
  };
  Quat predict(std::int64_t t_ns) noexcept;
  std::uint32_t predict_stars(const Quat& q) noexcept;
  Status refine(const Quat& q_pred, double radius_px, std::span<const Centroid> c,
                AttitudeSolution& out) noexcept;
  void finish(std::int64_t t_ns, AttitudeSolution& out) noexcept;

  const SkyIndex* index_ = nullptr;
  LostInSpaceSolver* lis_ = nullptr;
  RateSource* gyro_ = nullptr;
  TrackerConfig cfg_{};
  CameraModel cam_{};
  std::span<Predicted> pred_;
  std::span<Pair> pairs_;
  std::span<std::uint8_t> used_;
  std::span<Vec3> meas_;
  std::span<Vec3> ref_;
  std::span<double> resid_;
  std::span<Centroid> lis_buf_;
  std::uint32_t npred_ = 0;
  TrackMode mode_ = TrackMode::kLost;
  bool have_attitude_ = false;
  std::int64_t t_last_ = 0;
  Quat q_last_{};
  Vec3 rate_{};
  bool rate_valid_ = false;
  RingBuffer<detail::RateSample, 32> history_;  // consecutive good attitudes
  std::uint32_t failures_ = 0;
  bool last_ok_ = false;  // previous frame produced an attitude (for rate estimation)
  double half_diag_fov_ = 0;
};

}  // namespace st
