// EXPECT-ERROR: Arena never runs destructors
#include "st/core/arena.hpp"
struct NonTrivial {
  ~NonTrivial() {}
};
void f(st::Arena& a) { static_cast<void>(a.allocate<NonTrivial>(1)); }
