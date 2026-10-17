// EXPECT-ERROR: ignoring return value|unused-result|nodiscard
#include "st/core/result.hpp"
st::Result<int> g();
void f() { g(); }
