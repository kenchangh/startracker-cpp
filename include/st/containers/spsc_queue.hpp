// SPDX-License-Identifier: Apache-2.0
#pragma once

// SpscQueue<T>: lock-free, wait-free single-producer/single-consumer FIFO over caller-provided
// storage (a power-of-two span, usually from an Arena). Used to hand frame slots between
// pipeline stages.
//
// Indices are free-running 32-bit counters (head - tail = occupancy), so all 2^k slots are
// usable. Each side caches the other side's index to avoid cache-line ping-pong.
// Exactly one thread may call try_push and exactly one may call try_pop.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "st/core/status.hpp"

namespace st {

// Cache-line size used for padding. 64 B on Cortex-A72/A76, Carmel and x86-64.
inline constexpr std::size_t kCacheLineSize = 64;

template <class T>
class SpscQueue {
  static_assert(std::is_trivially_copyable_v<T>, "SpscQueue: T must be trivially copyable");
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                "SpscQueue needs lock-free 32-bit atomics");

 public:
  SpscQueue() noexcept = default;
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  // Binds the storage. Must be called before any push/pop, while no other thread uses the
  // queue. storage.size() must be a power of two in [2, 2^31].
  // Errors: kInvalidArgument. O(1).
  [[nodiscard]] Status init(std::span<T> storage) noexcept {
    const std::size_t n = storage.size();
    if (n < 2 || n > (std::size_t{1} << 31) || (n & (n - 1)) != 0) {
      return Status::kInvalidArgument;
    }
    buf_ = storage.data();
    mask_ = static_cast<std::uint32_t>(n - 1);
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    cached_tail_ = 0;
    cached_head_ = 0;
    return Status::kOk;
  }

  // Producer only. Returns false when full (or not initialized). Wait-free, O(1).
  [[nodiscard]] bool try_push(const T& v) noexcept {
    const std::uint32_t h = head_.load(std::memory_order_relaxed);
    const std::uint32_t cap = mask_ + 1;
    if (buf_ == nullptr) return false;
    if (h - cached_tail_ == cap) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (h - cached_tail_ == cap) return false;
    }
    buf_[h & mask_] = v;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  // Consumer only. Returns false when empty. Wait-free, O(1).
  [[nodiscard]] bool try_pop(T& out) noexcept {
    const std::uint32_t t = tail_.load(std::memory_order_relaxed);
    if (t == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (t == cached_head_) return false;
    }
    out = buf_[t & mask_];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  // Snapshot; exact only when called from a quiescent state. O(1).
  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::uint32_t h = head_.load(std::memory_order_acquire);
    const std::uint32_t t = tail_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(h - t);
  }
  [[nodiscard]] std::size_t capacity() const noexcept {
    return buf_ == nullptr ? 0 : static_cast<std::size_t>(mask_) + 1;
  }

 private:
  // Producer-owned line.
  alignas(kCacheLineSize) std::atomic<std::uint32_t> head_{0};
  std::uint32_t cached_tail_ = 0;
  // Consumer-owned line.
  alignas(kCacheLineSize) std::atomic<std::uint32_t> tail_{0};
  std::uint32_t cached_head_ = 0;
  // Read-only after init.
  alignas(kCacheLineSize) T* buf_ = nullptr;
  std::uint32_t mask_ = 0;
};

}  // namespace st
