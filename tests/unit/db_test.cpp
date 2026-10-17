// SPDX-License-Identifier: Apache-2.0
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "db_writer.hpp"
#include "st/db/database.hpp"
#include "unit/no_alloc_fixture.hpp"

namespace {
using namespace st;

// 64-byte aligned copy of a blob.
struct Aligned {
  std::vector<std::byte> raw;
  std::span<std::byte> span;
  explicit Aligned(const std::vector<std::byte>& b) : raw(b.size() + 64) {
    auto* p = raw.data() + (64 - reinterpret_cast<std::uintptr_t>(raw.data()) % 64) % 64;
    std::memcpy(p, b.data(), b.size());
    span = {p, b.size()};
  }
};

tools::DbContents tiny_db() {
  tools::DbContents c;
  for (int i = 0; i < 6; ++i) {
    const float a = 0.1f * static_cast<float>(i);
    c.star_table.insert(c.star_table.end(), {a, 0.0f, std::cos(a), std::sin(a), 0.0f, 3.0f + a});
    c.catalog_ids.push_back(static_cast<std::uint32_t>(100 + i));
  }
  c.slots.assign(16, {0, 0, 0, 0});
  c.slots[3] = {0, 1, 2, 3};
  c.slots[4] = {1, 2, 3, 4};
  c.slots[9] = {2, 3, 4, 5};
  c.header.pattern_bins = 50;
  c.header.pattern_max_error = 0.005;
  c.header.max_fov = c.header.min_fov = 0.3;
  return c;
}

TEST(DbFormat, Crc32cKnownVector) {
  const char* s = "123456789";
  EXPECT_EQ(db::crc32c({reinterpret_cast<const std::byte*>(s), 9}), 0xE3069283u);
  // Chaining equals one-shot.
  const auto* b = reinterpret_cast<const std::byte*>(s);
  EXPECT_EQ(db::crc32c({b + 4, 5}, db::crc32c({b, 4})), 0xE3069283u);
  EXPECT_EQ(db::crc32c({}), 0u);
}

TEST(DbFormat, HalfFloat) {
  for (float f : {0.0f, 1.0f, -2.5f, 65504.0f, 0.000061035156f, 5.9604645e-08f, 123.4375f}) {
    EXPECT_EQ(db::half_to_float(db::float_to_half(f)), f) << f;
  }
  EXPECT_EQ(db::float_to_half(1e6f), 0x7C00u);  // overflow to +inf
  EXPECT_NEAR(db::half_to_float(db::float_to_half(523.6f)), 523.5f, 0.26f);
}

TEST(DbFormat, HeaderRoundTrip) {
  db::Header h;
  h.num_stars = 7;
  h.num_slots = 11;
  h.pattern_bins = 50;
  h.pattern_max_error = 0.005;
  h.range_ra[1] = 1.25;
  h.sections[2] = {256, 88};
  h.flags = db::kFlagPresorted;
  std::array<std::byte, db::kHeaderSize> buf{};
  db::encode_header(h, buf);
  db::Header g;
  ASSERT_EQ(db::decode_header(buf, g), Status::kOk);
  EXPECT_EQ(g.num_stars, 7u);
  EXPECT_EQ(g.num_slots, 11u);
  EXPECT_EQ(g.pattern_max_error, 0.005);
  EXPECT_EQ(g.range_ra[1], 1.25);
  EXPECT_EQ(g.sections[2].size, 88u);
  EXPECT_EQ(g.flags, db::kFlagPresorted);
  buf[0] = std::byte{'X'};
  EXPECT_EQ(db::decode_header(buf, g), Status::kCorruptData);
}

TEST(Database, BuildOpenAndRead) {
  const auto blob = tools::build_blob(tiny_db());
  Aligned a(blob);
  std::uint64_t allocs = 0;
  Result<Database> d = Status::kNotInitialized;
  {
    st::test::AllocGuard guard(st::test::AllocMode::kCount);
    d = Database::open(a.span);
    allocs = guard.allocations();
  }
  EXPECT_EQ(allocs, 0u);
  ASSERT_TRUE(d.ok()) << to_string(d.status());
  const Database& db = d.value();
  EXPECT_EQ(db.num_stars(), 6u);
  EXPECT_EQ(db.num_slots(), 16u);
  EXPECT_EQ(db.header().num_patterns, 3u);
  EXPECT_EQ(db.header().threshold_patterns, 8u);
  EXPECT_EQ(db.catalog_id(5), 105u);
  EXPECT_FLOAT_EQ(db.star_magnitude(2), 3.2f);
  EXPECT_FLOAT_EQ(db.star(1).x, std::cos(0.1f));
  std::array<std::uint32_t, 4> p{};
  EXPECT_TRUE(db.pattern(4, p));
  EXPECT_EQ(p[3], 4u);
  EXPECT_FALSE(db.pattern(0, p));
  // Chains: start 3 -> slots 3,4 occupied, 7 empty (quadratic: 3, 4, 7, 12...)
  EXPECT_EQ(db.probe_slot(3, 0), 3u);
  EXPECT_EQ(db.probe_slot(3, 2), 7u);
  EXPECT_EQ(db.max_probe_chain(), 2u);  // from 3: {3,4} then 7 empty
  EXPECT_FALSE(db.has_key_hash());
  EXPECT_FALSE(db.has_largest_edge());
}

TEST(Database, RejectsCorruption) {
  const auto blob = tools::build_blob(tiny_db());
  {
    Aligned a(blob);
    a.span[db::kHeaderSize + 5] ^= std::byte{1};
    EXPECT_EQ(Database::open(a.span).status(), Status::kChecksumMismatch);
    Database::OpenOptions o;
    o.verify_checksum = false;
    EXPECT_TRUE(Database::open(a.span, o).ok());  // star table bit flip is not structural
  }
  {
    Aligned a(blob);
    a.span[40] ^= std::byte{1};  // num_patterns: header CRC catches it
    EXPECT_EQ(Database::open(a.span).status(), Status::kChecksumMismatch);
  }
  {
    Aligned a(blob);
    EXPECT_EQ(Database::open(a.span.first(a.span.size() - 64)).status(), Status::kCorruptData);
    EXPECT_EQ(Database::open(a.span.first(100)).status(), Status::kCorruptData);
    EXPECT_EQ(Database::open(a.span.subspan(1)).status(), Status::kInvalidArgument);
  }
  {
    Aligned a(blob);
    std::uint32_t v = 2;
    std::memcpy(a.span.data() + 8, &v, 4);
    Database::OpenOptions o;
    o.verify_checksum = false;
    EXPECT_EQ(Database::open(a.span, o).status(), Status::kVersionMismatch);
  }
  {
    // Out-of-range star index with a valid checksum must still be rejected.
    auto c = tiny_db();
    c.slots[9] = {2, 3, 4, 5};
    auto good = tools::build_blob(c);
    Aligned a(good);
    db::Header h;
    ASSERT_EQ(db::decode_header(a.span, h), Status::kOk);
    const std::uint64_t off = h.section(db::Section::kPatterns).offset + 9 * 8 + 6;
    const std::uint16_t bad = 77;
    std::memcpy(a.span.data() + off, &bad, 2);
    Database::OpenOptions o;
    o.verify_checksum = false;
    EXPECT_EQ(Database::open(a.span, o).status(), Status::kCorruptData);
  }
  {
    // Understated probe bound is caught by deep validation.
    Aligned a(blob);
    std::uint32_t one = 1;
    std::memcpy(a.span.data() + 48, &one, 4);
    Database::OpenOptions o;
    o.verify_checksum = false;
    o.validate_probe_bound = true;
    EXPECT_EQ(Database::open(a.span, o).status(), Status::kCorruptData);
  }
}

TEST(Database, KeyHashMatchesTetra3) {
  const std::uint32_t key[5] = {1, 2, 3, 4, 5};
  EXPECT_EQ(pattern_key_hash(key, 50), 1u + 2u * 50 + 3u * 2500 + 4u * 125000 + 5u * 6250000);
}

TEST(Database, OptionalSections) {
  auto c = tiny_db();
  c.key_hash.assign(16, 7);
  c.largest_edge_half.assign(16, db::float_to_half(123.5f));
  c.header.probe_mode = db::ProbeMode::kLinear;
  const auto blob = tools::build_blob(c);
  Aligned a(blob);
  auto d = Database::open(a.span);
  ASSERT_TRUE(d.ok());
  EXPECT_TRUE(d.value().has_key_hash());
  EXPECT_EQ(d.value().key_hash(3), 7u);
  EXPECT_FLOAT_EQ(d.value().largest_edge_mrad(3), 123.5f);
  EXPECT_EQ(d.value().probe_slot(3, 2), 5u);
  EXPECT_EQ(d.value().start_slot(21), 5u);
}
}  // namespace
