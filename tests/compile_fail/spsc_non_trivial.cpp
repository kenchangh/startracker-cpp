// EXPECT-ERROR: SpscQueue: T must be trivially copyable
#include "st/containers/spsc_queue.hpp"
struct NonTrivial {
  NonTrivial(const NonTrivial&) {}
};
st::SpscQueue<NonTrivial> q;
