// SPDX-License-Identifier: Apache-2.0
#pragma once
// Builds database blobs (host tool only).
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "st/db/format.hpp"

namespace st::tools {

struct DbContents {
  db::Header header;  // parameters; counts, sizes, offsets, CRCs and probe bound are filled in
  std::vector<float> star_table;                    // num_stars * 6
  std::vector<std::uint32_t> catalog_ids;           // empty or num_stars
  std::vector<std::array<std::uint32_t, 4>> slots;  // num_slots; all-zero = empty
  std::vector<std::uint16_t> key_hash;              // empty or num_slots
  std::vector<std::uint16_t> largest_edge_half;     // empty or num_slots
};

// Serializes, computes the probe bound and checksums, and re-opens the result with full
// validation. Throws std::runtime_error on failure.
std::vector<std::byte> build_blob(DbContents c);

void write_file(const std::string& path, const std::vector<std::byte>& bytes);
std::vector<std::byte> read_file(const std::string& path);

}  // namespace st::tools
