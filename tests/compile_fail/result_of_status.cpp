// EXPECT-ERROR: Result<Status> is meaningless
#include "st/core/result.hpp"
st::Result<st::Status> r{st::Status::kNoMatch};
