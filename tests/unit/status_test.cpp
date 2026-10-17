// SPDX-License-Identifier: Apache-2.0
#include "st/core/status.hpp"

#include <cstring>

#include "unit/no_alloc_fixture.hpp"

namespace {
using StatusTest = st::test::NoAllocTest;

TEST_F(StatusTest, EveryCodeHasAName) {
  for (unsigned i = 0; i <= static_cast<unsigned>(st::Status::kIoError); ++i) {
    const char* name = st::to_string(static_cast<st::Status>(i));
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name[0], 'k');
    EXPECT_NE(std::strcmp(name, "kUnknown"), 0) << i;
  }
  EXPECT_STREQ(st::to_string(st::Status::kNoMatch), "kNoMatch");
  EXPECT_STREQ(st::to_string(static_cast<st::Status>(999)), "kUnknown");
}
}  // namespace
