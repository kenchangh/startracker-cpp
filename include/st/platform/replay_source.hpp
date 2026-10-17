// SPDX-License-Identifier: Apache-2.0
#pragma once

// Replay of a recorded frame sequence (.strp file, memory-mapped). Part of st_platform.
//
// .strp layout (little-endian):
//   header, 64 bytes: magic "STRPLAY1", u32 version (1), u32 width, u32 height,
//                     u32 pixel_format (0 = mono8, 1 = mono16), u64 frame_count,
//                     u64 frame_stride (= 16 + frame bytes), zero padding to 64
//   frame_count records of: i64 t_capture_ns, u64 source_seq, pixels (row-major, packed)
//
// Pacing: with a clock, frames are released at their recorded timestamps (scaled by `speed`)
// relative to the first acquire(); otherwise as fast as they are requested.

#include <cstdint>

#include "st/core/clock.hpp"
#include "st/core/result.hpp"
#include "st/pipeline/frame_source.hpp"
#include "st/platform/mapped_file.hpp"

namespace st {

inline constexpr std::size_t kReplayHeaderBytes = 64;
inline constexpr std::size_t kReplayRecordHeaderBytes = 16;

struct ReplayOptions {
  bool loop = false;
  const Clock* pace_clock = nullptr;  // null: no pacing
  double speed = 1.0;                 // > 0; 2.0 plays twice as fast
};

class ReplaySource final : public FrameSource {
 public:
  ReplaySource() noexcept = default;
  // Errors: kIoError (cannot map), kCorruptData (bad header or size), kInvalidArgument.
  static Result<ReplaySource> open(const char* path, const ReplayOptions& options) noexcept;

  [[nodiscard]] FrameFormat format() const noexcept override { return fmt_; }
  // O(W*H) copy, plus at most timeout_ns of sleep when paced.
  Status acquire(std::span<std::byte> dst, std::int64_t timeout_ns,
                 FrameInfo& info) noexcept override;
  [[nodiscard]] std::uint64_t frame_count() const noexcept { return count_; }

 private:
  MappedFile file_;
  ReplayOptions opt_{};
  FrameFormat fmt_{};
  std::uint64_t count_ = 0, stride_ = 0, next_ = 0, lap_ = 0;
  std::int64_t t_first_ = 0, lap_ns_ = 0, start_ns_ = 0;
  bool started_ = false;
};

}  // namespace st
