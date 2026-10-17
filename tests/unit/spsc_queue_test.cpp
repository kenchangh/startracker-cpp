// SPDX-License-Identifier: Apache-2.0
#include "st/containers/spsc_queue.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include "st/core/arena.hpp"
#include "unit/no_alloc_fixture.hpp"

namespace {
using st::SpscQueue;
using st::Status;
using SpscQueueTest = st::test::NoAllocTest;

TEST_F(SpscQueueTest, InitValidation) {
  std::array<int, 6> six{};
  std::array<int, 1> one{};
  std::array<int, 8> eight{};
  SpscQueue<int> q;
  EXPECT_EQ(q.capacity(), 0u);
  int out = 0;
  EXPECT_FALSE(q.try_push(1));
  EXPECT_FALSE(q.try_pop(out));
  EXPECT_EQ(q.init(six), Status::kInvalidArgument);
  EXPECT_EQ(q.init(one), Status::kInvalidArgument);
  EXPECT_EQ(q.init(eight), Status::kOk);
  EXPECT_EQ(q.capacity(), 8u);
}

TEST_F(SpscQueueTest, SingleThreadFifoUsesAllSlots) {
  std::array<std::uint32_t, 4> storage{};
  SpscQueue<std::uint32_t> q;
  ASSERT_EQ(q.init(storage), Status::kOk);
  std::uint32_t out = 0;
  EXPECT_FALSE(q.try_pop(out));
  for (std::uint32_t round = 0; round < 1000; ++round) {  // wraps the indices many times
    for (std::uint32_t i = 0; i < 4; ++i) ASSERT_TRUE(q.try_push(round * 4 + i));
    EXPECT_FALSE(q.try_push(0));
    EXPECT_EQ(q.size_approx(), 4u);
    for (std::uint32_t i = 0; i < 4; ++i) {
      ASSERT_TRUE(q.try_pop(out));
      ASSERT_EQ(out, round * 4 + i);
    }
    EXPECT_FALSE(q.try_pop(out));
  }
}

TEST_F(SpscQueueTest, StorageFromArena) {
  alignas(64) std::array<std::byte, 1024> buf{};
  st::Arena arena(buf);
  auto storage = arena.allocate<std::uint64_t>(16, st::kCacheLineSize);
  ASSERT_TRUE(storage.ok());
  SpscQueue<std::uint64_t> q;
  ASSERT_EQ(q.init(storage.value()), Status::kOk);
  ASSERT_TRUE(q.try_push(7));
  std::uint64_t out = 0;
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(out, 7u);
}

std::uint64_t stress_items() {
  if (const char* env = std::getenv("ST_SPSC_STRESS_ITEMS")) return std::strtoull(env, nullptr, 10);
  return 10'000'000;
}

// Two threads, every item checked for order and loss. Run under TSan in CI.
TEST(SpscQueueStress, TwoThreadsOrderAndNoLoss) {
  const std::uint64_t n = stress_items();
  std::array<std::uint64_t, 64> storage{};
  SpscQueue<std::uint64_t> q;
  ASSERT_EQ(q.init(storage), Status::kOk);

  std::uint64_t errors = 0;
  std::uint64_t received = 0;
  std::atomic<bool> started{false};
  std::thread consumer([&] {
    started.store(true, std::memory_order_release);
    std::uint64_t expect = 0;
    std::uint64_t v = 0;
    while (expect < n) {
      if (q.try_pop(v)) {
        if (v != expect) ++errors;
        expect = v + 1;
        ++received;
      }
    }
  });
  // Thread start-up (runtime and sanitizer bookkeeping) is init, not the measured region.
  while (!started.load(std::memory_order_acquire)) {
  }
  std::uint64_t allocs = 0;
  {
    // Covers both threads: the guard mode is process-wide.
    st::test::AllocGuard guard(st::test::AllocMode::kCount);
    for (std::uint64_t i = 0; i < n;) {
      if (q.try_push(i)) ++i;
    }
    consumer.join();
    allocs = guard.allocations();
  }
  EXPECT_EQ(allocs, 0u);
  EXPECT_EQ(errors, 0u);
  EXPECT_EQ(received, n);
  std::uint64_t v = 0;
  EXPECT_FALSE(q.try_pop(v));
}
}  // namespace
