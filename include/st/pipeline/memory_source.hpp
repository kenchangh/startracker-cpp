// SPDX-License-Identifier: Apache-2.0
#pragma once

// Frame source over frames already in memory (tests, benchmarks, replay from a mapped file).
// No heap; acquire() is a memcpy: O(W*H).

#include <cstring>
#include <span>

#include "st/pipeline/frame_source.hpp"

namespace st {

class MemoryFrameSource final : public FrameSource {
 public:
  // `pixels` holds `count` tightly packed frames of `fmt`; `t_capture_ns` (optional) holds one
  // timestamp per frame, otherwise frame i is stamped i * period_ns. With `loop`, the sequence
  // repeats forever and timestamps keep increasing.
  MemoryFrameSource(FrameFormat fmt, std::span<const std::byte> pixels,
                    std::span<const std::int64_t> t_capture_ns, std::int64_t period_ns,
                    bool loop = false) noexcept
      : fmt_(fmt), pixels_(pixels), t_(t_capture_ns), period_(period_ns), loop_(loop) {
    count_ = fmt.bytes() == 0 ? 0 : pixels.size() / fmt.bytes();
  }

  [[nodiscard]] FrameFormat format() const noexcept override { return fmt_; }

  Status acquire(std::span<std::byte> dst, std::int64_t /*timeout_ns*/,
                 FrameInfo& info) noexcept override {
    if (count_ == 0) return Status::kCancelled;
    if (next_ >= count_ * (lap_ + 1)) {
      if (!loop_) return Status::kCancelled;
      ++lap_;
    }
    const std::size_t n = fmt_.bytes();
    if (dst.size() < n) return Status::kInvalidArgument;
    const std::size_t i = next_ - count_ * lap_;
    std::memcpy(dst.data(), pixels_.data() + i * n, n);
    const std::int64_t span_ns =
        t_.size() >= count_ && count_ > 1
            ? t_[count_ - 1] - t_[0] +
                  (t_[count_ - 1] - t_[0]) / static_cast<std::int64_t>(count_ - 1)
            : static_cast<std::int64_t>(count_) * period_;
    const std::int64_t base = t_.size() >= count_ ? t_[i] : static_cast<std::int64_t>(i) * period_;
    info.t_capture_ns = base + static_cast<std::int64_t>(lap_) * span_ns;
    info.source_seq = next_;
    ++next_;
    return Status::kOk;
  }

  [[nodiscard]] std::size_t frame_count() const noexcept { return count_; }

 private:
  FrameFormat fmt_;
  std::span<const std::byte> pixels_;
  std::span<const std::int64_t> t_;
  std::int64_t period_ = 0;
  bool loop_ = false;
  std::size_t count_ = 0;
  std::size_t next_ = 0;
  std::size_t lap_ = 0;
};

}  // namespace st
