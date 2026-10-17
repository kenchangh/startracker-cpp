// SPDX-License-Identifier: Apache-2.0
#include "alloc_guard.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

// ---- Backend selection --------------------------------------------------------------------

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ST_AG_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define ST_AG_SANITIZER 1
#endif
#endif

#if defined(__SANITIZE_THREAD__)
#define ST_AG_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ST_AG_TSAN 1
#endif
#endif

#if defined(ST_AG_SANITIZER)
#define ST_AG_BACKEND_SANITIZER 1
// From <sanitizer/allocator_interface.h>, declared here because GCC does not ship the header.
extern "C" void __sanitizer_print_stack_trace();
extern "C" int __sanitizer_install_malloc_and_free_hooks(void (*malloc_hook)(const volatile void*,
                                                                             std::size_t),
                                                         void (*free_hook)(const volatile void*));
#elif defined(__GLIBC__)
#define ST_AG_BACKEND_GLIBC 1
extern "C" {
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void __libc_free(void*);
void* __libc_memalign(std::size_t, std::size_t);
}
#else
#define ST_AG_BACKEND_NEW_ONLY 1
#endif

namespace st::test {
namespace {

std::atomic<unsigned> g_mode{0};
std::atomic<std::uint64_t> g_count{0};
// initial-exec: reading the TLS variable must never allocate (we are inside malloc).
#if defined(__GNUC__) && !defined(__APPLE__)
__attribute__((tls_model("initial-exec")))
#endif
constinit thread_local unsigned t_exempt = 0;

void write_stderr(const char* s) noexcept {
#if defined(__unix__) || defined(__APPLE__)
  ssize_t r = ::write(2, s, std::strlen(s));
  static_cast<void>(r);
#else
  static_cast<void>(s);
#endif
}

// ST_ALLOC_GUARD_TRACE=1 prints a stack trace for every counted allocation (ASan builds
// only; under TSan the symbolizer re-enters the allocator and hangs). Read once at static-init
// time.
const bool g_trace = [] {
  const char* e = std::getenv("ST_ALLOC_GUARD_TRACE");
  return e != nullptr && e[0] == '1';
}();

inline void on_alloc() noexcept {
  const unsigned mode = g_mode.load(std::memory_order_relaxed);
  if (mode == 0 || t_exempt != 0) return;
  g_count.fetch_add(1, std::memory_order_relaxed);
#if defined(ST_AG_BACKEND_SANITIZER) && !defined(ST_AG_TSAN)
  if (g_trace) {
    write_stderr("st::test::AllocGuard: counted allocation:\n");
    __sanitizer_print_stack_trace();
  }
#endif
  if (mode == static_cast<unsigned>(AllocMode::kAbort)) {
    t_exempt = 1;
    write_stderr("st::test::AllocGuard: heap allocation inside a no-alloc region\n");
    std::abort();
  }
}

#if defined(ST_AG_BACKEND_SANITIZER)
void sanitizer_malloc_hook(const volatile void*, std::size_t) { on_alloc(); }
void sanitizer_free_hook(const volatile void*) {}
// Installed at static-init time.
const int g_hook_installed =
    __sanitizer_install_malloc_and_free_hooks(&sanitizer_malloc_hook, &sanitizer_free_hook);
#endif

#if !defined(ST_AG_BACKEND_SANITIZER)
// Raw allocation used by the operator new replacements (already counted).
void* raw_alloc(std::size_t n, std::size_t align) noexcept {
  if (n == 0) n = 1;
#if defined(ST_AG_BACKEND_GLIBC)
  return align <= alignof(std::max_align_t) ? __libc_malloc(n) : __libc_memalign(align, n);
#else
  if (align <= alignof(std::max_align_t)) return std::malloc(n);
  void* p = nullptr;
  if (align < sizeof(void*)) align = sizeof(void*);
  return ::posix_memalign(&p, align, n) == 0 ? p : nullptr;
#endif
}
void raw_free(void* p) noexcept {
#if defined(ST_AG_BACKEND_GLIBC)
  __libc_free(p);
#else
  std::free(p);
#endif
}
#endif

}  // namespace

const char* alloc_guard_backend() noexcept {
#if defined(ST_AG_BACKEND_SANITIZER)
  return "sanitizer-hook";
#elif defined(ST_AG_BACKEND_GLIBC)
  return "glibc-interpose";
#else
  return "new-only";
#endif
}

bool alloc_guard_hooks_malloc() noexcept {
#if defined(ST_AG_BACKEND_SANITIZER)
  return g_hook_installed != 0;
#elif defined(ST_AG_BACKEND_GLIBC)
  return true;
#else
  return false;
#endif
}

bool alloc_guard_hooks_aligned_alloc() noexcept {
#if defined(ST_AG_TSAN)
  return false;  // TSan's aligned_alloc does not run the sanitizer malloc hooks.
#else
  return alloc_guard_hooks_malloc();
#endif
}

std::uint64_t allocation_count() noexcept { return g_count.load(std::memory_order_relaxed); }

AllocGuard::AllocGuard(AllocMode mode) noexcept
    : previous_(static_cast<AllocMode>(g_mode.load(std::memory_order_relaxed))),
      start_(g_count.load(std::memory_order_relaxed)) {
  g_mode.store(static_cast<unsigned>(mode), std::memory_order_seq_cst);
}
AllocGuard::~AllocGuard() {
  g_mode.store(static_cast<unsigned>(previous_), std::memory_order_seq_cst);
}
std::uint64_t AllocGuard::allocations() const noexcept {
  return g_count.load(std::memory_order_relaxed) - start_;
}

AllocExempt::AllocExempt() noexcept { ++t_exempt; }
AllocExempt::~AllocExempt() { --t_exempt; }

}  // namespace st::test

