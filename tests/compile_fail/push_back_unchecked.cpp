// EXPECT-ERROR: ignoring return value
#include "st/containers/fixed_vector.hpp"
void f(st::FixedVector<int, 4>& v) { v.push_back(1); }
