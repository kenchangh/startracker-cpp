// SPDX-License-Identifier: Apache-2.0
// Self-tests: the guard must see allocations, and must abort in abort mode.
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <vector>

#include "alloc_guard.hpp"

namespace {

using st::test::AllocGuard;
using st::test::AllocMode;

// Keeps the optimizer from eliding allocations.
void* volatile g_sink;

TEST(AllocGuard, ReportsBackend) {
  ::testing::Test::RecordProperty("backend", st::test::alloc_guard_backend());
  SUCCEED() << st::test::alloc_guard_backend();
}

TEST(AllocGuard, CountsVectorPush) {
  std::uint64_t n = 0;
  {
    AllocGuard g(AllocMode::kCount);
    std::vector<int> v;
    v.push_back(1);
    g_sink = v.data();
    n = g.allocations();
  }
  EXPECT_GT(n, 0u);
}

TEST(AllocGuard, CountsNewArrayAndAligned) {
  std::uint64_t n = 0;
  {
    AllocGuard g(AllocMode::kCount);
    auto* a = new int[4];
    g_sink = a;
    delete[] a;
    struct alignas(256) Big {
      char c;
    };
    auto* b = new Big;
    g_sink = b;
    delete b;
    n = g.allocations();
  }
  EXPECT_EQ(n, 2u);
}

TEST(AllocGuard, CountsMallocFamily) {
  if (!st::test::alloc_guard_hooks_malloc()) GTEST_SKIP() << "malloc not hooked on this platform";
  std::uint64_t n = 0;
  {
    AllocGuard g(AllocMode::kCount);
    void* p = std::malloc(16);
    g_sink = p;
    p = std::realloc(p, 64);
    g_sink = p;
    std::free(p);
    p = std::calloc(4, 4);
    g_sink = p;
    std::free(p);
    n = g.allocations();
  }
  EXPECT_EQ(n, 3u);
}

TEST(AllocGuard, CountsAlignedAlloc) {
  if (!st::test::alloc_guard_hooks_aligned_alloc()) {
    GTEST_SKIP() << "aligned_alloc not hooked in this build (" << st::test::alloc_guard_backend()
                 << ")";
  }
  std::uint64_t n = 0;
  {
    AllocGuard g(AllocMode::kCount);
    void* p = std::aligned_alloc(64, 128);
    g_sink = p;
    std::free(p);
    void* q = nullptr;
    if (posix_memalign(&q, 64, 128) == 0) g_sink = q;
    std::free(q);
    n = g.allocations();
  }
  EXPECT_EQ(n, 2u);
}

TEST(AllocGuard, OffOutsideGuard) {
  const auto before = st::test::allocation_count();
  auto p = std::make_unique<int>(3);
  g_sink = p.get();
  EXPECT_EQ(st::test::allocation_count(), before);
}

TEST(AllocGuard, ExemptThreadIsNotCounted) {
  AllocGuard g(AllocMode::kCount);
  {
    st::test::AllocExempt exempt;
    auto p = std::make_unique<int>(3);
    g_sink = p.get();
  }
  EXPECT_EQ(g.allocations(), 0u);
}

TEST(AllocGuardDeathTest, AbortModeAbortsOnNew) {
  EXPECT_DEATH(
      {
        AllocGuard g(AllocMode::kAbort);
        g_sink = new int(1);
      },
      "heap allocation inside a no-alloc region");
}

TEST(AllocGuardDeathTest, AbortModeAbortsOnMalloc) {
  if (!st::test::alloc_guard_hooks_malloc()) GTEST_SKIP() << "malloc not hooked on this platform";
  EXPECT_DEATH(
      {
        AllocGuard g(AllocMode::kAbort);
        g_sink = std::malloc(8);
      },
      "heap allocation inside a no-alloc region");
}

}  // namespace
