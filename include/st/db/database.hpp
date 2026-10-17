// SPDX-License-Identifier: Apache-2.0
#pragma once

// Read-only view of a database blob (see format.hpp). Never copies or allocates: the caller
// owns the bytes (mmap'd file or a buffer) and must keep them alive while the view is used.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "st/core/result.hpp"
#include "st/db/format.hpp"

namespace st {

struct StarRecord {
  float ra = 0, dec = 0;      // radians
  float x = 0, y = 0, z = 0;  // unit vector (ICRS)
  float mag = 0;
};

class Database {
 public:
  struct OpenOptions {
    bool verify_checksum = true;        // O(file size) CRC-32C over header and payload
    bool validate_indices = true;       // O(num_slots): every star index < num_stars
    bool validate_probe_bound = false;  // O(num_slots * max_probe_chain): header bound holds
  };

  // Validates and opens a blob in place. The blob must be 8-byte aligned (mmap gives page
  // alignment). Errors: kCorruptData (bad magic, sizes, offsets, indices), kVersionMismatch,
  // kChecksumMismatch, kUnsupported (big-endian host, unknown probe mode/index width),
  // kInvalidArgument (misaligned blob). Worst case: O(file size) with the default options.
  static Result<Database> open(std::span<const std::byte> blob,
                               const OpenOptions& options) noexcept;
  static Result<Database> open(std::span<const std::byte> blob) noexcept {
    return open(blob, OpenOptions{});
  }

  Database() noexcept = default;

  [[nodiscard]] const db::Header& header() const noexcept { return h_; }
  [[nodiscard]] std::uint32_t num_stars() const noexcept { return h_.num_stars; }
  [[nodiscard]] std::uint32_t num_slots() const noexcept { return h_.num_slots; }
  [[nodiscard]] std::span<const std::byte> blob() const noexcept { return blob_; }

  // Star table access. Precondition: i < num_stars() (asserted). O(1).
  [[nodiscard]] StarRecord star(std::uint32_t i) const noexcept {
    ST_ASSERT(i < h_.num_stars);
    StarRecord r;
    std::memcpy(&r, stars_ + static_cast<std::size_t>(i) * sizeof(float) * 6, sizeof(r));
    return r;
  }
  [[nodiscard]] std::array<float, 3> star_vector(std::uint32_t i) const noexcept {
    ST_ASSERT(i < h_.num_stars);
    std::array<float, 3> v{};
    std::memcpy(v.data(), stars_ + static_cast<std::size_t>(i) * sizeof(float) * 6 + 8, 12);
    return v;
  }
  [[nodiscard]] float star_magnitude(std::uint32_t i) const noexcept {
    ST_ASSERT(i < h_.num_stars);
    float m = 0;
    std::memcpy(&m, stars_ + static_cast<std::size_t>(i) * sizeof(float) * 6 + 20, 4);
    return m;
  }
  [[nodiscard]] bool has_catalog_ids() const noexcept { return ids_ != nullptr; }
  // 0 when the database has no IDs. O(1).
  [[nodiscard]] std::uint32_t catalog_id(std::uint32_t i) const noexcept {
    ST_ASSERT(i < h_.num_stars);
    if (ids_ == nullptr) return 0;
    std::uint32_t v = 0;
    std::memcpy(&v, ids_ + static_cast<std::size_t>(i) * 4, 4);
    return v;
  }

  // Reads slot `slot` into `out`; returns false for an empty slot. O(1).
  bool pattern(std::uint64_t slot, std::array<std::uint32_t, 4>& out) const noexcept {
    ST_ASSERT(slot < h_.num_slots);
    if (h_.star_index_bytes == 2) {
      std::uint16_t v[4];
      std::memcpy(v, patterns_ + slot * 8, 8);
      out = {v[0], v[1], v[2], v[3]};
    } else {
      std::memcpy(out.data(), patterns_ + slot * 16, 16);
    }
    return (out[0] | out[1] | out[2] | out[3]) != 0;
  }
  [[nodiscard]] bool has_key_hash() const noexcept { return key_hash_ != nullptr; }
  [[nodiscard]] std::uint16_t key_hash(std::uint64_t slot) const noexcept {
    ST_ASSERT(slot < h_.num_slots && key_hash_ != nullptr);
    std::uint16_t v = 0;
    std::memcpy(&v, key_hash_ + slot * 2, 2);
    return v;
  }
  [[nodiscard]] bool has_largest_edge() const noexcept { return largest_edge_ != nullptr; }
  // Largest pattern edge in milliradians (float16 in the blob). O(1).
  [[nodiscard]] float largest_edge_mrad(std::uint64_t slot) const noexcept {
    ST_ASSERT(slot < h_.num_slots && largest_edge_ != nullptr);
    std::uint16_t v = 0;
    std::memcpy(&v, largest_edge_ + slot * 2, 2);
    return db::half_to_float(v);
  }

  // Start slot for a key hash, and the c-th probe from it. O(1).
  [[nodiscard]] std::uint64_t start_slot(std::uint64_t key_hash) const noexcept;
  [[nodiscard]] std::uint64_t probe_slot(std::uint64_t start, std::uint64_t c) const noexcept;

  // Longest run of occupied slots probed from any start before an empty one (from the header).
  [[nodiscard]] std::uint32_t max_probe_chain() const noexcept { return h_.max_probe_chain; }

 private:
  db::Header h_{};
  std::span<const std::byte> blob_;
  const std::byte* stars_ = nullptr;
  const std::byte* ids_ = nullptr;
  const std::byte* patterns_ = nullptr;
  const std::byte* key_hash_ = nullptr;
  const std::byte* largest_edge_ = nullptr;
};

// tetra3's key hash: sum(key[i] * bins^i) in uint64 (wrapping). O(key.size()).
std::uint64_t pattern_key_hash(std::span<const std::uint32_t> key, std::uint32_t bins) noexcept;

// Computes the true max probe chain of a table (used by writers and deep validation).
// Worst case: O(num_slots * chain); returns kCorruptData if any chain exceeds `limit`.
Result<std::uint32_t> compute_max_probe_chain(const Database& db, std::uint32_t limit) noexcept;

}  // namespace st
