// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>

namespace st {

// Monotonic time source in nanoseconds. Injected wherever time is read so that deadlines
// and latency measurements are testable and reproducible.
//
// The destructor is protected and non-virtual on purpose: a virtual destructor would emit a
// deleting destructor that references operator delete, which the core must not reference.
// Clocks are never deleted through a Clock pointer.
class Clock {
 public:
  // Worst case: O(1) (one clock read).
  [[nodiscard]] virtual std::int64_t now_ns() const noexcept = 0;

 protected:
  Clock() = default;
  Clock(const Clock&) = default;
  Clock& operator=(const Clock&) = default;
  ~Clock() = default;
};

// std::chrono::steady_clock. Worst case: O(1) (one vDSO/syscall clock read).
class SteadyClock final : public Clock {
 public:
  [[nodiscard]] std::int64_t now_ns() const noexcept override {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
};

// Manually advanced clock for deterministic tests. Not thread-safe.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(std::int64_t start_ns = 0) noexcept : now_(start_ns) {}
  [[nodiscard]] std::int64_t now_ns() const noexcept override { return now_; }
  void set_ns(std::int64_t t) noexcept { now_ = t; }
  void advance_ns(std::int64_t dt) noexcept { now_ += dt; }

 private:
  std::int64_t now_;
};

}  // namespace st
