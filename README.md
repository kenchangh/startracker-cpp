# startracker-cpp

A flight-oriented C++20 star tracker library for ARM64 embedded computers (Raspberry Pi,
Jetson) and x86-64. It combines ESA tetra3's lost-in-space algorithm with a tracking mode and
a streaming camera pipeline. Apache-2.0.

* **Lost-in-space solver** matching ESA tetra3 @ f9fa2eb: 404/404 parity cases, worst
  boresight difference 0.0016″. Cedar Solve's changes are available as opt-in variants
  (303/303 parity cases).
* **Centroider** matching tetra3's `get_centroids_from_image` within 6e-5 px on 101 images.
* **Tracker**: predicted-star matching, Wahba/SVD attitude, QUEST covariance, windowed
  rate estimate, ROI centroiding, gyro hook, and lost-in-space fallback.
* **Pipeline**: frame slots on lock-free SPSC queues, single-threaded or threaded,
  three drop policies, per-frame timestamps and latency. Sources: replay files and V4L2.
* **Interfaces**: C++ core, C API (`libstartracker`), and Python bindings (ctypes).
* **Database tools**: converts tetra3 `.npz` databases, or generates new ones from
  Hipparcos or BSC5 (pattern sets identical to tetra3's).

## Runtime guarantees

* No heap after initialisation. All memory comes from a caller-provided arena; `st_core` and
  `libstartracker` contain no allocation, exception or RTTI symbols (checked in CI with `nm`).
* No exceptions or RTTI in the core (`-fno-exceptions -fno-rtti`). Errors are `st::Status`
  or `st::Result<T>` values.
* Every runtime function documents its worst-case bound. The lost-in-space search also
  takes a deterministic work budget and an optional deadline.
* The core depends only on the C++ standard library. POSIX pieces (mmap, threads, V4L2)
  live in `st_platform`.

## Measured performance

Measured on an Intel Xeon @ 2.80 GHz (4 vCPU container), GCC 11 -O3, ESA database, rendered
1280×960 frames, 14° FOV:

| step | median | p99 |
|---|---|---|
| full-frame centroiding | 15.5 ms | 22.5 ms |
| centroiding 32 ROIs while tracking | 0.08 ms | 0.16 ms |
| lost-in-space solve (tetra3: 15.8 ms median on the same host) | 0.28 ms | 0.37 ms |
| tracker update | 0.02 ms | 0.08 ms |

Run `st_bench` on your hardware; it prints the CPU, compiler and memory use with the numbers.
CI runs it on hosted x64 and arm64 runners. See
[docs/design/06](docs/design/06-phase6-pipeline.md).

## Build and test

Requires CMake >= 3.21, Ninja, and GCC >= 11 or Clang >= 14.

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
python3 tools/fetch_fixtures.py --dbtool build/dev/tools/st_dbtool   # databases + images (SHA-256 pinned)
```

* **Presets**:
  * `dev`: Debug, asserts, -Werror.
  * `release`, `release-asserts`.
  * `asan`: ASan + UBSan.
  * `tsan`.
  * `fuzz`: Clang libFuzzer.
* **Scripts and containers**: `tools/ci.sh <preset> [compiler]` reproduces one CI
  configuration. `docker/Dockerfile` provides the oldest supported toolchain.
* **Fixtures**: they are never committed. Tests that need them skip when they are missing.

## Using it

See [docs/usage.md](docs/usage.md) for the C++, C and Python APIs, the tools, the file
formats and the coordinate conventions. A minimal C example:

```c
st_config cfg; st_config_init(&cfg);
cfg.camera = st_camera_ideal(1280, 960, 11.4);
size_t need = st_tracker_memory_size(&cfg, db, db_size);       /* db: mapped .stdb file */
st_tracker* t; st_tracker_create(&cfg, db, db_size, aligned_alloc(64, need), need, &t);
st_solution s; st_track_image(t, t_ns, pixels, 1280, 960, 0, ST_MONO16, &s);
```

## Verification

* Unit tests run in the allocation guard. Test processes fail on any heap allocation,
  including inside the threaded pipeline.
* Parity against ESA tetra3 and cedar-solve, in pinned Python environments:
  * generator;
  * solver on tetra3 centroids;
  * images → centroids → solution, end to end.
* Real sky: 23 photographs plate-solved by astrometry.net, checked against our solutions.
* Synthetic sweeps over FOV, sensitivity, read noise and rate.
* A sidereal-rotation test on rendered frames.
* ASan/UBSan and TSan runs; libFuzzer runs on the database loader and the centroider.
* Static checks: clang-tidy, clang-format, strict per-header compiles, symbol audit.

Design notes for every phase are in [docs/design/](docs/design/); tetra3's quirks are
listed in note 03.

See [NOTICE](NOTICE) for attribution (tetra3, Cedar Solve, Tetra).
