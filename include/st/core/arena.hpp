// SPDX-License-Identifier: Apache-2.0
#pragma once

// Bump allocator over caller-provided memory. Never touches the heap.
//
// Typical use: one *persistent* arena filled during init and then frozen, and one
// *scratch* arena released to a marker at the start of every frame or solve.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

#include "st/core/assert.hpp"
#include "st/core/result.hpp"
#include "st/core/status.hpp"

namespace st {

class Arena {
 public:
  struct Marker {
    std::size_t offset = 0;
  };

  constexpr Arena() noexcept = default;
  explicit Arena(std::span<std::byte> backing) noexcept
      : base_(backing.data()), capacity_(backing.size()) {}

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // n value-initialized T (zeroed for arithmetic types), aligned to `align`
  // (power of two, >= alignof(T)). n == 0 returns an empty span.
  // Errors: kInvalidArgument (bad alignment), kArenaExhausted (no room, size overflow, or
  // frozen arena). Worst case: O(n) for the value-initialization.
  template <class T>
  [[nodiscard]] Result<std::span<T>> allocate(std::size_t n,
                                              std::size_t align = alignof(T)) noexcept {
    auto r = allocate_uninit<T>(n, align);
    if (r.ok()) std::uninitialized_value_construct_n(r.value().data(), n);
    return r;
  }

  // As allocate(), but default-initialized: for trivial T the contents are indeterminate and
  // the cost is O(1). Use for buffers that are fully written before being read.
  template <class T>
  [[nodiscard]] Result<std::span<T>> allocate_uninit(std::size_t n,
                                                     std::size_t align = alignof(T)) noexcept {
    static_assert(std::is_trivially_destructible_v<T>,
                  "Arena never runs destructors: T must be trivially destructible");
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "Arena: T must be nothrow default constructible");
    if (align < alignof(T) || (align & (align - 1)) != 0) return Status::kInvalidArgument;
    if (n > max_count<T>()) return Status::kArenaExhausted;
    std::size_t start = 0;
    if (!bump(n * sizeof(T), align, start)) return Status::kArenaExhausted;
    if (n == 0) return std::span<T>();
    T* first = reinterpret_cast<T*>(base_ + start);
    std::uninitialized_default_construct_n(first, n);
    return std::span<T>(first, n);
  }

  // O(1).
  [[nodiscard]] Marker mark() const noexcept { return Marker{offset_}; }

  // Frees everything allocated after `m`. Spans handed out after `m` become dangling.
  // Precondition: m came from this arena and is not newer than the current offset; the
  // arena is not frozen (both asserted). O(1).
  void release(Marker m) noexcept {
    ST_ASSERT(!frozen_);
    ST_ASSERT(m.offset <= offset_);
    if (!frozen_ && m.offset <= offset_) offset_ = m.offset;
  }
  void reset() noexcept { release(Marker{0}); }

  // After freeze(), allocate() fails with kArenaExhausted (and asserts): it marks the end of
  // initialization for a persistent arena. O(1).
  void freeze() noexcept { frozen_ = true; }
  [[nodiscard]] bool frozen() const noexcept { return frozen_; }

  [[nodiscard]] std::size_t used() const noexcept { return offset_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return capacity_ - offset_; }
  // Largest used() ever seen, including padding; use it to size the backing buffer.
  [[nodiscard]] std::size_t high_water() const noexcept { return high_water_; }

 private:
  template <class T>
  static constexpr std::size_t max_count() noexcept {
    return static_cast<std::size_t>(-1) / sizeof(T);
  }

  // Reserves `bytes` at the next `align` boundary; on success sets `start` (offset from
  // base_) and returns true.
  bool bump(std::size_t bytes, std::size_t align, std::size_t& start) noexcept {
    ST_ASSERT(!frozen_);
    if (frozen_) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(base_);
    const std::uintptr_t cur = base + offset_;
    const std::uintptr_t aligned = (cur + (align - 1)) & ~static_cast<std::uintptr_t>(align - 1);
    if (aligned < cur) return false;  // address overflow
    const auto s = static_cast<std::size_t>(aligned - base);
    if (s > capacity_ || bytes > capacity_ - s) return false;
    start = s;
    offset_ = s + bytes;
    if (offset_ > high_water_) high_water_ = offset_;
    return true;
  }

  std::byte* base_ = nullptr;
  std::size_t capacity_ = 0;
  std::size_t offset_ = 0;
  std::size_t high_water_ = 0;
  bool frozen_ = false;
};

// Releases the arena to the marker taken at construction. O(1).
class ArenaScope {
 public:
  explicit ArenaScope(Arena& arena) noexcept : arena_(arena), marker_(arena.mark()) {}
  ArenaScope(const ArenaScope&) = delete;
  ArenaScope& operator=(const ArenaScope&) = delete;
  ~ArenaScope() { arena_.release(marker_); }

 private:
  Arena& arena_;
  Arena::Marker marker_;
};

}  // namespace st
