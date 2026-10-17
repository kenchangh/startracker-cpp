// SPDX-License-Identifier: Apache-2.0
#include "st/containers/ring_buffer.hpp"

#include "unit/no_alloc_fixture.hpp"

namespace {
using st::RingBuffer;
using RingBufferTest = st::test::NoAllocTest;

TEST_F(RingBufferTest, FifoAndWrap) {
  RingBuffer<int, 4> r;
  EXPECT_TRUE(r.empty());
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(r.try_push(i));
  EXPECT_TRUE(r.full());
  EXPECT_FALSE(r.try_push(4));
  EXPECT_EQ(r.front(), 0);
  r.pop_front();
  r.pop_front();
  EXPECT_TRUE(r.try_push(4));
  EXPECT_TRUE(r.try_push(5));
  for (std::size_t i = 0; i < 4; ++i) EXPECT_EQ(r[i], static_cast<int>(i) + 2);
  EXPECT_EQ(r.back(), 5);
}

TEST_F(RingBufferTest, OverwriteDropsOldest) {
  RingBuffer<int, 2> r;
  EXPECT_FALSE(r.push_overwrite(1));
  EXPECT_FALSE(r.push_overwrite(2));
  EXPECT_TRUE(r.push_overwrite(3));
  EXPECT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], 2);
  EXPECT_EQ(r[1], 3);
  for (int i = 4; i < 100; ++i) r.push_overwrite(i);
  EXPECT_EQ(r.front(), 98);
  EXPECT_EQ(r.back(), 99);
  r.clear();
  EXPECT_TRUE(r.empty());
}

#if ST_ENABLE_ASSERTS
TEST(RingBufferDeathTest, PopEmptyAsserts) {
  RingBuffer<int, 2> r;
  EXPECT_DEATH(r.pop_front(), "size_ > 0");
}
#endif
}  // namespace
