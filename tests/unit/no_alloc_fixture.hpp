// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <gtest/gtest.h>

#include <optional>

#include "alloc_guard.hpp"

namespace st::test {

// Every test body runs inside a counting allocation guard; any heap allocation fails the
// test. (Passing gtest assertions do not allocate.)
class NoAllocTest : public ::testing::Test {
 protected:
  void SetUp() override { guard_.emplace(AllocMode::kCount); }
  void TearDown() override {
    const auto n = guard_->allocations();
    guard_.reset();
    EXPECT_EQ(n, 0u) << "heap allocations inside the test body";
  }

 private:
  std::optional<AllocGuard> guard_;
};

}  // namespace st::test
