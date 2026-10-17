// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace st {

// Error codes returned by every fallible function. kOk is the only success value.
enum class Status : std::uint8_t {
  kOk = 0,
  kInvalidArgument,
  kCapacityExceeded,
  kArenaExhausted,
  kNotInitialized,
  kCorruptData,
  kChecksumMismatch,
  kVersionMismatch,
  kUnsupported,
  kTooFewStars,
  kNoMatch,
  kTimeout,              // optional wall-clock deadline expired
  kWorkBudgetExhausted,  // deterministic work budget used up
  kCancelled,
  kIoError,  // only from host-side helpers and frame sources
};

// Static name of a status, e.g. "kNoMatch". Never allocates. Worst case: O(1).
const char* to_string(Status status) noexcept;

}  // namespace st
