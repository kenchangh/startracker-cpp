# Phase 3: lost-in-space solver

Status: **implemented** (2026-10-27). Written as built; the owner asked for all phases without
review stops.

## 1. API

`include/st/solver/lost_in_space.hpp`

```cpp
auto solver = LostInSpaceSolver::create(db, SolverLimits{}, arena);  // init: all memory
SolveResult r;
solver.value().solve(centroids /* (x, y), brightest first */, width, height, options, r);
```

* `SolveOptions` carries tetra3's parameters (FOV estimate and error, pattern checking stars,
  match radius and threshold, distortion `kNone` / `kFixed k` / `kRange (min, max)`), a
  deterministic `WorkBudget`, an optional `Clock` + timeout, an optional cancel flag, and the
  cedar-solve `SolverVariants`.
* `SolveResult` gives tetra3's RA/Dec/Roll/FOV/distortion/RMSE/Prob/matches plus P90E/MAXE
  (from cedar-solve), the matched (centroid, star) pairs in tetra3's order, the image pattern,
  the tetra3 rotation matrix, `q_cam_icrs` in our camera frame, and work counters.
* Statuses: `kOk`, `kNoMatch`, `kTooFewStars`, `kWorkBudgetExhausted`, `kTimeout`, `kCancelled`,
  `kInvalidArgument`, `kCapacityExceeded`.

## 2. Memory (allocated once from the arena)

For N = max centroids (database `verification_stars_per_fov` by default), M = trim × N,
C = `max_probe_chain + 1`, K = `max_keys_per_pattern` (cedar key order only), D = distortion
steps:

| Buffer | Size |
|---|---|
| centroids, undistorted | 2 × 16 N |
| pre-undistorted (kRange) | 16 D N + 48 D |
| key list (cedar order) | 12 K |
| probe chain entries | 40 C |
| nearby catalog stars | 48 M |
| matches + dedup tables | 16 N + 4 N + 4 M |
| match vectors, scratch | 48 N + 8 N² |

ESA default database (N = 30, C = 516): about 31 KB. cedar database (N = 150, C = 25): about
210 KB. `LostInSpaceSolver::workspace_bytes` returns the bound.

## 3. Algorithm and bounds

Faithful to ESA `solve_from_centroids`, step by step, including tetra3's float32 steps
(undistortion and catalog edge lengths are computed in float32 like numpy does on float32
arrays; everything else in double).

Three implementation choices that keep the results identical while bounding memory:

1. **Key enumeration without a buffer.** tetra3 builds `np.unique(np.sort(product))`. In the
   distortion-range mode that product can exceed any fixed buffer (a real ESA image needed 672
   distinct keys from a much larger product). `SortedKeys` enumerates exactly those tuples in
   the same lexicographic order with no storage: a depth-5 search where a sorted tuple is
   valid iff its values can be assigned to distinct key positions whose ranges contain them
   (greedy interval matching). Found by the parity harness.
2. **Streaming nearby-star search.** tetra3 collects every star in the diagonal FOV, keeps those
   that project inside the image, then keeps the first N (brightest). Visiting stars in index
   order and stopping at N gives the same list with an N-sized buffer.
3. **Bounded probing.** Probing stops at an empty slot (as tetra3) or after
   `max_probe_chain + 1` reads (never reached on a valid database).

Worst case per solve: P image patterns × K keys × C slot reads × (S stars + N × M match
distances), P = C(min(n, pattern_checking_stars), 4) = 70 by default. `WorkBudget` caps image
patterns, slot reads and candidates; the clock deadline is checked once per image pattern
(as tetra3) and is optional. No heap: enforced by the symbol audit (it caught a
`std::stable_sort`, which allocates, during development) and by a zero-allocation test.

## 4. Variants (cedar-solve, opt-in)

`SolverVariants::cedar()` sets all of: cluster-buster thinning of pattern candidates,
breadth-first (colex) pattern order over all candidates, keys probed by distance from the
measured key, 16-bit key-hash filter (when the database has it), `det < 0` rejection, nearby
trim 2N, `match_max_error` floor 0.002, catalog-first match de-duplication, and k fitted on raw
centroids (total k). Each is also individually selectable.

Documented divergences from cedar-solve:
* Breadth-first patterns are drawn from the trimmed centroid list, so cedar-solve's IndexError
  (more centroids than `verification_stars_per_fov`) cannot occur.
* Key tuples that are not non-decreasing are not probed (they cannot index a database pattern).
* Nearby stars use tetra3's cube + dot-product test rather than cedar's KD-tree; the two differ
  only for stars exactly on the boundary.

## 5. Results

Parity harness: `tests/parity/solve_parity.py`. tetra3's own `get_centroids_from_image` runs on
each image; both solvers then run on the same centroids.

Inputs: ESA's 2 sample images and the 99 fields of cedar-solve's synthetic corpus (Gaia DR3
positions), with 4 option sets for ESA (default; FOV hint 1°; distortion None; distortion range
(-0.2, 0.1)) and 3 for cedar (no range mode in cedar-solve).

Measured on the dev box (Intel Xeon @ 2.8 GHz, 4 vCPU container; GCC 13 Release; tetra3 under
CPython 3.12 + NumPy 1.26):

| Reference | Cases | Both solved | Failures | Worst boresight | Worst roll | Worst FOV (rel) |
|---|---|---|---|---|---|---|
| ESA tetra3 f9fa2eb | 404 | 360 | 0 | 0.0016" | 0.0008" | 3.8e-8 |
| cedar-solve 607e3f8 (cedar variants, cedar database) | 303 | 303 | 0 | 0.0002" | 0.0002" | 3.4e-8 |

In every solved case the matched catalog stars are identical and in the same order.

Solve times (same machine, all cases, no budget or timeout):

| | median | p99 | max |
|---|---|---|---|
| ESA tetra3 (404 cases) | 15.8 ms | 18.4 s | 45.6 s |
| ours, ESA settings | 0.48 ms | 0.80 s | 2.46 s |
| cedar-solve (303 cases) | 2.95 ms | 15.6 ms | 51.0 ms |
| ours, cedar variants | 0.30 ms | 0.81 ms | 1.21 ms |

The long tail comes from fields tetra3 cannot solve in distortion-range mode, where every
pattern and key is tried; this is what `WorkBudget` exists to cut off. These are CPython vs
C++ on x86-64, not ARM numbers; phase 6 benchmarks on the target class of hardware.

Unit tests (synthetic sky, no fixtures): random attitudes with zero allocations, all variants,
FOV hint, distortion-range recovery (k = -0.08 recovered within 0.01), no false match on random
centroids, and every bounded stop (budget, slot reads, timeout, cancel, capacity).

## 6. Accuracy note (tetra3 behaviour kept for parity)

tetra3 computes the reported rotation with the *coarse* FOV (from the matched pattern), and in
range mode with the raw (still distorted) centroids; the refined FOV and k are not fed back
into the rotation. On exact synthetic centroids this leaves about 1-15" of boresight error at
16-20° FOV, and about 100" with k = -0.08. This is ESA's accuracy, reproduced on purpose; the
tracking mode (phase 5) refines attitude with the calibrated camera model.

## 7. New quirk found

(j) ESA's largest-edge FOV filter divides a float16 array by a float64 scalar, which numpy 1.x
evaluates in float16 (about 1e-3 relative error). It only applies to databases with
`pattern_largest_edge` plus a FOV estimate and error (not the ESA default database). We compute
it in float32, as cedar-solve does.
