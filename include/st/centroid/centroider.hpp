// SPDX-License-Identifier: Apache-2.0
#pragma once

// Star centroider. The full-frame path reproduces ESA tetra3's get_centroids_from_image
// (tetra3/tetra3.py l.1796-2135 @ f9fa2eb; Apache-2.0, Copyright 2019 the European Space
// Agency) for the modes listed below, including its float32 arithmetic, so the same image gives
// the same spots. Written from tetra3 and general published techniques only (no code from
// other centroiders).
//
// Pixel coordinates: origin at the top-left corner of the image, pixel centres at +0.5.
//
// All buffers come from an Arena at create(); extract() never allocates. Worst case for one
// full frame of W x H pixels: O(W*H) for each of conversion, two box-filter passes, threshold,
// opening and labelling, plus O(sum of accepted spot bounding boxes) for statistics and
// O(B log B) to sort B accepted spots. Global-median modes add an O(W*H) selection.

#include <cstdint>
#include <span>

#include "st/containers/bitset.hpp"
#include "st/core/arena.hpp"
#include "st/core/result.hpp"

namespace st {

enum class PixelFormat : std::uint8_t {
  kMono8,   // uint8 per pixel
  kMono16,  // uint16 per pixel; 10/12/14-bit sensors LSB-aligned in 16 bits
};

struct ImageView {
  const void* data = nullptr;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::size_t stride_bytes = 0;  // 0 = tightly packed
  PixelFormat format = PixelFormat::kMono16;
};

enum class BackgroundMode : std::uint8_t {
  kNone,
  kLocalMean,  // tetra3 default: filter_size x filter_size box mean, reflect borders
  kGlobalMean,
  kGlobalMedian,
};

enum class SigmaMode : std::uint8_t {
  kGlobalRootSquare,  // tetra3 default: sqrt(mean(residual^2))
  kGlobalMedianAbs,   // 1.48 * median(|residual|)
  kLocalRootSquare,   // sqrt(box mean of residual^2), per pixel
};

struct CentroiderConfig {
  // Capacity (largest frame this instance can process).
  std::uint32_t max_width = 0;
  std::uint32_t max_height = 0;
  std::uint32_t max_blobs = 4096;  // connected components per frame (excess -> status)

  // tetra3 parameters (defaults are tetra3's).
  BackgroundMode background = BackgroundMode::kLocalMean;
  std::uint32_t filter_size = 25;  // odd
  SigmaMode sigma_mode = SigmaMode::kGlobalRootSquare;
  float sigma = 2.0f;
  float threshold = 0.0f;      // > 0: fixed threshold on the residual (tetra3 image_th)
  bool binary_open = true;     // 3x3 cross opening
  std::uint32_t min_area = 5;  // 0 = off
  std::uint32_t max_area = 100;
  double min_sum = 0;  // 0 = off
  double max_sum = 0;
  double max_axis_ratio = 0;          // 0 = off; elongated spots (trails) above this are rejected
  std::uint32_t max_returned = 0;     // 0 = all
  std::uint32_t centroid_window = 0;  // > 0: recompute centroids in a square window

  // Hot pixels are replaced by the mean of their non-hot 4-neighbours before processing.
  bool use_hot_pixel_map = false;
};

struct Spot {
  double x = 0, y = 0;  // centroid, pixels
  double sum = 0;       // background-subtracted flux
  std::uint32_t area = 0;
  double m2xx = 0, m2yy = 0, m2xy = 0;  // second moments
  double axis_ratio = 0;                // major / minor axis
};

struct CentroidStats {
  std::uint32_t components = 0;  // labelled regions
  std::uint32_t accepted = 0;
  std::uint32_t rejected = 0;
  bool blob_overflow = false;  // more than max_blobs components; extras ignored
  float threshold = 0;         // global threshold used (0 for local sigma modes)
};

// Rectangular region of interest, in pixels (clipped to the image).
struct Roi {
  std::uint32_t x0 = 0, y0 = 0, width = 0, height = 0;
};

struct RoiConfig {
  float sigma = 5.0f;  // threshold = border mean + sigma * border stddev
  std::uint32_t min_area = 2;
  std::uint32_t max_area = 400;
  double max_axis_ratio = 0;  // 0 = off
};

class Centroider {
 public:
  // Memory: about 16 bytes per pixel of the configured maximum frame plus 64 bytes per blob.
  static std::size_t workspace_bytes(const CentroiderConfig& config) noexcept;
  // Errors: kInvalidArgument (zero size, even filter, ...), kArenaExhausted.
  static Result<Centroider> create(const CentroiderConfig& config, Arena& arena) noexcept;

  Centroider() noexcept = default;
  const CentroiderConfig& config() const noexcept { return cfg_; }

  // Hot-pixel map: bit (y * max_width + x). Valid only if use_hot_pixel_map. O(1).
  BitSpan hot_pixels() noexcept { return hot_; }

  // Full-frame extraction, spots written to `out` brightest first (at most out.size()).
  // Returns the number written. Errors: kInvalidArgument (null data, frame larger than the
  // capacity, filter larger than the frame), kCapacityExceeded is reported through
  // stats.blob_overflow, not as an error.
  Result<std::uint32_t> extract(const ImageView& image, std::span<Spot> out,
                                CentroidStats* stats = nullptr) noexcept;

  // ROI extraction for tracking: for each ROI, the brightest component above a threshold from
  // the ROI border statistics. found[i] tells whether out[i] is valid. Worst case: O(sum of
  // ROI areas). Errors: kInvalidArgument (sizes), kCapacityExceeded (ROI larger than the
  // frame capacity).
  Status extract_rois(const ImageView& image, std::span<const Roi> rois, const RoiConfig& cfg,
                      std::span<Spot> out, std::span<bool> found) noexcept;

 private:
  struct Blob;
  CentroiderConfig cfg_{};
  std::span<float> img_;   // converted input, then residual
  std::span<float> work_;  // box filter output / scratch
  std::span<std::uint8_t> mask_a_, mask_b_;
  std::span<std::uint32_t> labels_;
  std::span<std::uint32_t> parent_;  // union-find over provisional labels
  std::span<std::uint32_t> remap_;   // provisional root -> final region id
  std::span<float> work2_;           // local sigma mode only
  std::span<Blob> blobs_;
  std::span<double> line_;  // filter line buffer (with borders)
  std::span<double> line_out_;
  std::span<double> pix_;  // per-spot pixel scratch
  std::span<std::uint64_t> hot_words_;
  BitSpan hot_;
};

// numpy's float32 sum: 8192-element chunks, each summed pairwise as numpy does, accumulated in
// float32 starting from zero (verified bit-exact against np.sum on 768x1024 arrays).
// Worst case: O(n), recursion depth <= 7.
float numpy_sum_f32(std::span<const float> v) noexcept;

}  // namespace st
