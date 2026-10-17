# Phase 1 design note: repo skeleton, build, CI, containers, no-alloc harness

Status: **draft, awaiting review**. Nothing in this phase is implemented yet.

Scope: everything later phases stand on. No star-tracker algorithms in this phase.

## 1. Repository layout

```
startracker-cpp/
  CMakeLists.txt  CMakePresets.json
  LICENSE (Apache-2.0)  NOTICE (attribution, see 00-overview.md §4)
  include/st/            public headers of the core library (namespace st)
    core/                status.hpp result.hpp assert.hpp arena.hpp clock.hpp
    containers/          fixed_vector.hpp span_vector.hpp ring_buffer.hpp
                         spsc_queue.hpp bitset.hpp
  src/core/              .cpp files of the core (few; most of phase 1 is header-only)
  tests/
    unit/                GoogleTest unit tests
    noalloc/             allocation-interposer library + its self-tests
    compile_fail/        "must not compile" checks (e.g. FixedVector<T,0> misuse)
  tools/                 (empty until phase 2) Python + C++ host tools
  docs/design/           these notes
  .github/workflows/ci.yml
  .clang-format  .clang-tidy
```

Only `include/st` + `src/core` form the core library (`st_core`). Everything else may use
exceptions, RTTI, heap, third-party libraries.

## 2. Core primitives

### 2.1 Status and Result

C++20 has no `std::expected` (it is C++23), and the GCC on JetPack 6 (GCC 11) does not ship it,
so we provide our own minimal type.

```cpp
namespace st {
enum class Status : std::uint16_t {
  kOk = 0,
  kInvalidArgument, kCapacityExceeded, kArenaExhausted, kNotInitialized,
  kCorruptData, kChecksumMismatch, kVersionMismatch, kUnsupported,
  kTooFewStars, kNoMatch, kTimeout, kIterationLimit, kCancelled,
  kIoError,       // only produced by host-side helpers / sources, never by pure algorithms
};
const char* to_string(Status) noexcept;  // static strings, no allocation

template <class T> class [[nodiscard]] Result {   // T or Status, never both
 public:
  Result(T v); Result(Status s);                  // s != kOk (asserted)
  bool ok() const noexcept;
  Status status() const noexcept;
  T& value() &;                                   // ST_ASSERT(ok())
  T value_or(T fallback) const;
  // no exceptions, no operator bool on purpose (forces explicit .ok())
};
}
```

Storage is a union + tag; `sizeof(Result<T>) <= sizeof(T) + alignof(T)`. A macro
`ST_TRY(x)` propagates errors. All public functions that can fail are `[[nodiscard]]`.

### 2.2 Assertions and the fault handler

* `ST_ASSERT(cond)`: internal invariants. Enabled in Debug and in a `-DST_ASSERTS=ON` release
  build. On failure calls `st::fault(file, line, msg)`.
* `st::fault` calls a user-installable handler (`set_fault_handler`, plain function pointer).
  The default handler writes to stderr and calls `std::abort()`. Flight software can install a
  handler that logs to telemetry and resets.
* Precondition violations by callers (bad sizes, null spans) return `Status::kInvalidArgument`
  rather than asserting, wherever the function already returns a `Result`/`Status`.

### 2.3 Arena

```cpp
class Arena {                        // bump allocator over caller-provided memory
 public:
  explicit Arena(std::span<std::byte> backing) noexcept;
  template <class T> Result<std::span<T>> allocate(std::size_t n) noexcept;  // value-init
  Marker mark() const noexcept;  void release(Marker) noexcept;   // scoped scratch
  void freeze() noexcept;        // further allocate() -> kArenaExhausted + ST_ASSERT
  std::size_t used() const, capacity() const, high_water() const;
};
```

* Two arenas per solver/pipeline instance: **persistent** (sized and filled during init, then
  frozen) and **scratch** (released to a marker at the start of every frame/solve, O(1)).
* `allocate<T>` requires `std::is_trivially_destructible_v<T>` (static_assert); the arena never
  runs destructors. Non-trivial objects are placed with `std::construct_at` by their owner.
