// SPDX-License-Identifier: Apache-2.0
#include "st/platform/replay_source.hpp"

#include <cstring>
#include <ctime>

namespace st {
namespace {

template <class T>
T load(const std::byte* p) noexcept {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;  // the project targets little-endian hosts (checked by the db loader too)
}

void sleep_ns(std::int64_t ns) noexcept {
  if (ns <= 0) return;
  timespec ts{static_cast<time_t>(ns / 1'000'000'000), static_cast<long>(ns % 1'000'000'000)};
  while (::nanosleep(&ts, &ts) != 0) {
  }
}

}  // namespace

Result<ReplaySource> ReplaySource::open(const char* path, const ReplayOptions& options) noexcept {
  if (!(options.speed > 0)) return Status::kInvalidArgument;
  ST_TRY_ASSIGN(auto file, MappedFile::open(path));
  const auto b = file.bytes();
  if (b.size() < kReplayHeaderBytes || std::memcmp(b.data(), "STRPLAY1", 8) != 0)
    return Status::kCorruptData;
  if (load<std::uint32_t>(b.data() + 8) != 1) return Status::kVersionMismatch;
  ReplaySource s;
  s.fmt_.width = load<std::uint32_t>(b.data() + 12);
  s.fmt_.height = load<std::uint32_t>(b.data() + 16);
  const auto pf = load<std::uint32_t>(b.data() + 20);
  if (pf > 1 || s.fmt_.width == 0 || s.fmt_.height == 0 || s.fmt_.width > 65536 ||
      s.fmt_.height > 65536)
    return Status::kCorruptData;
  s.fmt_.format = pf == 0 ? PixelFormat::kMono8 : PixelFormat::kMono16;
  s.count_ = load<std::uint64_t>(b.data() + 24);
  s.stride_ = load<std::uint64_t>(b.data() + 32);
  if (s.count_ == 0 || s.stride_ != kReplayRecordHeaderBytes + s.fmt_.bytes() ||
      (b.size() - kReplayHeaderBytes) / s.stride_ < s.count_) {
    return Status::kCorruptData;
  }
  s.t_first_ = load<std::int64_t>(b.data() + kReplayHeaderBytes);
  const std::int64_t t_last =
      load<std::int64_t>(b.data() + kReplayHeaderBytes + (s.count_ - 1) * s.stride_);
  s.lap_ns_ = s.count_ > 1 ? (t_last - s.t_first_) +
                                 (t_last - s.t_first_) / static_cast<std::int64_t>(s.count_ - 1)
                           : 0;
  s.opt_ = options;
  s.file_ = static_cast<MappedFile&&>(file);
  return s;
}

Status ReplaySource::acquire(std::span<std::byte> dst, std::int64_t timeout_ns,
                             FrameInfo& info) noexcept {
  if (count_ == 0) return Status::kNotInitialized;
  if (next_ >= count_) {
    if (!opt_.loop) return Status::kCancelled;
    next_ = 0;
    ++lap_;
  }
  const std::size_t n = fmt_.bytes();
  if (dst.size() < n) return Status::kInvalidArgument;
  const std::byte* rec = file_.bytes().data() + kReplayHeaderBytes + next_ * stride_;
  const std::int64_t t = load<std::int64_t>(rec) + static_cast<std::int64_t>(lap_) * lap_ns_;
  if (opt_.pace_clock != nullptr) {
    const std::int64_t now = opt_.pace_clock->now_ns();
    if (!started_) {
      started_ = true;
      start_ns_ = now;
    }
    const std::int64_t due =
        start_ns_ + static_cast<std::int64_t>(static_cast<double>(t - t_first_) / opt_.speed);
    if (due - now > timeout_ns) {
      sleep_ns(timeout_ns);
      return Status::kTimeout;
    }
    sleep_ns(due - now);
  }
  std::memcpy(dst.data(), rec + kReplayRecordHeaderBytes, n);
  info.t_capture_ns = t;
  info.source_seq = load<std::uint64_t>(rec + 8) + lap_ * count_;
  ++next_;
  return Status::kOk;
}

}  // namespace st
