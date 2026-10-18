# startracker-cpp

A C++20 star tracker for Raspberry Pi, other ARM64 boards and x86-64. Point a camera at the
night sky and it tells you which way the camera is facing (a full 3-D attitude), many times
per second.

* **Lost in space:** it identifies stars with no prior knowledge, using ESA's tetra3
  algorithm. It gives the same answers as tetra3, a few times faster.
* **Tracking:** once it knows where it is pointing, it follows the sky frame to frame by
  looking only at small windows around the predicted stars. In the C++ benchmark this costs
  about 0.1 ms per frame on a desktop CPU (0.3 ms through Python).
* **Flight-style engineering:** no heap allocation after start-up, no exceptions, and a
  documented worst-case cost for every runtime function.
* **Interfaces:** C++, C (`libstartracker`) and Python.

## Run it on a Raspberry Pi

One command installs the packages, builds, downloads the right star database, and starts
tracking with your camera:

```sh
git clone https://github.com/kenchangh/startracker-cpp.git && cd startracker-cpp
scripts/raspi/run.sh --camera hq-16mm
```

* **Cameras:** `hq-12mm`, `hq-16mm`, `hq-25mm` (HQ Camera), `gs-12mm`, `gs-16mm` (Global
  Shutter Camera), `cm3` (Camera Module 3) and `cm2` (Camera Module 2). For any other lens,
  pass `--fov <degrees>`.
* **Useful options:**
  * `--exposure-ms 200 --gain 16` for exposure;
  * `--csv attitude.csv` to log each frame;
  * `--save-pgm frames/` to record frames;
  * `--bench` to measure speed on your Pi;
  * `--from-pgm frames/` to replay recorded frames without a camera.
* **Status:** not yet run on a Raspberry Pi or a real Pi camera. Everything around it is
  tested: arm64 CI, the capture code on simulated frames, and every camera preset on rendered
  star fields.

The full guide is in [docs/raspberry-pi.md](docs/raspberry-pi.md). It covers which lens to
pick, lens calibration, memory, real-time settings, and using C/C++ instead of Python.

## Benchmark: vs ESA's tetra3 and cedar-solve

![startracker-cpp vs ESA tetra3 and cedar-solve: about 5x faster on photos that solve, 16x faster than tetra3 on photos with no match, same answers](docs/images/bench_vs_tetra3.png)

The test set is 23 real night-sky photographs. Each photo is processed start to finish: find
the stars in the image, then identify them. startracker-cpp is compared with each library on
that library's own star database, and gives the same answer on every photo.

| median per photo | ESA tetra3 (Python) | startracker-cpp | speed-up |
|---|---|---|---|
| **12 photos that solve** | **16.4 ms** | **3.3 ms** | **4.9×** |
| └ finding stars | 14.9 ms | 3.3 ms | 4.6× |
| └ identifying them | 1.4 ms | 0.05 ms | 28× |
| **11 photos with no match** (worst case: searches everything) | **35.2 ms** | **2.2 ms** | **16×** |

| median per photo | cedar-solve (Python) | startracker-cpp, `variant="cedar"` | speed-up |
|---|---|---|---|
| **19 photos that solve** | **15.4 ms** | **3.3 ms** | **4.7×** |
| └ finding stars | 14.7 ms | 3.2 ms | 4.6× |
| └ identifying them | 0.96 ms | 0.05 ms | 21× |
| **2 photos with no match, where cedar-solve gives up** at its 5 s timeout | **5006 ms** (stopped) | **82 ms** (finished) | **at least 61×** |

* **How it was measured:**
  * Apple M3 Max, one thread each. Each number is the median of 21 runs per photo.
  * Each pair shares one database: ESA's for tetra3, cedar-solve's for cedar-solve.
    cedar-solve cannot load ESA's database.
  * Both libraries find stars with their own Python centroider. cedar-solve can also use a
    separate star finder, cedar-detect, which was not tested.
  * startracker-cpp was called through its Python bindings, so the call overhead counts
    against it.
* **cedar-solve's timeout:** cedar-solve stops searching after 5 s by default; ESA tetra3 has
  no limit.
  * On the 2 photos above it stopped without finishing. Left to run, it took 43.6 s on one
    and crashed on the other (a cedar-solve indexing bug).
  * Its other 2 no-match photos have only 5 and 7 stars, so there is little to search:
    37.4 ms for cedar-solve, 8.3 ms for startracker-cpp.
* **cedar-solve's search:** with cedar-solve's database, pass `variant="cedar"`
  (`ST_VARIANT_CEDAR` in C). It solves 19 of the 23 photos; ESA's search on the same database
  solves 14.
