// SPDX-License-Identifier: Apache-2.0
#pragma once

// Frame sources feed the pipeline. Implementations (replay file, V4L2 camera, test sources)
// copy one frame into a caller-provided buffer; acquire() must not allocate and must have a
// bounded wait (its timeout).

#include <cstddef>
#include <cstdint>
#include <span>

#include "st/centroid/centroider.hpp"
#include "st/core/status.hpp"

namespace st {

struct FrameFormat {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  PixelFormat format = PixelFormat::kMono16;
  [[nodiscard]] std::size_t bytes() const noexcept {
    return std::size_t{width} * height * (format == PixelFormat::kMono8 ? 1u : 2u);
  }
};

struct FrameInfo {
  std::int64_t t_capture_ns = 0;  // sensor / file timestamp (source clock)
  std::uint64_t source_seq = 0;   // source frame counter (gaps = frames dropped upstream)
};

class FrameSource {
 public:
  [[nodiscard]] virtual FrameFormat format() const noexcept = 0;
  // Copies the next frame (format().bytes() bytes, tightly packed) into `dst`.
  // Returns kOk, kTimeout (no frame within timeout_ns; try again), kCancelled (end of
  // stream), or kIoError.
  virtual Status acquire(std::span<std::byte> dst, std::int64_t timeout_ns,
                         FrameInfo& info) noexcept = 0;

 protected:
  FrameSource() = default;
  FrameSource(const FrameSource&) = default;
  FrameSource& operator=(const FrameSource&) = default;
  ~FrameSource() = default;
};

}  // namespace st
