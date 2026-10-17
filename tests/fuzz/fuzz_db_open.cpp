// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: Database::open must never crash, hang or read out of bounds, and an opened
// database must be safe to traverse within its declared bounds.
#include <cstdint>
#include <cstring>
#include <vector>

#include "st/db/database.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static std::vector<std::uint64_t> buf;  // 8-byte aligned storage
  buf.assign(size / 8 + 1, 0);
  std::memcpy(buf.data(), data, size);
  const std::span<const std::byte> blob(reinterpret_cast<const std::byte*>(buf.data()), size);
  st::Database::OpenOptions o;
  o.verify_checksum = false;  // let the fuzzer reach the structural checks
  o.validate_indices = true;
  o.validate_probe_bound = size < 4096;  // keep runs fast
  auto d = st::Database::open(blob, o);
  if (!d.ok()) return 0;
  const st::Database& db = d.value();
  std::array<std::uint32_t, 4> p{};
  volatile float sink = 0;
  for (std::uint64_t s = 0; s < db.num_slots() && s < 4096; ++s) {
    if (db.pattern(s, p)) {
      for (std::uint32_t i : p) sink = sink + db.star(i).mag;
    }
    const std::uint64_t start = db.start_slot(s * 2654435761ULL);
    for (std::uint32_t c = 0; c <= db.max_probe_chain() && c < 64; ++c) {
      if (!db.pattern(db.probe_slot(start, c), p)) break;
    }
  }
  if (db.has_largest_edge()) sink = sink + db.largest_edge_mrad(0);
  if (db.has_key_hash()) sink = sink + db.key_hash(0);
  if (db.has_catalog_ids()) sink = sink + static_cast<float>(db.catalog_id(0));
  return 0;
}
