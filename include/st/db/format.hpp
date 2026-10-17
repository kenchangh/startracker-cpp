// SPDX-License-Identifier: Apache-2.0
#pragma once

// Runtime database blob format, version 1. Flat, little-endian, checksummed, loadable in place
// (mmap or a copy). Layout:
//
//   [0, 256)          header (fields below, fixed offsets, little-endian)
//   [256, file_size)  sections, each starting at a multiple of 64 bytes
//
// Header byte offsets:
//     0 magic "STDB\r\n\x1a\n"      8 u32 format_version   12 u32 header_size (256)
//    16 u64 file_size               24 u32 header_crc32c (over the header with this field 0)
//    28 u32 payload_crc32c (over [header_size, file_size))
//    32 u32 num_stars               36 u32 num_slots       40 u32 num_patterns
//    44 u32 threshold_patterns (the "number of patterns" the match threshold is divided by;
//           tetra3 uses slots/2)
//    48 u32 max_probe_chain (max occupied slots visited from any start index before an empty one)
//    52 u16 pattern_size (4)        54 u16 pattern_bins
//    56 u8  star_index_bytes (2|4)  57 u8 probe_mode       58 u8 catalog   59 u8 reserved
//    60 u32 flags
//    64 f64 pattern_max_error       72 f64 max_fov (rad)   80 f64 min_fov (rad)
//    88 f64 star_max_magnitude      96 f64 epoch_proper_motion   104 f64 epoch_equinox
//   112 f64 range_ra[2] (rad, NaN if unused)   128 f64 range_dec[2] (rad, NaN if unused)
//   144 u16 pattern_stars_per_fov  146 u16 verification_stars_per_fov
//   148 u32 patterns_per_lattice_field (0 unless lattice generation)
//   152 section table: kSectionCount x {u64 offset, u64 size}
//   232..255 reserved, zero
//
// Sections:
//   kStarTable    f32[num_stars][6]: ra, dec (rad), x, y, z (unit vector), magnitude
//   kCatalogIds   u32[num_stars] (optional; e.g. HIP numbers)
//   kPatterns     u16|u32[num_slots][4] star indices; all-zero row = empty slot
//   kKeyHash      u16[num_slots] low 16 bits of the key hash (optional)
//   kLargestEdge  u16[num_slots] IEEE half, largest edge in milliradians (optional)

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "st/core/status.hpp"

namespace st::db {

inline constexpr std::array<char, 8> kMagic = {'S', 'T', 'D', 'B', '\r', '\n', '\x1a', '\n'};
inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::size_t kHeaderSize = 256;
inline constexpr std::size_t kSectionAlign = 64;
inline constexpr std::size_t kStarTableColumns = 6;
inline constexpr std::size_t kPatternSize = 4;

enum class Section : std::uint32_t {
  kStarTable = 0,
  kCatalogIds = 1,
  kPatterns = 2,
  kKeyHash = 3,
  kLargestEdge = 4,
};
inline constexpr std::size_t kSectionCount = 5;

enum class ProbeMode : std::uint8_t {
  kQuadraticMagic = 0,  // tetra3: index = (hash * 2654435761) % slots, probe (i + c^2) % slots
  kLinear = 1,          // cedar-solve: index = hash % slots, probe (i + c) % slots
};

enum class CatalogKind : std::uint8_t { kUnknown = 0, kBsc5 = 1, kHipMain = 2, kTycMain = 3 };

enum Flags : std::uint32_t {
  kFlagPresorted = 1u << 0,        // pattern stars ordered by distance from pattern centroid
  kFlagSimplifyPattern = 1u << 1,  // tetra3 simplify_pattern
  kFlagLatticePatterns = 1u << 2,  // cedar-solve lattice-field pattern generation
  kFlagClusterBuster = 1u << 3,    // cedar-solve cluster-buster thinning
};

struct SectionInfo {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;  // 0 = absent
};

struct Header {
  std::uint32_t format_version = kFormatVersion;
  std::uint64_t file_size = 0;
  std::uint32_t header_crc = 0;
  std::uint32_t payload_crc = 0;
  std::uint32_t num_stars = 0;
  std::uint32_t num_slots = 0;
  std::uint32_t num_patterns = 0;
  std::uint32_t threshold_patterns = 0;
  std::uint32_t max_probe_chain = 0;
  std::uint16_t pattern_size = kPatternSize;
  std::uint16_t pattern_bins = 0;
  std::uint8_t star_index_bytes = 2;
  ProbeMode probe_mode = ProbeMode::kQuadraticMagic;
  CatalogKind catalog = CatalogKind::kUnknown;
  std::uint32_t flags = 0;
  double pattern_max_error = 0;
  double max_fov = 0;
  double min_fov = 0;
  double star_max_magnitude = 0;
  double epoch_proper_motion = 0;
  double epoch_equinox = 0;
  double range_ra[2] = {0, 0};
  double range_dec[2] = {0, 0};
  std::uint16_t pattern_stars_per_fov = 0;
  std::uint16_t verification_stars_per_fov = 0;
  std::uint32_t patterns_per_lattice_field = 0;
  std::array<SectionInfo, kSectionCount> sections{};

  [[nodiscard]] const SectionInfo& section(Section s) const noexcept {
    return sections[static_cast<std::size_t>(s)];
  }
  [[nodiscard]] SectionInfo& section(Section s) noexcept {
    return sections[static_cast<std::size_t>(s)];
  }
};

// CRC-32C (Castagnoli), slice-by-8. `crc` chains calls: crc32c(b, crc32c(a)) == crc32c(a+b).
// Worst case: O(bytes).
std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t crc = 0) noexcept;

// Decodes the fixed header fields (no validation beyond magic and size). O(1).
Status decode_header(std::span<const std::byte> bytes, Header& out) noexcept;
// Encodes a header (header_crc written as given). O(1).
void encode_header(const Header& h, std::span<std::byte, kHeaderSize> out) noexcept;

// IEEE 754 half -> float. O(1).
float half_to_float(std::uint16_t h) noexcept;
// float -> IEEE 754 half, round to nearest even. O(1).
std::uint16_t float_to_half(float f) noexcept;

}  // namespace st::db
