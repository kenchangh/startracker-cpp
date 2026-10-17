// SPDX-License-Identifier: Apache-2.0
#include "st/containers/bitset.hpp"

#include <array>

#include "unit/no_alloc_fixture.hpp"

namespace {
using st::BitSet;
using st::BitSpan;
using BitSetTest = st::test::NoAllocTest;

TEST_F(BitSetTest, SetTestCount) {
  BitSet<130> b;
  EXPECT_FALSE(b.any());
  EXPECT_EQ(b.find_first(), BitSet<130>::npos);
  b.set(0);
  b.set(64);
  b.set(129);
  EXPECT_TRUE(b.test(64));
  EXPECT_FALSE(b.test(63));
  EXPECT_EQ(b.count(), 3u);
  EXPECT_EQ(b.find_first(), 0u);
  EXPECT_EQ(b.find_next(1), 64u);
  EXPECT_EQ(b.find_next(65), 129u);
  EXPECT_EQ(b.find_next(130), BitSet<130>::npos);
  b.reset(64);
  b.assign(5, true);
  EXPECT_EQ(b.find_next(1), 5u);
  EXPECT_EQ(b.count(), 3u);
}

TEST_F(BitSetTest, FillKeepsTailClear) {
  BitSet<70> b;
  b.fill(true);
  EXPECT_EQ(b.count(), 70u);
  EXPECT_EQ(b.find_next(69), 69u);
  EXPECT_EQ(b.find_next(70), BitSet<70>::npos);
  b.fill(false);
  EXPECT_EQ(b.count(), 0u);
}

TEST_F(BitSetTest, ExactWordMultiple) {
  BitSet<128> b;
  b.fill(true);
  EXPECT_EQ(b.count(), 128u);
  EXPECT_EQ(b.find_next(127), 127u);
}

TEST_F(BitSetTest, SpanOverExternalWords) {
  std::array<std::uint64_t, 2> words{};
  BitSpan s(words, 100);
  EXPECT_EQ(s.size(), 100u);
  s.set(99);
  EXPECT_EQ(words[1], std::uint64_t{1} << 35);
  EXPECT_EQ(s.find_first(), 99u);
  s.fill(true);
  EXPECT_EQ(s.count(), 100u);
}

#if ST_ENABLE_ASSERTS
TEST(BitSetDeathTest, OutOfRangeAsserts) {
  BitSet<10> b;
  EXPECT_DEATH(b.set(10), "i < bits_");
}
#endif
}  // namespace
