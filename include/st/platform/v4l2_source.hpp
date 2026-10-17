// SPDX-License-Identifier: Apache-2.0
#pragma once

// Video4Linux2 camera source (Linux; elsewhere open() returns kUnsupported). Part of
// st_platform. Uses memory-mapped capture buffers (V4L2_MEMORY_MMAP) set up in open(); acquire()
// waits up to the timeout with poll(), dequeues a buffer, copies it out and re-queues it.
// Supported pixel formats: GREY (mono8) and Y16 / Y10 / Y12 (mono16, LSB-aligned).
// Timestamps are the driver's buffer timestamps (CLOCK_MONOTONIC on modern drivers).

#include <cstdint>

#include "st/core/result.hpp"
#include "st/pipeline/frame_source.hpp"

namespace st {

struct V4L2Options {
  std::uint32_t width = 0;  // 0: keep the driver's current size
  std::uint32_t height = 0;
  PixelFormat format = PixelFormat::kMono16;
  std::uint32_t buffers = 4;  // 2..8
};

class V4L2Source final : public FrameSource {
 public:
  static constexpr std::uint32_t kMaxBuffers = 8;

  V4L2Source() noexcept = default;
  V4L2Source(const V4L2Source&) = delete;
  V4L2Source& operator=(const V4L2Source&) = delete;
  V4L2Source(V4L2Source&& o) noexcept;
  V4L2Source& operator=(V4L2Source&& o) noexcept;
  ~V4L2Source();

  // Opens the device, sets the format, maps buffers and starts streaming.
  // Errors: kIoError (no such device / ioctl failure), kUnsupported (format, or not Linux),
  // kInvalidArgument.
  static Result<V4L2Source> open(const char* device, const V4L2Options& options) noexcept;

  [[nodiscard]] FrameFormat format() const noexcept override { return fmt_; }
  // Worst case: timeout_ns of waiting plus an O(W*H) copy.
  Status acquire(std::span<std::byte> dst, std::int64_t timeout_ns,
                 FrameInfo& info) noexcept override;

 private:
  void close() noexcept;
  int fd_ = -1;
  FrameFormat fmt_{};
  std::uint32_t bytesperline_ = 0;
  std::uint32_t nbuf_ = 0;
  void* buf_[kMaxBuffers] = {};
  std::size_t len_[kMaxBuffers] = {};
};

}  // namespace st
