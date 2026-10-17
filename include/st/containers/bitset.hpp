// SPDX-License-Identifier: Apache-2.0
#pragma once

// BitSpan: bit set over caller-provided 64-bit words (run-time size).
// BitSet<N>: same operations with inline storage (compile-time size).
// test/set/reset: O(1). count/find_first/find_next/fill: O(bits/64).

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "st/core/assert.hpp"

namespace st {

inline constexpr std::size_t bitset_words(std::size_t bits) noexcept { return (bits + 63) / 64; }

class BitSpan {
 public:
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  constexpr BitSpan() noexcept = default;
  // Precondition: words.size() >= bitset_words(bits) (asserted; bits is clamped otherwise).
  constexpr BitSpan(std::span<std::uint64_t> words, std::size_t bits) noexcept
      : words_(words.data()), bits_(bits) {
    ST_ASSERT(words.size() >= bitset_words(bits));
    if (words.size() < bitset_words(bits)) bits_ = words.size() * 64;
  }

  [[nodiscard]] constexpr std::size_t size() const noexcept { return bits_; }

  [[nodiscard]] constexpr bool test(std::size_t i) const noexcept {
    ST_ASSERT(i < bits_);
    return ((words_[i >> 6] >> (i & 63)) & 1u) != 0;
  }
  constexpr void set(std::size_t i) noexcept {
    ST_ASSERT(i < bits_);
    words_[i >> 6] |= std::uint64_t{1} << (i & 63);
  }
  constexpr void reset(std::size_t i) noexcept {
    ST_ASSERT(i < bits_);
    words_[i >> 6] &= ~(std::uint64_t{1} << (i & 63));
  }
  constexpr void assign(std::size_t i, bool v) noexcept {
    if (v) {
      set(i);
    } else {
      reset(i);
    }
  }

  // Sets or clears all bits. Bits past size() in the last word are always kept clear.
  constexpr void fill(bool v) noexcept {
    const std::size_t nw = bitset_words(bits_);
    for (std::size_t w = 0; w < nw; ++w) words_[w] = v ? ~std::uint64_t{0} : 0;
    if (v && nw > 0) words_[nw - 1] &= tail_mask();
  }

  [[nodiscard]] constexpr std::size_t count() const noexcept {
    std::size_t n = 0;
    const std::size_t nw = bitset_words(bits_);
    for (std::size_t w = 0; w < nw; ++w) {
      const std::uint64_t word = (w + 1 == nw) ? (words_[w] & tail_mask()) : words_[w];
      n += static_cast<std::size_t>(std::popcount(word));
    }
    return n;
  }
  [[nodiscard]] constexpr bool any() const noexcept { return find_first() != npos; }

  // Index of the first set bit >= from, or npos.
  [[nodiscard]] constexpr std::size_t find_next(std::size_t from) const noexcept {
    if (from >= bits_) return npos;
    const std::size_t nw = bitset_words(bits_);
    std::size_t w = from >> 6;
    std::uint64_t word = words_[w] & (~std::uint64_t{0} << (from & 63));
    for (;;) {
      if (w + 1 == nw) word &= tail_mask();
      if (word != 0) return (w << 6) + static_cast<std::size_t>(std::countr_zero(word));
      if (++w == nw) return npos;
      word = words_[w];
    }
  }
  [[nodiscard]] constexpr std::size_t find_first() const noexcept { return find_next(0); }

 private:
  [[nodiscard]] constexpr std::uint64_t tail_mask() const noexcept {
    const std::size_t r = bits_ & 63;
    return r == 0 ? ~std::uint64_t{0} : ((std::uint64_t{1} << r) - 1);
  }

  std::uint64_t* words_ = nullptr;
  std::size_t bits_ = 0;
};

template <std::size_t N>
class BitSet {
  static_assert(N > 0, "BitSet size must be > 0");

 public:
  static constexpr std::size_t npos = BitSpan::npos;

  [[nodiscard]] static constexpr std::size_t size() noexcept { return N; }
  [[nodiscard]] bool test(std::size_t i) const noexcept { return cview().test(i); }
  void set(std::size_t i) noexcept { view().set(i); }
  void reset(std::size_t i) noexcept { view().reset(i); }
  void assign(std::size_t i, bool v) noexcept { view().assign(i, v); }
  void fill(bool v) noexcept { view().fill(v); }
  [[nodiscard]] std::size_t count() const noexcept { return cview().count(); }
  [[nodiscard]] bool any() const noexcept { return cview().any(); }
  [[nodiscard]] std::size_t find_first() const noexcept { return cview().find_first(); }
  [[nodiscard]] std::size_t find_next(std::size_t from) const noexcept {
    return cview().find_next(from);
  }

  [[nodiscard]] BitSpan view() noexcept { return BitSpan(words_, N); }

 private:
  // BitSpan only reads through this view; the const_cast never leads to a write.
  [[nodiscard]] BitSpan cview() const noexcept {
    return BitSpan(const_cast<BitSet*>(this)->words_, N);
  }
  std::array<std::uint64_t, bitset_words(N)> words_{};
};

}  // namespace st
