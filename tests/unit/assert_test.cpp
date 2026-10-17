// SPDX-License-Identifier: Apache-2.0
#include "st/core/assert.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>

namespace {

void custom_handler(const char* file, int line, const char* msg) noexcept {
  std::fprintf(stderr, "custom handler: %s (%s:%d)\n", msg, file, line);
  std::_Exit(42);
}
void returning_handler(const char*, int, const char*) noexcept {}

TEST(FaultDeathTest, DefaultHandlerAborts) {
  EXPECT_DEATH(st::fault("f.cpp", 12, "boom"), "st: fault at f.cpp:12: boom");
}

TEST(FaultDeathTest, CustomHandlerIsCalled) {
  EXPECT_EXIT(
      {
        st::set_fault_handler(&custom_handler);
        st::fault("f.cpp", 1, "custom");
      },
      ::testing::ExitedWithCode(42), "custom handler: custom");
}

TEST(FaultDeathTest, ReturningHandlerStillAborts) {
  EXPECT_DEATH(
      {
        st::set_fault_handler(&returning_handler);
        st::fault("f.cpp", 1, "x");
      },
      "");
}

TEST(Fault, SetHandlerReturnsPrevious) {
  const st::FaultHandler def = st::set_fault_handler(&custom_handler);
  EXPECT_EQ(st::set_fault_handler(nullptr), &custom_handler);
  EXPECT_EQ(st::set_fault_handler(nullptr), def);  // nullptr restored the default
}

#if ST_ENABLE_ASSERTS
TEST(AssertDeathTest, FailedAssertFaults) {
  const int x = 1;
  EXPECT_DEATH(ST_ASSERT(x == 2), "x == 2");
}
#else
TEST(Assert, DisabledAssertDoesNotEvaluate) {
  int calls = 0;
  ST_ASSERT(++calls == 0);
  EXPECT_EQ(calls, 0);
}
#endif

}  // namespace
