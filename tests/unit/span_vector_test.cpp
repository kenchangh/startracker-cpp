// SPDX-License-Identifier: Apache-2.0
#include "st/containers/span_vector.hpp"

#include <array>

#include "st/core/arena.hpp"
#include "unit/no_alloc_fixture.hpp"

namespace {
using st::SpanVector;
using SpanVectorTest = st::test::NoAllocTest;

struct Centroid {
  float x, y;
};

TEST_F(SpanVectorTest, FromArena) {
  alignas(16) std::array<std::byte, 256> buf{};
  st::Arena arena(buf);
  auto storage = arena.allocate<Centroid>(3);
  ASSERT_TRUE(storage.ok());
  SpanVector<Centroid> v(storage.value());
  EXPECT_EQ(v.capacity(), 3u);
  EXPECT_TRUE(v.push_back({1, 2}));
  EXPECT_TRUE(v.emplace_back(3.0f, 4.0f));
  EXPECT_TRUE(v.push_back({5, 6}));
  EXPECT_TRUE(v.full());
  EXPECT_FALSE(v.push_back({7, 8}));
  EXPECT_EQ(v[1].y, 4.0f);
  EXPECT_EQ(v.back().x, 5.0f);
  v.erase_unordered(0);
  EXPECT_EQ(v[0].x, 5.0f);
  EXPECT_EQ(v.size(), 2u);
  v.pop_back();
  EXPECT_EQ(v.span().size(), 1u);
  ASSERT_TRUE(v.resize(3));
  EXPECT_EQ(v[2].x, 0.0f);
  EXPECT_FALSE(v.resize(4));
  v.clear();
  EXPECT_TRUE(v.empty());
}

TEST_F(SpanVectorTest, DefaultHasNoCapacity) {
  SpanVector<int> v;
  EXPECT_EQ(v.capacity(), 0u);
  EXPECT_FALSE(v.push_back(1));
}

#if ST_ENABLE_ASSERTS
TEST(SpanVectorDeathTest, OutOfBoundsAsserts) {
  std::array<int, 2> s{};
  SpanVector<int> v(s);
  EXPECT_DEATH(static_cast<void>(v[0]), "i < size_");
}
#endif
}  // namespace
