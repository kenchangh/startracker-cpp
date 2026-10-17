// SPDX-License-Identifier: Apache-2.0
#include "db_writer.hpp"

#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>

#include "st/db/database.hpp"

namespace st::tools {
namespace {
std::uint64_t align_up(std::uint64_t v) {
  return (v + db::kSectionAlign - 1) / db::kSectionAlign * db::kSectionAlign;
}

// 64-byte aligned byte buffer (Database::open requires 8-byte alignment).
struct AlignedBytes {
  std::unique_ptr<std::byte[]> raw;
  std::byte* p = nullptr;
  explicit AlignedBytes(std::size_t n) : raw(new std::byte[n + 64]) {
    p = raw.get() + (64 - reinterpret_cast<std::uintptr_t>(raw.get()) % 64) % 64;
  }
};
}  // namespace

std::vector<std::byte> build_blob(DbContents c) {
  db::Header& h = c.header;
  const std::size_t ns = c.star_table.size() / 6;
  if (ns == 0 || c.star_table.size() % 6 != 0)
    throw std::runtime_error("db: empty or ragged star table");
  if (!c.catalog_ids.empty() && c.catalog_ids.size() != ns)
    throw std::runtime_error("db: id count");
  h.num_stars = static_cast<std::uint32_t>(ns);
  h.num_slots = static_cast<std::uint32_t>(c.slots.size());
  if (!c.key_hash.empty() && c.key_hash.size() != c.slots.size())
    throw std::runtime_error("db: key hash count");
  if (!c.largest_edge_half.empty() && c.largest_edge_half.size() != c.slots.size())
    throw std::runtime_error("db: edge count");
  h.star_index_bytes = ns <= 65536 ? 2 : 4;
  std::uint32_t used = 0;
  for (const auto& s : c.slots) used += (s[0] | s[1] | s[2] | s[3]) != 0;
  if (h.num_patterns == 0) h.num_patterns = used;
  if (h.threshold_patterns == 0) h.threshold_patterns = h.num_slots / 2;

  std::uint64_t off = db::kHeaderSize;
  auto place = [&](db::Section s, std::uint64_t size) {
    if (size == 0) {
      h.section(s) = {};
      return;
    }
    off = align_up(off);
    h.section(s) = {off, size};
    off += size;
  };
  place(db::Section::kStarTable, ns * 24);
  place(db::Section::kCatalogIds, c.catalog_ids.size() * 4);
  place(db::Section::kPatterns, c.slots.size() * 4ull * h.star_index_bytes);
  place(db::Section::kKeyHash, c.key_hash.size() * 2);
  place(db::Section::kLargestEdge, c.largest_edge_half.size() * 2);
  h.file_size = align_up(off);

  AlignedBytes buf(h.file_size);
  std::memset(buf.p, 0, h.file_size);
  auto put = [&](db::Section s, const void* src) {
    const auto& si = h.section(s);
    if (si.size) std::memcpy(buf.p + si.offset, src, si.size);
  };
  put(db::Section::kStarTable, c.star_table.data());
  put(db::Section::kCatalogIds, c.catalog_ids.data());
  {
    const auto& si = h.section(db::Section::kPatterns);
    std::byte* dst = buf.p + si.offset;
    for (std::size_t i = 0; i < c.slots.size(); ++i) {
      for (int k = 0; k < 4; ++k) {
        if (c.slots[i][k] >= ns) throw std::runtime_error("db: star index out of range");
        if (h.star_index_bytes == 2) {
          const auto v = static_cast<std::uint16_t>(c.slots[i][k]);
          std::memcpy(dst + (i * 4 + k) * 2, &v, 2);
        } else {
          std::memcpy(dst + (i * 4 + k) * 4, &c.slots[i][k], 4);
        }
      }
    }
  }
  put(db::Section::kKeyHash, c.key_hash.data());
  put(db::Section::kLargestEdge, c.largest_edge_half.data());

  // Probe bound: open without validation, measure, then finalize.
  h.max_probe_chain = h.num_slots;  // temporary upper bound accepted by open()
  auto finalize = [&] {
    h.header_crc = 0;
    h.payload_crc = db::crc32c({buf.p + db::kHeaderSize, h.file_size - db::kHeaderSize});
    std::span<std::byte, db::kHeaderSize> hdr(buf.p, db::kHeaderSize);
    db::encode_header(h, hdr);
    h.header_crc = db::crc32c({buf.p, db::kHeaderSize});
    db::encode_header(h, hdr);
  };
  finalize();
  Database::OpenOptions loose;
  loose.verify_checksum = false;
  loose.validate_indices = false;
  auto d = Database::open({buf.p, h.file_size}, loose);
  if (!d.ok()) throw std::runtime_error(std::string("db: reopen failed: ") + to_string(d.status()));
  auto chain = compute_max_probe_chain(d.value(), h.num_slots);
  if (!chain.ok()) throw std::runtime_error("db: unbounded probe chain (table full or cyclic)");
  h.max_probe_chain = chain.value();
  finalize();

  Database::OpenOptions strict;
  strict.validate_probe_bound = true;
  auto check = Database::open({buf.p, h.file_size}, strict);
  if (!check.ok())
    throw std::runtime_error(std::string("db: validation failed: ") + to_string(check.status()));
  return std::vector<std::byte>(buf.p, buf.p + h.file_size);
}

void write_file(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!f) throw std::runtime_error("cannot write " + path);
}

std::vector<std::byte> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error("cannot open " + path);
  const auto n = static_cast<std::size_t>(f.tellg());
  std::vector<std::byte> v(n);
  f.seekg(0);
  f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n));
  if (!f) throw std::runtime_error("cannot read " + path);
  return v;
}

}  // namespace st::tools
