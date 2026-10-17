# Phase 7: C API, Python bindings, docs, real-sky and sidereal tests

Status: **implemented** (2026-10-27). Written as built.

## 1. C API (`include/st/c/st.h`, `src/capi/st_c.cpp`, shared library `libstartracker`)

* **One handle, caller memory.** `st_tracker` bundles the database view, centroider,
  lost-in-space solver, sky index, tracker and per-frame scratch.
  * `st_tracker_memory_size()` opens the database and sums the components'
    `workspace_bytes()`. `SkyIndex::workspace_bytes` and `Tracker::workspace_bytes` were
    added for this.
  * `st_tracker_create()` placement-constructs the handle at the start of the block and
    hands the rest to an `st::Arena`. There is no destroy function, because nothing outside
    the block is owned.
* **Errors**: `st_status` has the same numeric values as `st::Status`. A block that is too
  small returns `ST_ARENA_EXHAUSTED`; a misaligned one returns `ST_INVALID_ARGUMENT`.
* **Functions**: `st_track_image` (centroids, using ROIs while tracking, then the tracker),
  `st_track_centroids`, `st_extract_centroids`, `st_solve_lost_in_space` (tetra3-style
  options), `st_tracker_set_attitude` and `st_tracker_reset`.
* **Exports**: the library is built with hidden visibility. `--exclude-libs,ALL` (or
  `-exported_symbol,_st_*` on macOS) means only the 12 `st_*` functions are exported. The
  symbol audit runs on `libstartracker` too. It has no allocation, exception or typeinfo
  imports; the only C++ runtime import is `steady_clock::now`.
* **Tests**:
  * `capi.smoke_esa_alt40` (C11): the header compiles as C, and a real ESA image goes
    through extract, lost-in-space, then track (lost-in-space, then tracking with ROIs).
  * `CApi.*` (gtest): a rendered sequence is tracked with zero allocations; also checks a
    corrupt database, too-small memory, misalignment, reset and set_attitude.

## 2. Python (`python/startracker`)

* Pure ctypes, with no compiled extension and no NumPy requirement. Images can be NumPy
  arrays (uint8/uint16, shape gives the size) or any buffer plus width and height.
* `Tracker` owns a copy of the database and its own aligned memory block.
  `StarTrackerError` carries the status.
* Test: `python.bindings_esa_alt40` (extract, lost-in-space, track, reset, bad database).

## 3. Real-sky tests (`tests/realsky/realsky_check.py`, CI job `real-sky`)

* **Images**: 23 night-sky photographs from cedar-solve's `examples/data/medium_fov`
  (fixture `real_sky`, SHA-256 pinned, never committed). They are 8 ESA field images plus
  15 contributed photos taken with different lenses and sensors.
* **References**: the CI job installs astrometry.net and the Tycho-2 index files
  4107–4119, plate-solves every image (`solve-field`), and records RA/Dec at the centre,
  the corners and the edge midpoints (`wcs-xy2rd`, FITS +0.5 pixel offset applied).
* **Check**: each image is solved with the ESA and the cedar databases through the Python
  bindings, with no FOV hint. Our quaternion and fitted FOV, using an ideal pinhole as
  tetra3 does, predict the same points.
  * **Failure**: any solution we report is off by more than 3 px at the centre, or more than
    1.5 % of the diagonal at any point. Lens distortion is not modelled, so the corners
    carry its error. All 8 ESA images must also solve.
  * Unsolved images are only reported.
* **Not run here**: astrometry.net's hosts (nova, data, portal.nersc.gov) are blocked by
  this sandbox's network policy, so the references come only from CI.
* **Geometry validated locally**: using tetra3's own matched stars as stand-in references
  on the 8 ESA images, the centre error was ≤ 0.31 px and the worst star 6.5 px. The 6.5 px
  case sits in the corner of the most distorted image.
* **Local solve rate**: 15 of 23 images solve with at least one database (ESA 12, cedar
  12). The others are outside the databases' FOV range (10–30°) or are very sparse (tree,
  roof).

### Result (CI run 40457215637, 2026-10-27)

| | |
|---|---|
| astrometry.net solved | 23 / 23 |
| solved by us (either database) | 15 (ESA 12, cedar 12) |
| wrong solutions | 0 |
| centre error vs astrometry.net | 0.00–1.06 px (median ≈ 0.25 px) |
| worst point (corners / edges) | ≤ 2.9 px, except virgo at 19.4 px |

The virgo photo is a 1920×1080 wide shot taken through a strongly distorting lens. Our
centre agrees to 0.43 px; the 19.4 px corner error is within the 1.5 %-of-diagonal (33 px)
limit. The 8 unsolved images (crappy, discord_seoul3/4, orion2, orion_trees, pleiades, roof,
tree) are outside the databases' 10–30° FOV range or too sparse, and tetra3 behaves the same
way with these databases.

## 4. Sidereal rotation on rendered frames

`PipelineTest.SiderealRotationOnRenderedFrames` sets up an Earth-fixed camera: a constant
body rate of ω⊕·R(q0)ẑ. It renders 40 frames at 0.5 Hz, runs them through the pipeline with
ROI tracking, and checks:

* every attitude is within 40″ of truth;
* the estimated rate is within 5 % of 7.2921×10⁻⁵ rad/s;
* the estimated rate is within 5° of the celestial pole.

The existing `TrackerTest.SiderealRotation` covers the same case on projected centroids.

## 5. Hipparcos vs the shipped ESA database (informational)

* `tests/parity/hip_esa_db_check.py` generates a database from `hip_main.dat` with the ESA
  database's stored parameters and reports the overlap of pattern sets.
* It is informational because the shipped database came from an earlier tetra3 run whose
  magnitude-sort tie order we do not reproduce (quirk f). Generator correctness is gated by
  `generator_parity.py` on BSC5, where the sets are identical.
* CDS is blocked in this sandbox, so the check runs in CI only and skips when the download
  fails.
