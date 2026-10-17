// SPDX-License-Identifier: Apache-2.0
#include "st/core/assert.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace st {
namespace {

void default_fault_handler(const char* file, int line, const char* message) noexcept {
  // stderr is unbuffered: no allocation on this path.
  static_cast<void>(std::fprintf(stderr, "st: fault at %s:%d: %s\n", file != nullptr ? file : "?",
                                 line, message != nullptr ? message : "?"));
}

std::atomic<FaultHandler> g_handler{&default_fault_handler};

}  // namespace

FaultHandler set_fault_handler(FaultHandler handler) noexcept {
  return g_handler.exchange(handler != nullptr ? handler : &default_fault_handler,
                            std::memory_order_acq_rel);
}

void fault(const char* file, int line, const char* message) noexcept {
  g_handler.load(std::memory_order_acquire)(file, line, message);
  // The handler must not return; if it does, stop here.
  std::abort();
}

}  // namespace st
