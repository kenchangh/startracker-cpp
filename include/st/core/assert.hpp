// SPDX-License-Identifier: Apache-2.0
#pragma once

// Internal-invariant checks and the fault handler.
//
// ST_ASSERT(cond) checks internal invariants. It is enabled when ST_ENABLE_ASSERTS is 1
// (CMake option ST_ASSERTS; defaults to on unless NDEBUG). A failed check calls st::fault,
// which never returns.
//
// Caller mistakes on functions that already return Status/Result are reported as
// Status::kInvalidArgument, not asserted.

#ifndef ST_ENABLE_ASSERTS
#ifdef NDEBUG
#define ST_ENABLE_ASSERTS 0
#else
#define ST_ENABLE_ASSERTS 1
#endif
#endif

namespace st {

// Called on a fatal fault. Must not return normally; if it does, st::fault calls
// std::abort(). Flight software can install a handler that logs to telemetry and resets.
using FaultHandler = void (*)(const char* file, int line, const char* message) noexcept;

// Installs `handler` (nullptr restores the default) and returns the previous one.
// Thread-safe. Worst case: O(1).
FaultHandler set_fault_handler(FaultHandler handler) noexcept;

// Reports a fatal fault through the installed handler, then aborts.
// Default handler: one line to stderr, no allocation. Worst case: O(strlen(message)).
[[noreturn]] void fault(const char* file, int line, const char* message) noexcept;

}  // namespace st

#if ST_ENABLE_ASSERTS
#define ST_ASSERT(cond) \
  (static_cast<bool>(cond) ? static_cast<void>(0) : ::st::fault(__FILE__, __LINE__, #cond))
#else
// Not evaluated, but still type-checked.
#define ST_ASSERT(cond) static_cast<void>(sizeof(static_cast<bool>(cond)))
#endif
