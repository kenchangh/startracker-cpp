// SPDX-License-Identifier: Apache-2.0
#include "st/db/format.hpp"

#include <bit>
#include <cstring>

namespace st::db {
namespace {

constexpr std::array<std::array<std::uint32_t, 256>, 8> make_crc_tables() noexcept {
  std::array<std::array<std::uint32_t, 256>, 8> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
    t[0][i] = c;
  }
  for (std::size_t i = 0; i < 256; ++i) {
    for (std::size_t s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFFu];
  }
  return t;
}
constexpr auto kCrcTables = make_crc_tables();

template <class T>
T load(const std::byte* p) noexcept {
  T v;
  std::memcpy(&v, p, sizeof(T));
  if constexpr (std::endian::native == std::endian::big && sizeof(T) > 1) {
    // Big-endian hosts are rejected by Database::open; keep decode well-defined anyway.
    unsigned char b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    for (std::size_t i = 0; i < sizeof(T) / 2; ++i) {
      const unsigned char t = b[i];
      b[i] = b[sizeof(T) - 1 - i];
      b[sizeof(T) - 1 - i] = t;
    }
    std::memcpy(&v, b, sizeof(T));
  }
  return v;
}
template <class T>
void store(std::byte* p, T v) noexcept {
  std::memcpy(p, &v, sizeof(T));
}

}  // namespace

std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t crc) noexcept {
  const auto* p = reinterpret_cast<const unsigned char*>(data.data());
  std::size_t n = data.size();
  crc = ~crc;
  while (n >= 8) {
    std::uint32_t lo = 0, hi = 0;
    std::memcpy(&lo, p, 4);
    std::memcpy(&hi, p + 4, 4);
    if constexpr (std::endian::native == std::endian::big) {
      lo = __builtin_bswap32(lo);
      hi = __builtin_bswap32(hi);
    }
    lo ^= crc;
    crc = kCrcTables[7][lo & 0xFFu] ^ kCrcTables[6][(lo >> 8) & 0xFFu] ^
          kCrcTables[5][(lo >> 16) & 0xFFu] ^ kCrcTables[4][lo >> 24] ^ kCrcTables[3][hi & 0xFFu] ^
          kCrcTables[2][(hi >> 8) & 0xFFu] ^ kCrcTables[1][(hi >> 16) & 0xFFu] ^
          kCrcTables[0][hi >> 24];
    p += 8;
    n -= 8;
  }
  while (n-- > 0) crc = (crc >> 8) ^ kCrcTables[0][(crc ^ *p++) & 0xFFu];
  return ~crc;
}

Status decode_header(std::span<const std::byte> bytes, Header& h) noexcept {
  if (bytes.size() < kHeaderSize) return Status::kCorruptData;
  if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) return Status::kCorruptData;
  const std::byte* p = bytes.data();
  h.format_version = load<std::uint32_t>(p + 8);
  if (load<std::uint32_t>(p + 12) != kHeaderSize) return Status::kCorruptData;
  h.file_size = load<std::uint64_t>(p + 16);
  h.header_crc = load<std::uint32_t>(p + 24);
  h.payload_crc = load<std::uint32_t>(p + 28);
  h.num_stars = load<std::uint32_t>(p + 32);
  h.num_slots = load<std::uint32_t>(p + 36);
  h.num_patterns = load<std::uint32_t>(p + 40);
  h.threshold_patterns = load<std::uint32_t>(p + 44);
  h.max_probe_chain = load<std::uint32_t>(p + 48);
  h.pattern_size = load<std::uint16_t>(p + 52);
  h.pattern_bins = load<std::uint16_t>(p + 54);
  h.star_index_bytes = load<std::uint8_t>(p + 56);
  h.probe_mode = static_cast<ProbeMode>(load<std::uint8_t>(p + 57));
  h.catalog = static_cast<CatalogKind>(load<std::uint8_t>(p + 58));
  h.flags = load<std::uint32_t>(p + 60);
  h.pattern_max_error = load<double>(p + 64);
  h.max_fov = load<double>(p + 72);
  h.min_fov = load<double>(p + 80);
  h.star_max_magnitude = load<double>(p + 88);
  h.epoch_proper_motion = load<double>(p + 96);
  h.epoch_equinox = load<double>(p + 104);
  h.range_ra[0] = load<double>(p + 112);
  h.range_ra[1] = load<double>(p + 120);
  h.range_dec[0] = load<double>(p + 128);
  h.range_dec[1] = load<double>(p + 136);
  h.pattern_stars_per_fov = load<std::uint16_t>(p + 144);
  h.verification_stars_per_fov = load<std::uint16_t>(p + 146);
  h.patterns_per_lattice_field = load<std::uint32_t>(p + 148);
  for (std::size_t i = 0; i < kSectionCount; ++i) {
    h.sections[i].offset = load<std::uint64_t>(p + 152 + 16 * i);
    h.sections[i].size = load<std::uint64_t>(p + 160 + 16 * i);
  }
  return Status::kOk;
}

