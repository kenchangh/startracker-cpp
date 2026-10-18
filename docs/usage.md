# Using startracker-cpp

## 1. Get a database

A database (`.stdb`) holds the star table and the pattern hash table. You get one in either
of two ways:

```sh
# Convert a tetra3 / cedar-solve .npz database
python3 tools/convert_tetra3_npz.py default_database.npz esa_default.stdb --dbtool build/release/tools/st_dbtool

# Or generate one (Hipparcos hip_main.dat or BSC5), with tetra3's parameters
build/release/tools/st_dbtool generate --catalog hip_main.dat --max-fov 30 --min-fov 10 \
    --mag 7 --epoch 2026 -o mydb.stdb          # add --lattice --linear-probe ... for the cedar variant
build/release/tools/st_dbtool info mydb.stdb   # parameters; `verify` checks CRCs and indices
```

* **Format**: a 256-byte header, then sections aligned to 64 bytes, each protected by a
  CRC-32C. Star vectors are float32. See `include/st/db/format.hpp`.
* **Loading**: memory-map the file (`st::MappedFile`). It is used in place with no copy.

## 2. Conventions

* **Pixels**: the origin is the top-left corner of the image, x runs right, y runs down.
  The centre of the first pixel is (0.5, 0.5). This matches tetra3.
* **Camera frame**: +Z is the boresight, +X points right, +Y points down.
* **Attitude**: `q_cam_icrs` (w, x, y, z) rotates ICRS unit vectors into the camera frame.
  It propagates as q(t+dt) = exp(−ω·dt) ⊗ q(t), with ω in the camera frame.
* **RA/Dec/roll**: in degrees, using tetra3's definitions, so outputs compare directly with
  tetra3.
* **Covariance**: a small-angle attitude error covariance in the camera frame, in rad².

## 3. C++

```cpp
#include "st/centroid/centroider.hpp"
#include "st/platform/mapped_file.hpp"
#include "st/track/tracker.hpp"

auto file = st::MappedFile::open("esa_default.stdb", /*prefault=*/true);
auto db = st::Database::open(file.value().bytes());
std::vector<std::byte> mem(64 << 20);            // or a static buffer; sized via workspace_bytes()
st::Arena arena(mem);
auto lis = st::LostInSpaceSolver::create(db.value(), st::SolverLimits{}, arena);
auto index = st::SkyIndex::create(db.value(), 16, arena);
st::TrackerConfig tc;
tc.camera = st::CameraModel::ideal(1280, 960, 11.4 * st::kDegToRad);  // or calibrated intrinsics
auto tracker = st::Tracker::create(index.value(), &lis.value(), tc, arena);
st::CentroiderConfig cc; cc.max_width = 1280; cc.max_height = 960;
auto cent = st::Centroider::create(cc, arena);
arena.freeze();                                   // initialisation done; no more allocation

// per frame (never allocates):
std::array<st::Spot, 64> spots; std::array<st::Centroid, 64> cs;
auto n = cent.value().extract(st::ImageView{pixels, 1280, 960, 0, st::PixelFormat::kMono16}, spots);
for (uint32_t i = 0; i < n.value(); ++i) cs[i] = {spots[i].x, spots[i].y};
st::AttitudeSolution sol;
tracker.value().update(t_ns, std::span(cs.data(), n.value()), sol);
```

* **Lost-in-space only**: call `LostInSpaceSolver::solve(centroids, w, h, SolveOptions, out)`.
  * `SolveOptions` mirrors tetra3's `solve_from_image` arguments, plus a `WorkBudget`, a
    deadline and a cancel flag.
  * `SolverVariants::cedar()` turns on Cedar Solve's changes.
* **Streaming**: `st::Pipeline` (`include/st/pipeline/pipeline.hpp`) runs a `FrameSource`
  through the stages.
  * For one thread, call `step()` yourself.
  * For three threads, use `st::PipelineRunner`, which sets priority and affinity.
  * Outputs come from `poll()` or a real-time-safe callback.

## 4. C API (`libstartracker`, header `include/st/c/st.h`)

* `st_tracker_memory_size()` tells you how much memory to provide.
* `st_tracker_create()` builds everything inside that block. There is no destroy call: free
  the block when you are done.
* Per frame:
  * `st_track_image()` does centroiding plus tracking, or lost-in-space when there is no
    attitude yet.
  * `st_track_centroids()` takes your own centroids.
  * `st_extract_centroids()` and `st_solve_lost_in_space()` give tetra3-style single solves.
* **Search variant**: `variant` in `st_config` (tracking) and in `st_lis_options` (single
  solves) picks ESA tetra3's search (`ST_VARIANT_ESA`, the default) or cedar-solve's
  (`ST_VARIANT_CEDAR`). Use the cedar variant with a database converted from cedar-solve; it
  also defaults the match threshold to cedar-solve's 1e-5.
* Only `st_*` symbols are exported. The library never allocates (symbol audit).

## 5. Python (`python/startracker`, ctypes, no build step)

```python
import startracker as st          # set STARTRACKER_LIB=/path/to/libstartracker.so if needed
t = st.Tracker(open("esa_default.stdb", "rb").read(), st.Camera.ideal(1280, 960, 11.4))
sol = t.track_image(numpy_uint16_image, t_ns=0)   # Solution(OK, lost_in_space, ra=..., ...)
cents = t.extract_centroids(img)
res = t.solve_lost_in_space(cents, 1280, 960, fov_estimate_deg=11.4)
# With a cedar-solve database, use cedar-solve's search:
t = st.Tracker(open("cedar_default.stdb", "rb").read(), st.Camera.ideal(1280, 960, 11.4), variant="cedar")
```

## 6. Tools (host only; these may allocate)

| tool | purpose |
|---|---|
| `st_dbtool` | `generate`, `pack` (from converted .npz), `info`, `verify` |
| `st_centroid` | centroids of PGM images (tetra3 parameters) |
| `st_solve_cases` | batch lost-in-space solves from a text case file (parity harness) |
| `st_render` | synthetic star fields (PSF, smear, noise, distortion, hot pixels) + truth |
| `st_sweep` | FOV / sensitivity / noise / rate sweeps scored against truth |
| `st_replay` | `pack` PGMs or `render` a rotating sequence into `.strp`; `run` it through the pipeline |
| `st_bench` | latency (median / p99 / max), memory, DB size, host info |

`.strp` replay format: a 64-byte header (`STRPLAY1`, version, width, height, pixel format,
frame count, stride), then per frame an i64 timestamp, a u64 sequence number and the pixels.
See `include/st/platform/replay_source.hpp`.

## 7. Cameras

* `V4L2Source` (Linux) captures GREY, Y16, Y10 or Y12 frames using MMAP buffers.
* Pass a calibrated `CameraIntrinsics` to the tracker: focal lengths, principal point,
  Brown-Conrady k1–k3 and p1–p2, and a mirror flag. The tracker refines every
  lost-in-space attitude with this model. tetra3 itself assumes an ideal pinhole with a
  coarse FOV.
