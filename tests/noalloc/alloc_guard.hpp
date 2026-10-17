// SPDX-License-Identifier: Apache-2.0
#pragma once

// Test-only allocation guard. Link the `st_alloc_guard` object library into a test
// executable; it hooks every heap allocation in the process:
//   * Linux/glibc, no sanitizer: replaces operator new/delete (all forms) and interposes
//     malloc, calloc, realloc, reallocarray, posix_memalign, aligned_alloc, memalign, valloc,
//     pvalloc, forwarding to glibc's __libc_* functions.
//   * ASan / TSan: uses the sanitizer allocator hook (the sanitizer owns malloc).
//   * Elsewhere (macOS dev builds): replaces operator new/delete only.
// Never linked into st_core.

#include <cstdint>

namespace st::test {

enum class AllocMode : unsigned {
  kOff = 0,
  kCount = 1,  // count allocations
  kAbort = 2,  // count, then print a message and abort on the first allocation
};

// Which hooking mechanism this build uses ("glibc-interpose", "sanitizer-hook", "new-only").
const char* alloc_guard_backend() noexcept;
// True if malloc-family calls (not only operator new) are hooked in this build.
bool alloc_guard_hooks_malloc() noexcept;
// False under TSan, whose aligned_alloc bypasses the sanitizer hooks. TSan builds are for race
// checking; the authoritative no-alloc runs are the non-sanitizer Linux builds.
bool alloc_guard_hooks_aligned_alloc() noexcept;

// Total allocations seen while a guard was active (process-wide, all threads).
std::uint64_t allocation_count() noexcept;

// Sets the process-wide mode for its lifetime and restores the previous mode on exit.
// Applies to every thread, so worker threads are covered too.
class AllocGuard {
 public:
  explicit AllocGuard(AllocMode mode) noexcept;
  AllocGuard(const AllocGuard&) = delete;
  AllocGuard& operator=(const AllocGuard&) = delete;
  ~AllocGuard();

  // Allocations seen since this guard was created.
  [[nodiscard]] std::uint64_t allocations() const noexcept;

 private:
  AllocMode previous_;
  std::uint64_t start_;
};

// Exempts the current thread while alive (e.g. for test-framework reporting).
class AllocExempt {
 public:
  AllocExempt() noexcept;
  AllocExempt(const AllocExempt&) = delete;
  AllocExempt& operator=(const AllocExempt&) = delete;
  ~AllocExempt();
};

}  // namespace st::test
