// EXPECT-ERROR: RingBuffer capacity must be a power of two
#include "st/containers/ring_buffer.hpp"
st::RingBuffer<int, 3> r;
