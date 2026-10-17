// SPDX-License-Identifier: Apache-2.0
#include "st/containers/fixed_vector.hpp"

#include <numeric>

#include "unit/no_alloc_fixture.hpp"

namespace {
using st::FixedVector;
using FixedVectorTest = st::test::NoAllocTest;

struct Tracked {
  static inline int live = 0;
  int v = 0;
  Tracked() noexcept { ++live; }
  explicit Tracked(int x) noexcept : v(x) { ++live; }
  Tracked(const Tracked& o) noexcept : v(o.v) { ++live; }
  Tracked(Tracked&& o) noexcept : v(o.v) { ++live; }
  Tracked& operator=(const Tracked&) noexcept = default;
  Tracked& operator=(Tracked&&) noexcept = default;
  ~Tracked() { --live; }
};

TEST_F(FixedVectorTest, PushUntilFull) {
  FixedVector<int, 4> v;
  EXPECT_TRUE(v.empty());
  EXPECT_EQ(v.capacity(), 4u);
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(v.push_back(i));
  EXPECT_TRUE(v.full());
  EXPECT_FALSE(v.push_back(99));
  EXPECT_FALSE(v.emplace_back(99));
  EXPECT_EQ(v.size(), 4u);
  EXPECT_EQ(std::accumulate(v.begin(), v.end(), 0), 6);
  EXPECT_EQ(v.front(), 0);
  EXPECT_EQ(v.back(), 3);
  EXPECT_EQ(v.span().size(), 4u);
}

TEST_F(FixedVectorTest, PopResizeErase) {
  FixedVector<int, 8> v;
  ASSERT_TRUE(v.resize(5));
  for (int x : v) EXPECT_EQ(x, 0);
  for (std::size_t i = 0; i < 5; ++i) v[i] = static_cast<int>(i);
  v.erase_unordered(1);  // last (4) moves into slot 1
  EXPECT_EQ(v.size(), 4u);
  EXPECT_EQ(v[1], 4);
  v.erase_unordered(3);  // erase last
  EXPECT_EQ(v.size(), 3u);
  v.pop_back();
  EXPECT_EQ(v.size(), 2u);
  EXPECT_FALSE(v.resize(9));
  EXPECT_EQ(v.size(), 2u);
  ASSERT_TRUE(v.resize(0));
  EXPECT_TRUE(v.empty());
}

TEST_F(FixedVectorTest, NonTrivialLifetimes) {
  Tracked::live = 0;
  {
    FixedVector<Tracked, 4> a;
    ASSERT_TRUE(a.emplace_back(1));
    ASSERT_TRUE(a.push_back(Tracked(2)));
    EXPECT_EQ(Tracked::live, 2);
    FixedVector<Tracked, 4> b = a;
    EXPECT_EQ(Tracked::live, 4);
    FixedVector<Tracked, 4> c = std::move(b);
    EXPECT_EQ(c[1].v, 2);
    b = c;
    a.pop_back();
    EXPECT_EQ(a.size(), 1u);
    ASSERT_TRUE(a.resize(3));
    a.erase_unordered(0);
    EXPECT_EQ(a.size(), 2u);
    a.clear();
    EXPECT_TRUE(a.empty());
  }
  EXPECT_EQ(Tracked::live, 0);
}

#if ST_ENABLE_ASSERTS
TEST(FixedVectorDeathTest, OutOfBoundsAsserts) {
  FixedVector<int, 2> v;
  ASSERT_TRUE(v.push_back(1));
  EXPECT_DEATH(static_cast<void>(v[1]), "i < size_");
}
TEST(FixedVectorDeathTest, PopEmptyAsserts) {
  FixedVector<int, 2> v;
  EXPECT_DEATH(v.pop_back(), "size_ > 0");
}
#endif
}  // namespace