* `high_water()` is reported so callers can size the backing buffer from a measured run.

### 2.4 Containers

All containers: no exceptions, no heap, `noexcept` everywhere, bounds-checked `operator[]`
under `ST_ASSERT`, `push_back`/`emplace_back` return `bool` (false when full, `[[nodiscard]]`).

| Type | Capacity | Storage | Use |
|---|---|---|---|
| `FixedVector<T, N>` | compile-time N | inline `alignas(T) std::byte[N*sizeof(T)]` | small fixed bounds (pattern of 4, top-K) |
| `SpanVector<T>` | run-time, set at init | non-owning `std::span<T>` from an arena | centroid lists, match lists, sized from config |
| `RingBuffer<T, N>` | compile-time, power of 2 | inline | single-thread history (rate estimation) |
| `SpscQueue<T>` | run-time, power of 2 | arena span | lock-free single-producer/single-consumer; frame-slot handoff in phase 6 |
| `BitSet<N>` / `BitSpan` | compile/run-time | inline / arena | keep-flags, hot-pixel maps |

`SpscQueue` design: head and tail indices as `std::atomic<std::uint32_t>` on separate
64-byte cache lines (`alignas(64)`; Cortex-A72/A76 and Carmel all use 64-byte lines), producer
caches the consumer index and vice versa to avoid cache-line ping-pong. Push: relaxed load of
own index, acquire load of the other, release store. Wait-free, O(1). Capacity `2^k`, one
slot sacrificed (usable `2^k - 1`), indices wrap with a mask. Tested under TSan and with a
two-thread stress test (10^7 items, checks order and no loss).

### 2.5 Clock

`struct Clock { virtual std::int64_t now_ns() const noexcept = 0; }` (virtual dispatch is
fine without RTTI). `SteadyClock` wraps `std::chrono::steady_clock`; `ManualClock` is for
deterministic tests. Solver deadlines (phase 3) take a `const Clock&` so timeouts are
testable and reproducible.

## 3. Build system

* CMake >= 3.21 (presets). Targets: `st_core` (static library), `st_tests`, later `st_tools`.
* Compilers: **GCC >= 11, Clang >= 14** (JetPack 6 / Ubuntu 22.04 ship GCC 11; Raspberry Pi
  OS bookworm ships GCC 12). AppleClang is supported for development only.
* Options:

| Option | Default | Effect |
|---|---|---|
| `ST_STRICT` | ON | core built with `-fno-exceptions -fno-rtti` |
| `ST_WERROR` | ON in CI | `-Werror` |
| `ST_ASSERTS` | ON in Debug | enable `ST_ASSERT` |
| `ST_SANITIZE` | empty | `address,undefined` or `thread` |
| `ST_BUILD_TESTS` / `ST_BUILD_TOOLS` / `ST_FUZZ` | ON / OFF / OFF | |

* Warnings on the core: `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
  -Wdouble-promotion -Wold-style-cast -Wcast-align -Wnull-dereference`.
  `-Wdouble-promotion` is there on purpose: it flags accidental float->double promotion,
  which matters given the float32-by-default rule.
* Floating point: **no `-ffast-math`**, and **`-ffp-contract=off`** on the core. GCC on
  aarch64 contracts `a*b+c` into FMA by default and GCC on x86-64 without `-mfma` does not,
  so without this flag the same input gives different bits on x86 and ARM64. We want
  identical results on both for parity and for regression tests. The cost will be measured in
  phase 6; if FMA turns out to matter it becomes an opt-in flag.

## 4. Enforcing the hard constraints automatically

Three independent checks, all run by `ctest` and in CI:

1. **Symbol audit of `st_core`** (CMake script using `nm`): the static library must not
   reference `__cxa_throw`, `__cxa_allocate_exception`, `__gxx_personality_v0`, `typeinfo`
   symbols, any `operator new`/`delete` (`_Znwm`, `_Znam`, `_ZnwmSt11align_val_t`, ...), or
   `malloc/calloc/realloc/free/posix_memalign/aligned_alloc`. This catches hidden heap use
   in the standard library too (for example `std::stable_sort` and `std::function` allocate).
   Exception: symbols explicitly allow-listed per file with a justification (expected: none).
