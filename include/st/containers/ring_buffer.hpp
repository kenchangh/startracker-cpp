// SPDX-License-Identifier: Apache-2.0
#pragma once

// RingBuffer<T, N>: single-threaded FIFO with compile-time capacity N (a power of two) and
// inline storage. Index 0 is the oldest element. All operations are O(1).

#include <array>
#include <cstddef>
#include <type_traits>

#include "st/core/assert.hpp"

namespace st {

template <class T, std::size_t N>
class RingBuffer {
  static_assert(N > 0 && (N & (N - 1)) == 0, "RingBuffer capacity must be a power of two");
  static_assert(std::is_nothrow_default_constructible_v<T> && std::is_nothrow_copy_assignable_v<T>,
                "RingBuffer: T must be nothrow default-constructible and copy-assignable");

 public:
  using value_type = T;
  using size_type = std::size_t;

  // Returns false (and does nothing) when full.
  [[nodiscard]] bool try_push(const T& v) noexcept {
    if (size_ == N) return false;
    buf_[(head_ + size_) & kMask] = v;
    ++size_;
    return true;
  }
  // Always succeeds; when full, the oldest element is dropped. Returns true if one was dropped.
  bool push_overwrite(const T& v) noexcept {
    if (size_ == N) {
      buf_[head_] = v;
      head_ = (head_ + 1) & kMask;
      return true;
    }
    buf_[(head_ + size_) & kMask] = v;
    ++size_;
    return false;
  }
  // Precondition: !empty() (asserted).
  void pop_front() noexcept {
    ST_ASSERT(size_ > 0);
    if (size_ == 0) return;
    head_ = (head_ + 1) & kMask;
    --size_;
  }
  void clear() noexcept { head_ = size_ = 0; }

  // i = 0 is the oldest. Bounds-checked under ST_ASSERT.
  [[nodiscard]] T& operator[](size_type i) noexcept {
    ST_ASSERT(i < size_);
    return buf_[(head_ + i) & kMask];
  }
  [[nodiscard]] const T& operator[](size_type i) const noexcept {
    ST_ASSERT(i < size_);
    return buf_[(head_ + i) & kMask];
  }
  [[nodiscard]] T& front() noexcept { return (*this)[0]; }
  [[nodiscard]] const T& front() const noexcept { return (*this)[0]; }
  [[nodiscard]] T& back() noexcept {
    ST_ASSERT(size_ > 0);
    return (*this)[size_ - 1];
  }
  [[nodiscard]] const T& back() const noexcept {
    ST_ASSERT(size_ > 0);
    return (*this)[size_ - 1];
  }

  [[nodiscard]] size_type size() const noexcept { return size_; }
  [[nodiscard]] static constexpr size_type capacity() noexcept { return N; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] bool full() const noexcept { return size_ == N; }

 private:
  static constexpr size_type kMask = N - 1;
  std::array<T, N> buf_{};
  size_type head_ = 0;
  size_type size_ = 0;
};

}  // namespace st
