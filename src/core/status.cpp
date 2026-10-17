// SPDX-License-Identifier: Apache-2.0
#include "st/core/status.hpp"

namespace st {

const char* to_string(Status status) noexcept {
  switch (status) {
    case Status::kOk: return "kOk";
    case Status::kInvalidArgument: return "kInvalidArgument";
    case Status::kCapacityExceeded: return "kCapacityExceeded";
    case Status::kArenaExhausted: return "kArenaExhausted";
    case Status::kNotInitialized: return "kNotInitialized";
    case Status::kCorruptData: return "kCorruptData";
    case Status::kChecksumMismatch: return "kChecksumMismatch";
    case Status::kVersionMismatch: return "kVersionMismatch";
    case Status::kUnsupported: return "kUnsupported";
    case Status::kTooFewStars: return "kTooFewStars";
    case Status::kNoMatch: return "kNoMatch";
    case Status::kTimeout: return "kTimeout";
    case Status::kWorkBudgetExhausted: return "kWorkBudgetExhausted";
    case Status::kCancelled: return "kCancelled";
    case Status::kIoError: return "kIoError";
  }
  return "kUnknown";
}

}  // namespace st
