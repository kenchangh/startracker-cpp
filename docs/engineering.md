# What's inside, and how it is checked

## Components

* **Lost-in-space solver:** a port of ESA tetra3 (@ f9fa2eb).
  * Parity: 404/404 parity cases; the worst boresight difference is 0.0016″.
  * Search limits: a deterministic work budget and an optional deadline.
  * cedar-solve's search is an opt-in variant, from C++, C and Python (303/303 parity cases).
* **Centroider:** matches tetra3's `get_centroids_from_image` within 6e-5 px on 101 images.
  It also has hot-pixel maps, trail rejection, and a windowed (ROI) mode for tracking.
* **Tracker:**
  * matches predicted stars and fits the attitude (Wahba, SVD);
  * reports an attitude covariance and an estimated rotation rate;
  * has a gyro hook, and falls back to lost-in-space when it loses lock;
  * uses a calibrated camera model with lens distortion.
* **Pipeline:**
  * acquire → centroid → solve, single-threaded or one thread per stage;
  * lock-free queues, three drop policies, and per-frame timestamps;
  * frame sources: replay files and V4L2 cameras.
* **Databases:** converts tetra3/cedar-solve `.npz` files, or generates new databases from
  Hipparcos or the Bright Star Catalogue. Generated pattern sets are identical to tetra3's.
* **Tools:**
  * `st_dbtool`: database generate, convert, info and verify;
  * `st_render`: synthetic star fields;
  * `st_sweep`: accuracy sweeps;
  * `st_replay`: recorded sequences;
  * `st_bench`: latency and memory.

## Runtime guarantees

* **No heap after initialisation.**
  * All memory comes from a caller-provided arena.
  * `st_core` and `libstartracker` contain no allocation, exception or RTTI symbols; CI
    checks this with `nm`.
  * Unit tests fail on any heap allocation, including inside the multi-threaded pipeline.
* **No exceptions or RTTI.** The core builds with `-fno-exceptions -fno-rtti`, and errors are
  returned as status codes.
* **Bounded cost:** every runtime function documents its worst-case cost.
* **Dependencies:** the core depends only on the C++ standard library. POSIX pieces (mmap,
  threads, V4L2) are kept in `st_platform`.

## Verification

* **Parity against ESA tetra3 and cedar-solve:** the database generator, the solver on
  tetra3's own centroids, and the full image → centroids → solution chain.
* **Real sky:** 23 photographs plate-solved by astrometry.net in CI. Every solution we report
  agrees with it (centre within about 1 px).
* **Synthetic tests:** sweeps over field of view, sensitivity, noise and rotation rate, and
  a sidereal-rotation test on rendered frames.
* **Robustness:** ASan/UBSan and TSan runs, fuzzing of the database loader and the
  centroider, clang-tidy, and strict per-header compiles.
* **Platforms:** CI runs on x86-64 and arm64 Linux (GCC 11–14, Clang 14–18) and on macOS.

## Building and testing

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release
python3 tools/fetch_fixtures.py --dbtool build/release/tools/st_dbtool   # databases + test images
```

* **Presets:** `dev`, `release`, `release-asserts`, `asan` (ASan + UBSan), `tsan` and `fuzz`.
* **Fixtures:** test data is downloaded with SHA-256 checks and never committed. Tests that
  need it skip when it is missing.

Design notes for every phase are in [design/](design/); tetra3's quirks are documented in
note 03.
