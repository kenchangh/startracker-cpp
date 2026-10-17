// SPDX-License-Identifier: Apache-2.0
#include "st/core/result.hpp"

#include <type_traits>
#include <utility>

#include "unit/no_alloc_fixture.hpp"

namespace {
using st::Result;
using st::Status;
using ResultTest = st::test::NoAllocTest;

static_assert(std::is_trivially_copyable_v<Result<int>>);
static_assert(std::is_trivially_copyable_v<Result<float>>);
static_assert(sizeof(Result<float>) <= sizeof(float) + alignof(float));
static_assert(sizeof(Result<double>) <= sizeof(double) + alignof(double));

// Non-trivial type that counts live instances.
struct Tracked {
  static inline int live = 0;
  int v;
  explicit Tracked(int x) : v(x) { ++live; }
  Tracked(const Tracked& o) : v(o.v) { ++live; }
  Tracked(Tracked&& o) noexcept : v(o.v) { ++live; }
  Tracked& operator=(const Tracked&) = default;
  Tracked& operator=(Tracked&&) noexcept = default;
  ~Tracked() { --live; }
};
static_assert(!std::is_trivially_copyable_v<Result<Tracked>>);

TEST_F(ResultTest, ValueAndError) {
  Result<int> a = 7;
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a.status(), Status::kOk);
  EXPECT_EQ(a.value(), 7);
  Result<int> b = Status::kNoMatch;
  EXPECT_FALSE(b.ok());
  EXPECT_EQ(b.status(), Status::kNoMatch);
  EXPECT_EQ(b.value_or(-1), -1);
  EXPECT_EQ(a.value_or(-1), 7);
}

TEST_F(ResultTest, InPlace) {
  struct P {
    int x, y;
  };
  Result<P> r(std::in_place, P{1, 2});
  EXPECT_EQ(r.value().y, 2);
}

TEST_F(ResultTest, NonTrivialLifetime) {
  Tracked::live = 0;
  {
    Result<Tracked> a{Tracked(1)};
    EXPECT_EQ(Tracked::live, 1);
    Result<Tracked> b = a;  // copy
    EXPECT_EQ(Tracked::live, 2);
    Result<Tracked> c = std::move(b);
    EXPECT_EQ(Tracked::live, 3);
    Result<Tracked> e = Status::kTimeout;
    EXPECT_EQ(Tracked::live, 3);
    e = a;  // error -> value
    EXPECT_EQ(Tracked::live, 4);
    EXPECT_EQ(e.value().v, 1);
    e = Result<Tracked>(Status::kCancelled);  // value -> error
    EXPECT_EQ(Tracked::live, 3);
    EXPECT_EQ(e.status(), Status::kCancelled);
    c = a;  // value -> value
    EXPECT_EQ(Tracked::live, 3);
  }
  EXPECT_EQ(Tracked::live, 0);
}

Status needs_positive(int x) { return x > 0 ? Status::kOk : Status::kInvalidArgument; }
Result<int> twice(int x) {
  ST_TRY(needs_positive(x));
  return 2 * x;
}
Result<int> plus_one_of_twice(int x) {
  ST_TRY_ASSIGN(const int t, twice(x));
  return t + 1;
}
Status discard_value(int x) {
  ST_TRY(twice(x));
  return Status::kOk;
}

TEST_F(ResultTest, TryMacros) {
  EXPECT_EQ(twice(3).value(), 6);
  EXPECT_EQ(twice(-1).status(), Status::kInvalidArgument);
  EXPECT_EQ(plus_one_of_twice(3).value(), 7);
  EXPECT_EQ(plus_one_of_twice(0).status(), Status::kInvalidArgument);
  EXPECT_EQ(discard_value(1), Status::kOk);
  EXPECT_EQ(discard_value(0), Status::kInvalidArgument);
}

#if ST_ENABLE_ASSERTS
TEST(ResultDeathTest, ValueOfErrorAsserts) {
  Result<int> r = Status::kNoMatch;
  EXPECT_DEATH(static_cast<void>(r.value()), "ok\\(\\)");
}
TEST(ResultDeathTest, ConstructFromOkAsserts) {
  EXPECT_DEATH(static_cast<void>(Result<int>{Status::kOk}), "kOk");
}
#endif
}  // namespace
