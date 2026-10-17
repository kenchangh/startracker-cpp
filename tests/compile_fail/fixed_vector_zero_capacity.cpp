// EXPECT-ERROR: FixedVector capacity must be > 0
#include "st/containers/fixed_vector.hpp"
st::FixedVector<int, 0> v;
