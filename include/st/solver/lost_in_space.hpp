// SPDX-License-Identifier: Apache-2.0
#pragma once

// Lost-in-space solver: ESA tetra3 (f9fa2eb) solve_from_centroids, ported for parity, with
// cedar-solve behaviours available as opt-in variants. Derived from tetra3 (Apache-2.0,
// Copyright 2019 the European Space Agency) and Cedar Solve (Apache-2.0, Copyright 2023
// Steven Rosenthal); see NOTICE.
//
// All working memory is taken from an Arena when the solver is created; solve() never
// allocates. Results reference solver memory and stay valid until the next solve().
//
// Coordinates: centroids are pixel (x, y), origin at the top-left corner of the image, pixel
// centres at +0.5, brightest first. Output attitude is q_cam_icrs: rotate(q, v_icrs) gives the
// direction in the camera frame (+Z boresight, +X right, +Y down; see camera.hpp). RA/Dec/Roll
// follow tetra3's definitions so they can be compared directly.
//
// Worst-case work for one solve(), with P image patterns (70 for 8 pattern stars), K keys per
// pattern (<= 32 with tetra3's default bins), C = max_probe_chain + 1 slot reads per key,
// N centroids, S catalog stars:
//   O(P * K * C * (S + N * M))  where M <= nearby_trim * N.
// The WorkBudget cuts this off deterministically; the optional deadline cuts it off in time.

#include <atomic>
#include <cstdint>
#include <limits>
#include <span>

#include "st/core/arena.hpp"
#include "st/core/clock.hpp"
#include "st/core/result.hpp"
#include "st/db/database.hpp"
#include "st/math/vec.hpp"

namespace st {

struct Centroid {
  double x = 0;  // pixels, origin at the image's top-left corner
  double y = 0;
};

enum class DistortionMode : std::uint8_t {
  kNone,   // tetra3 distortion=None: no distortion term; FOV refined from star separations
  kFixed,  // tetra3 scalar distortion k (default 0): undistort, then fit k and FOV
  kRange,  // tetra3 (min, max) tuple: search pre-undistorted copies every 0.1
};

enum class PatternOrder : std::uint8_t {
  kLexicographic,  // ESA: itertools.combinations over the pattern_checking_stars brightest
  kBreadthFirst,   // cedar-solve: colex order over all candidate centroids
};

enum class KeyOrder : std::uint8_t {
  kSortedUnique,  // ESA: sorted, de-duplicated key tuples
  kByDistance,    // cedar-solve: keys nearest the measured key first (non-decreasing keys only)
};

enum class MatchDedup : std::uint8_t {
  kImageFirst,    // ESA: unique image column, then catalog column
  kCatalogFirst,  // cedar-solve: unique catalog column, then image column
};

// Opt-in cedar-solve behaviours. Default-constructed = ESA tetra3.
struct SolverVariants {
  bool centroid_cluster_buster = false;  // thin pattern candidates, sep 0.6*width/sqrt(n)
  PatternOrder pattern_order = PatternOrder::kLexicographic;
  KeyOrder key_order = KeyOrder::kSortedUnique;
  bool reject_reflection = false;        // drop candidate rotations with det < 0
  std::uint32_t nearby_trim_factor = 1;  // keep factor*N nearby catalog stars (cedar: 2)
  double match_max_error = 0;            // > 0: max(this, db error) as the ratio tolerance
  MatchDedup match_dedup = MatchDedup::kImageFirst;
  bool distortion_total_k = false;  // fit k on raw centroids (total k) instead of residual k
  bool use_key_hash_filter = true;  // only takes effect if the database stores key hashes

