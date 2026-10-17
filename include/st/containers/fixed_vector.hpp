// SPDX-License-Identifier: Apache-2.0
#pragma once

// FixedVector<T, N>: vector with compile-time capacity N and inline storage. No heap.
// Inserting into a full vector returns false and leaves the vector unchanged.

#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

#include "st/core/assert.hpp"

namespace st {

template <class T, std::size_t N>
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init): storage_ is raw storage.
class FixedVector {
  static_assert(N > 0, "FixedVector capacity must be > 0");
  static_assert(std::is_nothrow_destructible_v<T>, "FixedVector: T must be nothrow destructible");

 public:
  using value_type = T;
  using size_type = std::size_t;
  using iterator = T*;
  using const_iterator = const T*;

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init): storage_ is raw storage.
  FixedVector() noexcept = default;

  // O(size) copies/moves; O(size) destruction (O(1) for trivially destructible T).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  FixedVector(const FixedVector& o) noexcept(std::is_nothrow_copy_constructible_v<T>) {
    std::uninitialized_copy_n(o.data(), o.size_, data());
    size_ = o.size_;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  FixedVector(FixedVector&& o) noexcept(std::is_nothrow_move_constructible_v<T>) {
    std::uninitialized_move_n(o.data(), o.size_, data());
    size_ = o.size_;
  }
  FixedVector& operator=(const FixedVector& o) noexcept(std::is_nothrow_copy_constructible_v<T>) {
    if (this != &o) {
      clear();
      std::uninitialized_copy_n(o.data(), o.size_, data());
      size_ = o.size_;
    }
    return *this;
  }
  FixedVector& operator=(FixedVector&& o) noexcept(std::is_nothrow_move_constructible_v<T>) {
    if (this != &o) {
      clear();
      std::uninitialized_move_n(o.data(), o.size_, data());
      size_ = o.size_;
    }
    return *this;
  }
  ~FixedVector() { clear(); }

  // O(1). Returns false (and does nothing) when full.
  [[nodiscard]] bool push_back(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
    return emplace_back(v);
  }
  [[nodiscard]] bool push_back(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>) {
    return emplace_back(std::move(v));
  }
  template <class... Args>
  [[nodiscard]] bool emplace_back(Args&&... args) noexcept(
      std::is_nothrow_constructible_v<T, Args...>) {
    if (size_ == N) return false;
    std::construct_at(data() + size_, std::forward<Args>(args)...);
    ++size_;
    return true;
  }

  // Precondition: !empty() (asserted). O(1).
  void pop_back() noexcept {
    ST_ASSERT(size_ > 0);
    if (size_ == 0) return;
    --size_;
    std::destroy_at(data() + size_);
  }

  // O(1) for trivially destructible T, else O(size).
  void clear() noexcept {
    std::destroy_n(data(), size_);
    size_ = 0;
  }

  // Grows with value-initialized elements or shrinks. Returns false if n > N. O(|n - size|).
  [[nodiscard]] bool resize(size_type n) noexcept(std::is_nothrow_default_constructible_v<T>) {
    if (n > N) return false;
    if (n < size_) {
      std::destroy_n(data() + n, size_ - n);
    } else if (n > size_) {
      std::uninitialized_value_construct_n(data() + size_, n - size_);
    }
    size_ = n;
    return true;
  }

  // Removes element i by moving the last element into its place. Order is not kept.
  // Precondition: i < size() (asserted). O(1).
  void erase_unordered(size_type i) noexcept(std::is_nothrow_move_assignable_v<T>) {
    ST_ASSERT(i < size_);
    if (i >= size_) return;
    if (i != size_ - 1) data()[i] = std::move(data()[size_ - 1]);
    pop_back();
  }

  // Bounds-checked under ST_ASSERT. O(1).
  [[nodiscard]] T& operator[](size_type i) noexcept {
    ST_ASSERT(i < size_);
    return data()[i];
  }
  [[nodiscard]] const T& operator[](size_type i) const noexcept {
    ST_ASSERT(i < size_);
    return data()[i];
  }
  [[nodiscard]] T& front() noexcept { return (*this)[0]; }
  [[nodiscard]] const T& front() const noexcept { return (*this)[0]; }
  [[nodiscard]] T& back() noexcept {
    ST_ASSERT(size_ > 0);
    return data()[size_ - 1];
  }
  [[nodiscard]] const T& back() const noexcept {
    ST_ASSERT(size_ > 0);
    return data()[size_ - 1];
  }

  [[nodiscard]] T* data() noexcept { return std::launder(reinterpret_cast<T*>(storage_)); }
  [[nodiscard]] const T* data() const noexcept {
    return std::launder(reinterpret_cast<const T*>(storage_));
  }
  [[nodiscard]] iterator begin() noexcept { return data(); }
  [[nodiscard]] iterator end() noexcept { return data() + size_; }
  [[nodiscard]] const_iterator begin() const noexcept { return data(); }
  [[nodiscard]] const_iterator end() const noexcept { return data() + size_; }

  [[nodiscard]] std::span<T> span() noexcept { return {data(), size_}; }
  [[nodiscard]] std::span<const T> span() const noexcept { return {data(), size_}; }

  [[nodiscard]] size_type size() const noexcept { return size_; }
  [[nodiscard]] static constexpr size_type capacity() noexcept { return N; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] bool full() const noexcept { return size_ == N; }

 private:
  // Left uninitialized on purpose: construction stays O(1) and elements are constructed in
  // place. NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init,modernize-avoid-c-arrays)
  alignas(T) std::byte storage_[N * sizeof(T)];
  size_type size_ = 0;
};

}  // namespace st
