// SPDX-License-Identifier: Apache-2.0
#include "st/core/clock.hpp"

#include "unit/no_alloc_fixture.hpp"

namespace {
using ClockTest = st::test::NoAllocTest;

std::int64_t elapsed(const st::Clock& c, std::int64_t t0) { return c.now_ns() - t0; }

TEST_F(ClockTest, SteadyIsMonotonic) {
  st::SteadyClock c;
  const auto a = c.now_ns();
  const auto b = c.now_ns();
  EXPECT_GE(b, a);
  EXPECT_GE(elapsed(c, a), 0);
}

TEST_F(ClockTest, ManualThroughInterface) {
  st::ManualClock m(100);
  EXPECT_EQ(elapsed(m, 100), 0);
  m.advance_ns(50);
  EXPECT_EQ(elapsed(m, 100), 50);
  m.set_ns(1000);
  EXPECT_EQ(m.now_ns(), 1000);
}
}  // namespace
