# startracker-cpp

A flight-oriented C++20 star tracker library: the tetra3 lost-in-space algorithm plus a
tracking mode, for ARM64 embedded computers (Raspberry Pi, Jetson). Apache-2.0.

**Status: phase 1 of 7 (skeleton, build, CI, containers, no-alloc harness).** No star-tracking
algorithms yet. Design notes: [`docs/design/`](docs/design/).

Goals:
- Zero heap allocation after init, with documented worst-case bounds on every runtime function.
- A streaming pipeline over camera frames.
- Algorithmic parity with ESA tetra3; Cedar Solve's improvements as opt-in variants.

## Build and test

Requires CMake >= 3.21, Ninja, and GCC >= 11 or Clang >= 14 (Clang 14 needs libstdc++ <= 12).

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

Presets: `dev` (Debug, asserts, -Werror), `release`, `release-asserts`, `asan` (ASan+UBSan),
`tsan`. `tools/ci.sh <preset> [gcc-11|clang-18|...]` runs one configuration the way CI does.
`docker/Dockerfile` provides the oldest supported toolchain (Ubuntu 22.04, GCC 11, Clang 14).

## How the constraints are enforced

- `st_core` is built with `-fno-exceptions -fno-rtti` (`ST_STRICT=ON`).
- `symbol_audit.st_core` fails if the library references `operator new/delete`, the malloc
  family, exception runtime or typeinfo symbols (with a negative control proving it works).
- Every public header is compiled alone with strict flags and `-Werror`.
- `tests/noalloc` hooks every allocation in test processes (operator new and, on glibc, the
  malloc family); unit tests fail on any allocation and abort mode kills the process.

See [NOTICE](NOTICE) for attribution (tetra3, Cedar Solve, Tetra).
