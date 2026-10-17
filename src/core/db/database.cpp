// SPDX-License-Identifier: Apache-2.0
#include "st/db/database.hpp"

#include <bit>
#include <cmath>

namespace st {
namespace {

constexpr std::uint64_t kMagicRand = 2654435761ULL;

bool section_ok(const db::SectionInfo& s, std::uint64_t file_size, std::uint64_t expected,
                bool required) noexcept {
  if (s.size == 0 && s.offset == 0) return !required;
  if (s.size != expected) return false;
  if (s.offset < db::kHeaderSize || s.offset % db::kSectionAlign != 0) return false;
  if (s.offset > file_size || s.size > file_size - s.offset) return false;
  return true;
}

}  // namespace

std::uint64_t pattern_key_hash(std::span<const std::uint32_t> key, std::uint32_t bins) noexcept {
  std::uint64_t h = 0;
  std::uint64_t f = 1;
  for (std::uint32_t k : key) {
    h += static_cast<std::uint64_t>(k) * f;
    f *= bins;
  }
  return h;
}

std::uint64_t Database::start_slot(std::uint64_t key_hash) const noexcept {
  const std::uint64_t n = h_.num_slots;
  if (h_.probe_mode == db::ProbeMode::kLinear) return key_hash % n;
  return (key_hash * kMagicRand) % n;  // uint64 wrap, as numpy does
}

std::uint64_t Database::probe_slot(std::uint64_t start, std::uint64_t c) const noexcept {
  const std::uint64_t n = h_.num_slots;
  if (h_.probe_mode == db::ProbeMode::kLinear) return (start + c) % n;
  return (start + c * c) % n;
}

Result<Database> Database::open(std::span<const std::byte> blob,
                                const OpenOptions& options) noexcept {
  if constexpr (std::endian::native != std::endian::little) return Status::kUnsupported;
  if (reinterpret_cast<std::uintptr_t>(blob.data()) % 8 != 0) return Status::kInvalidArgument;
  db::Header h;
  ST_TRY(db::decode_header(blob, h));
  if (h.format_version != db::kFormatVersion) return Status::kVersionMismatch;
  if (h.file_size != blob.size()) return Status::kCorruptData;
  if (options.verify_checksum) {
    std::array<std::byte, db::kHeaderSize> tmp{};
    std::memcpy(tmp.data(), blob.data(), db::kHeaderSize);
    std::memset(tmp.data() + 24, 0, 4);
    if (db::crc32c(tmp) != h.header_crc) return Status::kChecksumMismatch;
    if (db::crc32c(blob.subspan(db::kHeaderSize)) != h.payload_crc) {
      return Status::kChecksumMismatch;
    }
  }
  if (h.pattern_size != db::kPatternSize) return Status::kUnsupported;
  if (h.star_index_bytes != 2 && h.star_index_bytes != 4) return Status::kUnsupported;
  if (h.probe_mode != db::ProbeMode::kQuadraticMagic && h.probe_mode != db::ProbeMode::kLinear) {
    return Status::kUnsupported;
  }
  if (h.num_stars == 0 || h.num_slots == 0 || h.pattern_bins == 0) return Status::kCorruptData;
  if (h.star_index_bytes == 2 && h.num_stars > 65536u) return Status::kCorruptData;
  if (!(h.pattern_max_error > 0) || !(h.max_fov > 0) || !(h.min_fov > 0) ||
      !std::isfinite(h.max_fov) || !std::isfinite(h.min_fov) || h.max_fov > 3.2) {
    return Status::kCorruptData;
  }
  const std::uint64_t ns = h.num_stars, nslots = h.num_slots;
  const std::uint64_t fs = h.file_size;
  if (!section_ok(h.section(db::Section::kStarTable), fs, ns * 24, true) ||
      !section_ok(h.section(db::Section::kCatalogIds), fs, ns * 4, false) ||
      !section_ok(h.section(db::Section::kPatterns), fs, nslots * 4 * h.star_index_bytes, true) ||
      !section_ok(h.section(db::Section::kKeyHash), fs, nslots * 2, false) ||
      !section_ok(h.section(db::Section::kLargestEdge), fs, nslots * 2, false)) {
    return Status::kCorruptData;
  }
  if (h.max_probe_chain == 0 || h.max_probe_chain > h.num_slots) return Status::kCorruptData;

  Database d;
  d.h_ = h;
  d.blob_ = blob;
  auto at = [&](db::Section s) -> const std::byte* {
    const auto& si = h.section(s);
    return si.size == 0 ? nullptr : blob.data() + si.offset;
  };
  d.stars_ = at(db::Section::kStarTable);
  d.ids_ = at(db::Section::kCatalogIds);
  d.patterns_ = at(db::Section::kPatterns);
  d.key_hash_ = at(db::Section::kKeyHash);
  d.largest_edge_ = at(db::Section::kLargestEdge);

  if (options.validate_indices) {
    std::array<std::uint32_t, 4> p{};
    for (std::uint64_t s = 0; s < nslots; ++s) {
      if (!d.pattern(s, p)) continue;
      if (p[0] >= ns || p[1] >= ns || p[2] >= ns || p[3] >= ns) return Status::kCorruptData;
    }
    for (std::uint32_t i = 0; i < h.num_stars; ++i) {
      const StarRecord r = d.star(i);
      if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.z)) {
        return Status::kCorruptData;
      }
    }
  }
  if (options.validate_probe_bound) {
    auto chain = compute_max_probe_chain(d, h.max_probe_chain);
    if (!chain.ok() || chain.value() > h.max_probe_chain) return Status::kCorruptData;
  }
  return d;
}

Result<std::uint32_t> compute_max_probe_chain(const Database& d, std::uint32_t limit) noexcept {
  std::uint32_t worst = 0;
  std::array<std::uint32_t, 4> p{};
  const std::uint64_t n = d.num_slots();
  for (std::uint64_t start = 0; start < n; ++start) {
    std::uint32_t c = 0;
    while (d.pattern(d.probe_slot(start, c), p)) {
      ++c;
      if (c > limit) return Status::kCorruptData;
    }
    if (c > worst) worst = c;
  }
  // A chain of length 0 still costs one probe; report at least 1 so the bound is usable.
  return worst == 0 ? 1u : worst;
}

}  // namespace st
