// SPDX-License-Identifier: Apache-2.0
#include "st/core/arena.hpp"

#include <array>
#include <cstdint>

#include "unit/no_alloc_fixture.hpp"

namespace {
using st::Arena;
using st::Status;
using ArenaTest = st::test::NoAllocTest;

alignas(64) std::array<std::byte, 1024> g_buf;

bool aligned(const void* p, std::size_t a) { return reinterpret_cast<std::uintptr_t>(p) % a == 0; }

TEST_F(ArenaTest, AllocatesValueInitialized) {
  g_buf.fill(std::byte{0xAB});
  Arena a(g_buf);
  auto r = a.allocate<std::uint32_t>(10);
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r.value().size(), 10u);
  for (auto v : r.value()) EXPECT_EQ(v, 0u);
  EXPECT_EQ(a.used(), 40u);
}

TEST_F(ArenaTest, AlignmentAndPadding) {
  Arena a(g_buf);
  ASSERT_TRUE(a.allocate<char>(1).ok());
  auto d = a.allocate<double>(1);
  ASSERT_TRUE(d.ok());
  EXPECT_TRUE(aligned(d.value().data(), alignof(double)));
  auto line = a.allocate<char>(3, 64);
  ASSERT_TRUE(line.ok());
  EXPECT_TRUE(aligned(line.value().data(), 64));
  EXPECT_EQ(a.allocate<int>(1, 3).status(), Status::kInvalidArgument);     // not a power of 2
  EXPECT_EQ(a.allocate<double>(1, 4).status(), Status::kInvalidArgument);  // < alignof
}

TEST_F(ArenaTest, ZeroCountAndExhaustion) {
  Arena a(g_buf);
  auto z = a.allocate<int>(0);
  ASSERT_TRUE(z.ok());
  EXPECT_TRUE(z.value().empty());
  EXPECT_TRUE(a.allocate<std::byte>(1024).ok());
  EXPECT_EQ(a.remaining(), 0u);
  EXPECT_EQ(a.allocate<std::byte>(1).status(), Status::kArenaExhausted);
  EXPECT_EQ(a.used(), 1024u);  // failed allocation changes nothing
}

TEST_F(ArenaTest, SizeOverflowIsRejected) {
  Arena a(g_buf);
  EXPECT_EQ(a.allocate_uninit<std::uint64_t>(static_cast<std::size_t>(-1) / 4).status(),
            Status::kArenaExhausted);
  EXPECT_EQ(a.allocate_uninit<std::uint64_t>(static_cast<std::size_t>(-1) / 8).status(),
            Status::kArenaExhausted);
  EXPECT_EQ(a.used(), 0u);
}

TEST_F(ArenaTest, EmptyArena) {
  Arena a;
  EXPECT_EQ(a.capacity(), 0u);
  EXPECT_TRUE(a.allocate<int>(0).ok());
  EXPECT_EQ(a.allocate<int>(1).status(), Status::kArenaExhausted);
}

TEST_F(ArenaTest, MarkReleaseAndHighWater) {
  Arena a(g_buf);
  ASSERT_TRUE(a.allocate<std::byte>(100).ok());
  const auto m = a.mark();
  ASSERT_TRUE(a.allocate<std::byte>(300).ok());
  EXPECT_EQ(a.used(), 400u);
  a.release(m);
  EXPECT_EQ(a.used(), 100u);
  EXPECT_EQ(a.high_water(), 400u);
  {
    st::ArenaScope scope(a);
    ASSERT_TRUE(a.allocate<std::byte>(50).ok());
    EXPECT_EQ(a.used(), 150u);
  }
  EXPECT_EQ(a.used(), 100u);
  a.reset();
  EXPECT_EQ(a.used(), 0u);
  EXPECT_EQ(a.high_water(), 400u);
}

TEST_F(ArenaTest, UninitIsO1AndUsable) {
  Arena a(g_buf);
  auto r = a.allocate_uninit<float>(8);
  ASSERT_TRUE(r.ok());
  for (auto& v : r.value()) v = 1.0f;
  EXPECT_EQ(r.value()[7], 1.0f);
}

#if ST_ENABLE_ASSERTS
TEST(ArenaDeathTest, AllocateAfterFreezeAsserts) {
  Arena a(g_buf);
  a.freeze();
  EXPECT_TRUE(a.frozen());
  EXPECT_DEATH(static_cast<void>(a.allocate<int>(1)), "frozen");
}
TEST(ArenaDeathTest, ReleaseToNewerMarkerAsserts) {
  Arena a(g_buf);
  ASSERT_TRUE(a.allocate<int>(4).ok());
  const auto m = a.mark();
  a.reset();
  EXPECT_DEATH(a.release(m), "offset");
}
#else
TEST_F(ArenaTest, AllocateAfterFreezeFails) {
  Arena a(g_buf);
  a.freeze();
  EXPECT_EQ(a.allocate<int>(1).status(), Status::kArenaExhausted);
}
#endif
}  // namespace