// ---- operator new / delete ----------------------------------------------------------------

#if !defined(ST_AG_BACKEND_SANITIZER)
namespace {
void* counted_new(std::size_t n, std::size_t align) {
  st::test::on_alloc();
  void* p = st::test::raw_alloc(n, align);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* counted_new_nothrow(std::size_t n, std::size_t align) noexcept {
  st::test::on_alloc();
  return st::test::raw_alloc(n, align);
}
constexpr std::size_t kDefaultAlign = alignof(std::max_align_t);
}  // namespace

void* operator new(std::size_t n) { return counted_new(n, kDefaultAlign); }
void* operator new[](std::size_t n) { return counted_new(n, kDefaultAlign); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  return counted_new_nothrow(n, kDefaultAlign);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  return counted_new_nothrow(n, kDefaultAlign);
}
void* operator new(std::size_t n, std::align_val_t a) {
  return counted_new(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
  return counted_new(n, static_cast<std::size_t>(a));
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return counted_new_nothrow(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return counted_new_nothrow(n, static_cast<std::size_t>(a));
}

void operator delete(void* p) noexcept { st::test::raw_free(p); }
void operator delete[](void* p) noexcept { st::test::raw_free(p); }
void operator delete(void* p, std::size_t) noexcept { st::test::raw_free(p); }
void operator delete[](void* p, std::size_t) noexcept { st::test::raw_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { st::test::raw_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { st::test::raw_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { st::test::raw_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { st::test::raw_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { st::test::raw_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { st::test::raw_free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  st::test::raw_free(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  st::test::raw_free(p);
}
#endif

// ---- malloc family (glibc only) -----------------------------------------------------------

#if defined(ST_AG_BACKEND_GLIBC)
extern "C" {

void* malloc(std::size_t n) noexcept {
  st::test::on_alloc();
  return __libc_malloc(n);
}
void* calloc(std::size_t n, std::size_t s) noexcept {
  st::test::on_alloc();
  return __libc_calloc(n, s);
}
void* realloc(void* p, std::size_t n) noexcept {
  st::test::on_alloc();
  return __libc_realloc(p, n);
}
void* reallocarray(void* p, std::size_t n, std::size_t s) noexcept {
  st::test::on_alloc();
  std::size_t total = 0;
  if (__builtin_mul_overflow(n, s, &total)) {
    errno = ENOMEM;
    return nullptr;
  }
  return __libc_realloc(p, total);
}
void free(void* p) noexcept { __libc_free(p); }
void* memalign(std::size_t a, std::size_t n) noexcept {
  st::test::on_alloc();
  return __libc_memalign(a, n);
}
void* aligned_alloc(std::size_t a, std::size_t n) noexcept {
  st::test::on_alloc();
  return __libc_memalign(a, n);
}
int posix_memalign(void** out, std::size_t a, std::size_t n) noexcept {
  st::test::on_alloc();
  if (a < sizeof(void*) || (a & (a - 1)) != 0) return EINVAL;
  void* p = __libc_memalign(a, n);
  if (p == nullptr) return ENOMEM;
  *out = p;
  return 0;
}
void* valloc(std::size_t n) noexcept {
  st::test::on_alloc();
  return __libc_memalign(static_cast<std::size_t>(::sysconf(_SC_PAGESIZE)), n);
}
void* pvalloc(std::size_t n) noexcept {
  st::test::on_alloc();
  const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  return __libc_memalign(page, (n + page - 1) & ~(page - 1));
}

}  // extern "C"
#endif