  static SolverVariants esa() noexcept { return {}; }
  static SolverVariants cedar() noexcept {
    SolverVariants v;
    v.centroid_cluster_buster = true;
    v.pattern_order = PatternOrder::kBreadthFirst;
    v.key_order = KeyOrder::kByDistance;
    v.reject_reflection = true;
    v.nearby_trim_factor = 2;
    v.match_max_error = 0.002;
    v.match_dedup = MatchDedup::kCatalogFirst;
    v.distortion_total_k = true;
    return v;
  }
};

// Deterministic work limits (0 = unlimited).
struct WorkBudget {
  std::uint64_t max_image_patterns = 0;
  std::uint64_t max_slot_reads = 0;
  std::uint64_t max_candidates = 0;  // catalog patterns passing the ratio test
};

struct SolveOptions {
  double fov_estimate = std::numeric_limits<double>::quiet_NaN();   // radians, NaN = unknown
  double fov_max_error = std::numeric_limits<double>::quiet_NaN();  // radians, NaN = none
  std::uint32_t pattern_checking_stars = 8;
  double match_radius = 0.01;     // fraction of image width
  double match_threshold = 1e-3;  // divided by the database's pattern count
  DistortionMode distortion_mode = DistortionMode::kFixed;
  double distortion = 0;      // kFixed: k
  double distortion_min = 0;  // kRange
  double distortion_max = 0;  // kRange
  WorkBudget budget{};
  const Clock* clock = nullptr;               // optional; enables timing and the deadline
  std::int64_t timeout_ns = 0;                // > 0 with a clock: wall-clock deadline (kTimeout)
  const std::atomic<bool>* cancel = nullptr;  // checked once per image pattern (kCancelled)
  SolverVariants variants{};
};

struct SolveMatch {
  std::uint32_t centroid_index;  // into the solve() input
  std::uint32_t star_index;      // database star table row
};

struct WorkStats {
  std::uint64_t image_patterns = 0;
  std::uint64_t keys = 0;
  std::uint64_t slot_reads = 0;
  std::uint64_t candidates = 0;        // passed the edge-ratio test
  std::uint64_t skipped_patterns = 0;  // needed more than max_keys_per_pattern keys
};

struct SolveResult {
  Status status = Status::kNoMatch;
  double ra_deg = 0, dec_deg = 0, roll_deg = 0;                  // tetra3 conventions
  double fov = 0;                                                // horizontal FOV, radians
  double distortion = std::numeric_limits<double>::quiet_NaN();  // NaN when mode kNone
  double rmse_arcsec = 0, p90e_arcsec = 0, maxe_arcsec = 0;
  double probability = 0;  // false-match probability times the pattern count (tetra3 "Prob")
  std::uint32_t num_matches = 0;
  Mat3 rotation_tetra3{};  // tetra3's rotation matrix (catalog -> tetra3 camera frame)
  Quat q_cam_icrs{};       // our convention; only meaningful when the rotation is proper
  bool proper_rotation = true;
  std::span<const SolveMatch> matches;      // ordered as tetra3 orders matched_stars
  std::uint32_t pattern[4] = {0, 0, 0, 0};  // centroid indices of the matched image pattern
  WorkStats stats{};
  std::int64_t solve_ns = 0;  // when a clock was given
};

// Creation-time capacities.
struct SolverLimits {
  std::uint32_t max_centroids = 0;            // 0: database verification_stars_per_fov
  std::uint32_t max_keys_per_pattern = 1024;  // key tuples per image pattern
  std::uint32_t max_distortion_steps = 32;    // kRange pre-undistorted copies
  std::uint32_t max_nearby_trim_factor = 2;
};

class LostInSpaceSolver {
 public:
  // Allocates the workspace from `arena` (O(capacity)). Errors: kArenaExhausted,
  // kInvalidArgument (unusable database parameters).
  static Result<LostInSpaceSolver> create(const Database& db, const SolverLimits& limits,
                                          Arena& arena) noexcept;
  static std::size_t workspace_bytes(const Database& db, const SolverLimits& limits) noexcept;

  LostInSpaceSolver() noexcept = default;

  // Solves from centroids (brightest first). Never allocates. Statuses in `out.status`:
  // kOk, kNoMatch, kTooFewStars, kWorkBudgetExhausted, kTimeout, kCancelled,
  // kInvalidArgument (bad image size or options), kCapacityExceeded (option exceeds a limit).
  // The return value equals out.status. Worst case: see the file comment.
  Status solve(std::span<const Centroid> centroids, std::uint32_t width, std::uint32_t height,
               const SolveOptions& options, SolveResult& out) noexcept;

  const Database& database() const noexcept { return *db_; }

 private:
  struct Workspace;
  const Database* db_ = nullptr;
  SolverLimits limits_{};
  Workspace* ws_ = nullptr;
};

}  // namespace st