2. **Strict-flags compile check**: every public header is compiled in its own TU with
   `-fno-exceptions -fno-rtti -Werror`, so headers stay usable by strict consumers.
3. **Runtime allocation guard** (`tests/noalloc`, a separate library linked only into test
   executables):
   * Replaces all global `operator new/delete` forms (plain, array, nothrow, sized, aligned).
   * On Linux/glibc, also interposes `malloc, calloc, realloc, free, posix_memalign,
     aligned_alloc, memalign, valloc` and forwards to `__libc_malloc` etc.
   * On macOS (dev only) the malloc family cannot be interposed without
     `DYLD_INSERT_LIBRARIES`; there the guard covers `operator new` only, plus a
     `malloc_zone_statistics` before/after count. CI's authoritative run is Linux.
   * API: `st::test::AllocGuard guard(Mode::kAbort | Mode::kCount);` global atomic mode, so
     it applies to every thread (the phase 6 pipeline threads included). A thread-local
     "exempt" flag lets the test framework itself allocate when reporting results.
   * Self-tests: a `std::vector` push inside a counting guard counts > 0; inside an abort
     guard it dies (GoogleTest death test); every container op in phase 1 runs inside an
     abort guard.

## 5. CI (GitHub Actions)

| Job | Runner | Compilers | Config |
|---|---|---|---|
| linux-x64 | ubuntu-24.04 | GCC 11, GCC 14, Clang 14, Clang 18 | Debug + Release, ST_STRICT, -Werror |
| linux-arm64 | ubuntu-24.04-arm (native) | GCC 11, Clang 18 | Release |
| macos-arm64 | macos-15 | AppleClang | Debug |
| asan-ubsan | ubuntu-24.04 | Clang 18 | ST_SANITIZE=address,undefined |
| tsan | ubuntu-24.04 | Clang 18 | ST_SANITIZE=thread (SPSC stress) |
| format | ubuntu-24.04 | clang-format 18 | check only |

Fuzzing jobs (libFuzzer, Clang) start in phase 2 (database loader) and phase 4 (centroider);
the `ST_FUZZ` CMake plumbing is created now.

GCC 11 is installed from the Ubuntu toolchain repository on the 24.04 runner so that the
oldest supported compiler (JetPack 6) is tested.

## 6. Memory and worst-case bounds (phase 1 components)

| Component | Memory | Worst case |
|---|---|---|
| `Result<T>` | `sizeof(T)` + tag, padded | O(1) |
| `Arena` | 4 words of bookkeeping; backing is caller's | `allocate`: O(1) + value-init O(n); `release`: O(1) |
| `FixedVector<T,N>` | `N*sizeof(T)` + size word | push/pop O(1); `clear` O(1) for trivially destructible T, else O(size) |
| `SpanVector<T>` | 2 words + backing span | as above |
| `RingBuffer<T,N>` | `N*sizeof(T)` + 2 words | O(1) |
| `SpscQueue<T>` | `cap*sizeof(T)` + 2 x 64 B (cache-line padded) + 2 cached-index words | push/pop O(1), wait-free |
| `BitSet<N>` | `ceil(N/64)*8` B | test/set O(1); `count`/`find_first` O(N/64) |
| alloc guard (tests only) | ~0 (atomics) | O(1) per hook |

## 7. Test framework choice

GoogleTest 1.15.x, fetched with CMake `FetchContent` at a pinned commit hash (tests only;
never linked into `st_core`). Reasons: death tests (needed for the abort guard), wide
familiarity, CTest integration. Alternative considered: doctest (lighter, no death tests).

## 8. Definition of done for phase 1

* `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` passes locally
  (macOS arm64, AppleClang) and on all CI jobs above.
* Unit tests cover every container/arena/result operation including full/empty edges, and
  all of them run inside an abort-mode allocation guard.
* Symbol audit and strict-header checks pass.
* TSan stress test of `SpscQueue` passes.
* One commit: "Phase 1: skeleton, build, CI, containers, no-alloc harness".