void encode_header(const Header& h, std::span<std::byte, kHeaderSize> out) noexcept {
  std::byte* p = out.data();
  std::memset(p, 0, kHeaderSize);
  std::memcpy(p, kMagic.data(), kMagic.size());
  store<std::uint32_t>(p + 8, h.format_version);
  store<std::uint32_t>(p + 12, static_cast<std::uint32_t>(kHeaderSize));
  store<std::uint64_t>(p + 16, h.file_size);
  store<std::uint32_t>(p + 24, h.header_crc);
  store<std::uint32_t>(p + 28, h.payload_crc);
  store<std::uint32_t>(p + 32, h.num_stars);
  store<std::uint32_t>(p + 36, h.num_slots);
  store<std::uint32_t>(p + 40, h.num_patterns);
  store<std::uint32_t>(p + 44, h.threshold_patterns);
  store<std::uint32_t>(p + 48, h.max_probe_chain);
  store<std::uint16_t>(p + 52, h.pattern_size);
  store<std::uint16_t>(p + 54, h.pattern_bins);
  store<std::uint8_t>(p + 56, h.star_index_bytes);
  store<std::uint8_t>(p + 57, static_cast<std::uint8_t>(h.probe_mode));
  store<std::uint8_t>(p + 58, static_cast<std::uint8_t>(h.catalog));
  store<std::uint32_t>(p + 60, h.flags);
  store<double>(p + 64, h.pattern_max_error);
  store<double>(p + 72, h.max_fov);
  store<double>(p + 80, h.min_fov);
  store<double>(p + 88, h.star_max_magnitude);
  store<double>(p + 96, h.epoch_proper_motion);
  store<double>(p + 104, h.epoch_equinox);
  store<double>(p + 112, h.range_ra[0]);
  store<double>(p + 120, h.range_ra[1]);
  store<double>(p + 128, h.range_dec[0]);
  store<double>(p + 136, h.range_dec[1]);
  store<std::uint16_t>(p + 144, h.pattern_stars_per_fov);
  store<std::uint16_t>(p + 146, h.verification_stars_per_fov);
  store<std::uint32_t>(p + 148, h.patterns_per_lattice_field);
  for (std::size_t i = 0; i < kSectionCount; ++i) {
    store<std::uint64_t>(p + 152 + 16 * i, h.sections[i].offset);
    store<std::uint64_t>(p + 160 + 16 * i, h.sections[i].size);
  }
}

float half_to_float(std::uint16_t h) noexcept {
  const std::uint32_t sign = (h & 0x8000u) << 16;
  const std::uint32_t exp = (h >> 10) & 0x1Fu;
  std::uint32_t mant = h & 0x3FFu;
  std::uint32_t bits = 0;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else {  // subnormal: normalize
      int e = -1;
      do {
        ++e;
        mant <<= 1;
      } while ((mant & 0x400u) == 0);
      bits = sign | static_cast<std::uint32_t>(127 - 15 - e) << 23 | (mant & 0x3FFu) << 13;
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | mant << 13;
  } else {
    bits = sign | (exp + 112u) << 23 | mant << 13;
  }
  return std::bit_cast<float>(bits);
}

std::uint16_t float_to_half(float f) noexcept {
  const std::uint32_t x = std::bit_cast<std::uint32_t>(f);
  const std::uint32_t sign = (x >> 16) & 0x8000u;
  const std::uint32_t exp = (x >> 23) & 0xFFu;
  std::uint32_t mant = x & 0x7FFFFFu;
  if (exp == 0xFF) return static_cast<std::uint16_t>(sign | 0x7C00u | (mant != 0 ? 0x200u : 0u));
  const int e = static_cast<int>(exp) - 127 + 15;
  if (e >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);  // overflow -> inf
  if (e <= 0) {                                                    // subnormal or zero
    if (e < -10) return static_cast<std::uint16_t>(sign);
    mant |= 0x800000u;
    const unsigned shift = static_cast<unsigned>(14 - e);
    std::uint32_t half = mant >> shift;
    const std::uint32_t rem = mant & ((1u << shift) - 1u);
    const std::uint32_t halfway = 1u << (shift - 1);
    if (rem > halfway || (rem == halfway && (half & 1u))) ++half;
    return static_cast<std::uint16_t>(sign | half);
  }
  std::uint32_t half = static_cast<std::uint32_t>(e) << 10 | mant >> 13;
  const std::uint32_t rem = mant & 0x1FFFu;
  if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) ++half;  // may carry into exponent
  return static_cast<std::uint16_t>(sign | half);
}

}  // namespace st::db
