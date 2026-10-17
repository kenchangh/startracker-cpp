// SPDX-License-Identifier: Apache-2.0
#pragma once

// Result<T>: holds either a T or a non-OK Status (expected-style, no exceptions).
// std::expected is C++23 and missing from GCC 11, so we provide our own.

#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "st/core/assert.hpp"
#include "st/core/status.hpp"

namespace st {
namespace detail {

// Trivially copyable T: everything defaulted, so Result<T> stays trivially copyable and is
// returned in registers.
template <class T, bool = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>>
struct ResultStorage {
  union {
    unsigned char empty_;
    T value_;
  };
  Status status_;

  constexpr explicit ResultStorage(Status s) noexcept : empty_{}, status_(s) {}
  template <class... Args>
  constexpr explicit ResultStorage(std::in_place_t, Args&&... args) noexcept(
      std::is_nothrow_constructible_v<T, Args...>)
      : value_(std::forward<Args>(args)...), status_(Status::kOk) {}
};

// Non-trivial T: copies, moves and destroys the active value by hand.
template <class T>
struct ResultStorage<T, false> {
  union {
    unsigned char empty_;
    T value_;
  };
  Status status_;

  constexpr explicit ResultStorage(Status s) noexcept : empty_{}, status_(s) {}
  template <class... Args>
  constexpr explicit ResultStorage(std::in_place_t, Args&&... args) noexcept(
      std::is_nothrow_constructible_v<T, Args...>)
      : value_(std::forward<Args>(args)...), status_(Status::kOk) {}

  // The union member is constructed in the body only when there is a value.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  ResultStorage(const ResultStorage& o) noexcept(std::is_nothrow_copy_constructible_v<T>)
      : status_(o.status_) {
    if (status_ == Status::kOk) std::construct_at(std::addressof(value_), o.value_);
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  ResultStorage(ResultStorage&& o) noexcept(std::is_nothrow_move_constructible_v<T>)
      : status_(o.status_) {
    if (status_ == Status::kOk) std::construct_at(std::addressof(value_), std::move(o.value_));
  }
  ResultStorage& operator=(const ResultStorage& o) noexcept(
      std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_assignable_v<T>) {
    if (this == &o) return *this;
    if (status_ == Status::kOk && o.status_ == Status::kOk) {
      value_ = o.value_;
    } else {
      reset();
      if (o.status_ == Status::kOk) std::construct_at(std::addressof(value_), o.value_);
      status_ = o.status_;
    }
    return *this;
  }
  ResultStorage& operator=(ResultStorage&& o) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                                       std::is_nothrow_move_assignable_v<T>) {
    if (this == &o) return *this;
    if (status_ == Status::kOk && o.status_ == Status::kOk) {
      value_ = std::move(o.value_);
    } else {
      reset();
      if (o.status_ == Status::kOk) {
        std::construct_at(std::addressof(value_), std::move(o.value_));
      }
      status_ = o.status_;
    }
    return *this;
  }
  ~ResultStorage() { reset(); }

 private:
  void reset() noexcept {
    if (status_ == Status::kOk) std::destroy_at(std::addressof(value_));
  }
};

}  // namespace detail

// All operations are O(1) plus the cost of copying/moving T.
template <class T>
class [[nodiscard]] Result : private detail::ResultStorage<T> {
  static_assert(!std::is_reference_v<T>, "Result<T&> is not supported; use a pointer");
  static_assert(!std::is_same_v<std::remove_cv_t<T>, Status>,
                "Result<Status> is meaningless; return Status directly");
  using Base = detail::ResultStorage<T>;

 public:
  using value_type = T;

  // Implicit from a value.
  constexpr Result(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>)  // NOLINT
      : Base(std::in_place, v) {}
  constexpr Result(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>)  // NOLINT
      : Base(std::in_place, std::move(v)) {}
  template <class... Args>
  constexpr explicit Result(std::in_place_t,
                            Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
      : Base(std::in_place, std::forward<Args>(args)...) {}

  // Implicit from an error. Passing kOk is a bug: it asserts, and without asserts it is
  // turned into kInvalidArgument so no uninitialized value can ever be read.
  constexpr Result(Status s) noexcept  // NOLINT
      : Base(s == Status::kOk ? Status::kInvalidArgument : s) {
    ST_ASSERT(s != Status::kOk);
  }

  [[nodiscard]] constexpr bool ok() const noexcept { return this->status_ == Status::kOk; }
  [[nodiscard]] constexpr Status status() const noexcept { return this->status_; }

  // Precondition: ok() (asserted).
  [[nodiscard]] constexpr T& value() & noexcept {
    ST_ASSERT(ok());
    return this->value_;
  }
  [[nodiscard]] constexpr const T& value() const& noexcept {
    ST_ASSERT(ok());
    return this->value_;
  }
  [[nodiscard]] constexpr T&& value() && noexcept {
    ST_ASSERT(ok());
    return std::move(this->value_);
  }

  [[nodiscard]] constexpr T value_or(T fallback) const& { return ok() ? this->value_ : fallback; }
  [[nodiscard]] constexpr T value_or(T fallback) && {
    return ok() ? std::move(this->value_) : fallback;
  }
};

namespace detail {
constexpr Status status_of(Status s) noexcept { return s; }
template <class T>
constexpr Status status_of(const Result<T>& r) noexcept {
  return r.status();
}
}  // namespace detail

}  // namespace st

#define ST_CONCAT_INNER_(a, b) a##b
#define ST_CONCAT_(a, b) ST_CONCAT_INNER_(a, b)

// Returns the error from the enclosing function if `expr` (a Status or Result) failed.
#define ST_TRY(expr)                                                       \
  do {                                                                     \
    if (const ::st::Status st_try_status_ = ::st::detail::status_of(expr); \
        st_try_status_ != ::st::Status::kOk) {                             \
      return st_try_status_;                                               \
    }                                                                      \
  } while (false)

// `ST_TRY_ASSIGN(auto x, f());` or `ST_TRY_ASSIGN(x, f());`: on success assigns the value,
// otherwise returns the error from the enclosing function.
#define ST_TRY_ASSIGN(lhs, expr) ST_TRY_ASSIGN_IMPL_(ST_CONCAT_(st_try_r_, __LINE__), lhs, expr)
// `lhs` may be a declaration (`auto x`), so it cannot be parenthesized.
// NOLINTBEGIN(bugprone-macro-parentheses)
#define ST_TRY_ASSIGN_IMPL_(tmp, lhs, expr) \
  auto tmp = (expr);                        \
  if (!tmp.ok()) return tmp.status();       \
  lhs = std::move(tmp).value()
// NOLINTEND(bugprone-macro-parentheses)
