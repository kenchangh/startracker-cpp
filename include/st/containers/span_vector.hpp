// SPDX-License-Identifier: Apache-2.0
#pragma once

// SpanVector<T>: vector over caller-provided storage (usually from an Arena), with the
// capacity fixed at run time. Non-owning; never allocates. Elements of the backing span are
// already constructed (Arena value-initializes them), so T must be trivially destructible.

#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>

#include "st/core/assert.hpp"

namespace st {

template <class T>
class SpanVector {
  static_assert(std::is_trivially_destructible_v<T>,
                "SpanVector: T must be trivially destructible");
  static_assert(std::is_nothrow_copy_assignable_v<T>, "SpanVector: T must be copy-assignable");

 public:
  using value_type = T;
  using size_type = std::size_t;
  using iterator = T*;
  using const_iterator = const T*;

  constexpr SpanVector() noexcept = default;
  explicit constexpr SpanVector(std::span<T> storage) noexcept : buf_(storage) {}

  // O(1). Returns false (and does nothing) when full.
  [[nodiscard]] bool push_back(const T& v) noexcept {
    if (size_ == buf_.size()) return false;
    buf_[size_++] = v;
    return true;
  }
  template <class... Args>
  [[nodiscard]] bool emplace_back(Args&&... args) noexcept {
    if (size_ == buf_.size()) return false;
    buf_[size_++] = T{std::forward<Args>(args)...};
    return true;
  }
  // Precondition: !empty() (asserted). O(1).
  void pop_back() noexcept {
    ST_ASSERT(size_ > 0);
    if (size_ > 0) --size_;
  }
  // O(1); stale elements stay in the backing storage.
  void clear() noexcept { size_ = 0; }

  // Grows with value-initialized elements or shrinks. False if n > capacity(). O(|n - size|).
  [[nodiscard]] bool resize(size_type n) noexcept {
    if (n > buf_.size()) return false;
    for (size_type i = size_; i < n; ++i) buf_[i] = T{};
    size_ = n;
    return true;
  }
  // Removes element i by moving the last element into its place. Order is not kept. O(1).
  void erase_unordered(size_type i) noexcept {
    ST_ASSERT(i < size_);
    if (i >= size_) return;
    buf_[i] = buf_[size_ - 1];
    --size_;
  }

  // Bounds-checked under ST_ASSERT. O(1).
  [[nodiscard]] T& operator[](size_type i) noexcept {
    ST_ASSERT(i < size_);
    return buf_.data()[i];
  }
  [[nodiscard]] const T& operator[](size_type i) const noexcept {
    ST_ASSERT(i < size_);
    return buf_.data()[i];
  }
  [[nodiscard]] T& back() noexcept {
    ST_ASSERT(size_ > 0);
    return buf_.data()[size_ - 1];
  }

  [[nodiscard]] T* data() noexcept { return buf_.data(); }
  [[nodiscard]] const T* data() const noexcept { return buf_.data(); }
  [[nodiscard]] iterator begin() noexcept { return buf_.data(); }
  [[nodiscard]] iterator end() noexcept { return buf_.data() + size_; }
  [[nodiscard]] const_iterator begin() const noexcept { return buf_.data(); }
  [[nodiscard]] const_iterator end() const noexcept { return buf_.data() + size_; }
  [[nodiscard]] std::span<T> span() noexcept { return buf_.first(size_); }
  [[nodiscard]] std::span<const T> span() const noexcept { return buf_.first(size_); }

  [[nodiscard]] size_type size() const noexcept { return size_; }
  [[nodiscard]] size_type capacity() const noexcept { return buf_.size(); }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] bool full() const noexcept { return size_ == buf_.size(); }

 private:
  std::span<T> buf_;
  size_type size_ = 0;
};

}  // namespace st