* **Where the time goes:** finding the stars in the image is most of a frame. While
  tracking, the library skips that full-image search and looks only at small windows
  around the predicted stars (about 0.1 ms per frame).
* **On arm64 Linux:** a GitHub-hosted Arm Neoverse N2 runner took 10.9 ms for a full 1280×960
  frame and 0.03 ms for tracking windows (synthetic database).
* **Raspberry Pi:** no numbers yet. Run `scripts/raspi/run.sh --camera <yours> --bench`.
* **Reproduce:**
  * `tests/parity/speed_vs_tetra3.py` runs both comparisons;
  * `docs/images/make_bench_chart.py --font xkcd-script.ttf` draws the chart;
  * the raw numbers are in `docs/images/bench_vs_tetra3.json`.

## What's inside

* **Lost-in-space solver:** a port of ESA tetra3 (@ f9fa2eb).
  * Parity: 404/404 parity cases; the worst boresight difference is 0.0016″.
  * Search limits: a deterministic work budget and an optional deadline.
  * Cedar Solve's search is available as an opt-in variant, from C++, C and Python
    (303/303 parity cases).
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
* **Databases:** converts tetra3/cedar `.npz` files, or generates new databases from
  Hipparcos or the Bright Star Catalogue. Generated pattern sets are identical to tetra3's.
* **Tools:**
  * `st_dbtool`: database generate, convert, info and verify;
  * `st_render`: synthetic star fields;
  * `st_sweep`: accuracy sweeps;
  * `st_replay`: recorded sequences;
  * `st_bench`: latency and memory.

## Build from source (desktop / CI)

Requires CMake ≥ 3.21, Ninja, and GCC ≥ 11 or Clang ≥ 14.

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release
python3 tools/fetch_fixtures.py --dbtool build/release/tools/st_dbtool   # databases + test images
```

* **Presets:** `dev`, `release`, `release-asserts`, `asan` (ASan + UBSan), `tsan` and `fuzz`.
* **Fixtures:** test data is downloaded with SHA-256 checks and never committed. Tests that
  need it skip when it is missing.

## Using the library

**Python:**

```python
import startracker as st   # PYTHONPATH=python, STARTRACKER_LIB=build/release/src/libstartracker.so
t = st.Tracker(open("fixtures/cedar_default.stdb", "rb").read(), st.Camera.ideal(1280, 960, 14.0),
               variant="cedar")   # cedar-solve's search, for its database
sol = t.track_image(image_uint16, t_ns=0)   # Solution(OK, lost_in_space, ra=..., dec=..., roll=...)
```

**C:**

```c
st_config cfg; st_config_init(&cfg);
cfg.camera = st_camera_ideal(1280, 960, 14.0);
size_t need = st_tracker_memory_size(&cfg, db, db_size);       /* db: the .stdb file in memory */
st_tracker* t; st_tracker_create(&cfg, db, db_size, aligned_alloc(64, need), need, &t);
st_solution s; st_track_image(t, t_ns, pixels, 1280, 960, 0, ST_MONO16, &s);
```

The C++ API, the file formats and the coordinate conventions are in
[docs/usage.md](docs/usage.md).

## Runtime guarantees and how they are checked

* **No heap after initialisation.**
  * All memory comes from a caller-provided arena.
  * `st_core` and `libstartracker` contain no allocation, exception or RTTI symbols; CI
    checks this with `nm`.
  * Unit tests fail on any heap allocation, including inside the multi-threaded pipeline.
* **No exceptions or RTTI.** The core builds with `-fno-exceptions -fno-rtti`, and errors are
  returned as status codes.
* **Dependencies:** the core depends only on the C++ standard library. POSIX pieces (mmap,
  threads, V4L2) are kept in `st_platform`.

## Verification

* **Parity against ESA tetra3 and cedar-solve:** the database generator, the solver on
  tetra3's own centroids, and the full image → centroids → solution chain.
* **Real sky:** 23 photographs plate-solved by astrometry.net. Every solution we report
  agrees with it (centre within about 1 px).
* **Synthetic tests:** sweeps over field of view, sensitivity, noise and rotation rate, and
  a sidereal-rotation test on rendered frames.
* **Robustness:** ASan/UBSan and TSan runs, fuzzing of the database loader and the
  centroider, clang-tidy, and strict per-header compiles.
* **Platforms:** CI runs on x86-64 and arm64 Linux (GCC 11–14, Clang 14–18) and on macOS.

Design notes for every phase are in [docs/design/](docs/design/); tetra3's quirks are
documented in note 03.

## License

Apache-2.0. See [NOTICE](NOTICE) for attribution: tetra3 (ESA), Cedar Solve, and Tetra.
