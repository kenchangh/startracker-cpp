# Design overview and reference-source findings

Status: **draft, awaiting review**. This note records what was learned from the reference
sources and the project-wide decisions every phase depends on.

**Primary algorithm and parity target: ESA tetra3 @ f9fa2eb.** cedar-solve's changes are
offered as optional, selectable variants; ESA behaviour is always the default.
Line numbers `E:nnn` refer to `tetra3/tetra3.py` in esa/tetra3 @ f9fa2eb, `C:nnn` to the same
file in smroid/cedar-solve @ 607e3f8.

## 1. Reference sources

| Project | Commit | License | How we use it |
|---|---|---|---|
| esa/tetra3 | f9fa2eb (2023-12-09) | Apache-2.0 | **Primary** algorithm reference and parity target |
| smroid/cedar-solve | 607e3f8 (2026-08-05) | Apache-2.0 (code) | Source of optional variants (§3) |
| oakamil/cedar-solve | 74ad65c | Apache-2.0 | Stale fork of smroid/cedar-solve; not used |
| brownj4/Tetra | (cited in headers) | MIT | Original algorithm; attribution only |
| smroid/cedar-detect | 2f403b8 | **FSL-1.1-MIT** | License read only. **Not ported, code not read** (§5) |
| UWCubeSat/lost | 4b1bdc2 | Implied MIT (Makefile header, nix) | Survey only; no code reuse |

## 2. The ESA tetra3 algorithm (what we reproduce by default)

### 2.1 Database generation (`generate_database`)

Defaults (E:570-574): `pattern_stars_per_fov=10`, `verification_stars_per_fov=30`,
`star_max_magnitude=7`, `pattern_max_error=0.005`, `simplify_pattern=False`,
`presort_patterns=True`, `save_largest_edge=False`.

1. Load catalog, propagate proper motion to the chosen epoch, sort by magnitude, cut at
   `star_max_magnitude`.
2. FOV scales: one scale if `max_fov/min_fov` is small, else log-spaced scales (step 1.5).
   Edge case (d) in §4.
3. Thin to **pattern** density (`pattern_stars_per_fov`) and, separately, to
   **verification** density (`verification_stars_per_fov`). The stored star table is the
   verification-density set.
4. Patterns: for every pattern star, all 3-neighbour combinations within the FOV
   (within fov/2 if `simplify_pattern`), forming 4-star patterns. Deduplicated through a
   Python `set`, then `list(set)`.
5. Pattern key: 6 edge angles, sort, divide the 5 smallest by the largest, quantize to
   `bins = round(1/4/pattern_max_error)` = **50** bins. `key_hash = Σ key[i]·bins^i` in uint64.
6. Hash table: `catalog_length = 2·N_patterns` (**not prime**, E:1039). Index
   `(key_hash · 2654435761) % size` (E:110), quadratic probing `(h + c²) % size`
   (E:137-153). A slot is empty when all 4 star indices are 0.
7. Presort: pattern stars ordered by distance from the pattern centroid (`np.argsort`,
   unstable; see (f)).
8. Stored arrays: `pattern_catalog` (uint16/uint32 star indices), `star_table` float32
   `[ra, dec, x, y, z, mag]`, `star_catalog_IDs`, `pattern_largest_edge` (float16 mrad, only
   if `save_largest_edge`). No key-hash array; `num_patterns` is not stored and is taken as
   `slots/2`.

### 2.2 Solving (`solve_from_image` / `solve_from_centroids`)

Defaults: `pattern_checking_stars=8`, `match_radius=0.01`, `match_threshold=1e-3`, which is
divided by `slots//2` (E:1120, 1231).

1. Trim centroids to `verification_stars_per_fov` (E:1352-1360). Because of this ESA does
   **not** have cedar-solve's IndexError (§3.2).
2. Distortion handling: scalar `k` → centroids undistorted in place (E:1367); tuple range →
   pre-undistorted copies every 0.1 across the range (E:1369-1378).
3. Image patterns: `itertools.combinations(range(min(n, 8)), 4)` — lexicographic, at most 70.
4. Per image pattern: edge ratios → key; enumerate keys in the ±`pattern_max_error` box,
   hash, quadratic probe until an empty slot; compare exact ratios of each catalog hit.
5. Per candidate: coarse FOV from the largest edge, order image stars to match the presorted
   catalog stars, rotation by SVD `U·Vh` **without det<0 rejection**.
6. Nearby catalog stars: brute force over the star table inside the FOV, trimmed to
   `len(image_centroids)`.
7. Matching within `width·match_radius` px. Deduplicate by image column first, then catalog
   column; the **first index wins**, not the nearest.
8. Mismatch probability `binom.cdf(N − (M − 2), N, 1 − n_nearby·match_radius²)`; accept
   when below `match_threshold / (slots//2)`. First accepted candidate wins.
9. Refinement: SVD rotation on all matches. Without distortion, FOV refined by the mean
   angle ratio. With distortion, least squares `r = f·tanθ + k·r³`, then `f /= (1 − k)`.
   Report RMSE, P90E, MAXE, matches, probability, T_solve.
10. Timeout: wall clock, checked once per image pattern.

`get_centroids_from_image` is code-identical in ESA and cedar-solve (whitespace only), so
centroider parity does not depend on which one is chosen.

### 2.3 Measured database sizes (headers parsed from the shipped `.npz` files)

| Database | Patterns | Slots | Star idx | Stars | Uncompressed |
|---|---|---|---|---|---|
| esa/tetra3 default (quadratic probe, no key hashes / largest edge) | 6,184,546 (= slots/2) | 12,369,092 | uint16 | 8,818 | 99.2 MB |
| cedar-solve default (hip_main, 10-30°, mag ≤ 8.03, linear probe, epoch 2025) | 1,010,981 | 3,032,963 | uint16 | 42,212 | 37.6 MB |

## 3. cedar-solve variants (optional; ESA is the default)

Each row is an independent option in the generator or solver config. A database records which
generator options built it, and the solver refuses option combinations the database cannot
support (e.g. key-hash filter on a database without key hashes) with `kUnsupported`.

### 3.1 Variant table

| Option | ESA default | cedar-solve behaviour | Effect |
|---|---|---|---|
| `pattern_generation` | `neighbour` (all 3-neighbour combos per pattern star) | `lattice`: Fibonacci lattice of field centres, `patterns_per_lattice_field` per field, breadth-first combos | Fewer patterns (~6× smaller DB at default settings), more uniform sky coverage |
| `db_thinning` | density thinning | `cluster_buster`: greedy, min sep `0.6·fov/√verification_stars` | Avoids close doubles that make unstable patterns |
| `centroid_thinning` | none | `cluster_buster` on image centroids, same rule in pixels | Same, on the image side |
| `image_pattern_order` | `lexicographic` over the 8 brightest | `breadth_first` (colex) over the thinned, trimmed set (see §3.2) | More patterns tried when the brightest stars are bad |
| `key_probe_order` | box order | sorted by squared distance from the measured key | Finds the right pattern sooner |
| `table_layout` | quadratic probe, size 2N, `_MAGIC_RAND` | linear probe, `next_prime(3N)`, 16-bit key-hash filter | Fewer full candidate checks; needs the key-hash array |
| `largest_edge_filter` | off | reject candidates whose largest edge disagrees with `fov_estimate ± fov_max_error` | Needs `pattern_largest_edge` in the DB |
| `reject_reflection` | off | reject rotations with `det < 0` | Drops mirror-image false matches |
| `nearby_trim` | `N` | `2N` | More catalog stars considered when matching |
| `match_max_error_floor` | off | lower bound on the ratio tolerance | Robustness at small `pattern_max_error` |
| `match_dedup_order` | image column first | catalog column first | Changes which pairs survive in crowded fields |
| `status_detail` | ESA outcome (match / none) | MATCH_FOUND / NO_MATCH / TIMEOUT / CANCELLED / TOO_FEW | We always return these as `Status` codes; this only affects what the parity layer compares |
| `distortion_k_report` | residual k (quirk (a)) | total k, fitted on raw centroids | Reported k is meaningful on its own |

### 3.2 cedar-solve's own bug, if the breadth-first variant is enabled

cedar-solve computes pattern indices over all centroids (C:1779) and only then truncates the
list to `verification_stars_per_fov` (C:1785, 1838). Breadth-first iteration reaches indices
beyond the truncated length and raises `IndexError`. Our `breadth_first` variant iterates only
over the trimmed set; this is a documented divergence from cedar-solve. It does not affect
the ESA default.

## 4. ESA quirks: decisions needed

Found by reading; confirmed by running in phase 3. Proposal given for each.

| # | Quirk | Proposal |
|---|---|---|
| (a) | Scalar distortion: centroids undistorted in place (E:1367) and k re-fit on them (E:1632), so the reported k is the **residual**, not the total | Replicate by default; `distortion_k_report=total` variant |
| (b) | `np.math.factorial` (E:1395) breaks on NumPy ≥ 2 | Parity harness pins `numpy<2` in its own venv; no change to our code |
| (c) | Non-prime table (2N) with quadratic probing: insertion not guaranteed to terminate in theory | Converter accepts ESA tables as given. Our generator in ESA mode uses 2N for parity, with a probe limit; failure returns `kCapacityExceeded` and the generator retries with a larger table |
| (d) | Single-scale edge case `1 < max/min < √1.5`: pattern separation uses `max_fov` | Replicate and document |
| (e) | `range_ra`/`range_dec` stored in radians though the parameter is in degrees | Converter reads radians; our blob stores radians and the docs say so |
| (f) | `np.argsort` in the presort is unstable for ties | Our generator uses a stable tie-break (lower star index first). Parity uses the converter, so no effect there |
| (g) | `list(set)` order decides slot layout | Parity runs use converted ESA DBs (identical tables). Generator tests compare **pattern sets**, not layouts |
| (h) | A key digit can equal `bins` when a ratio is exactly 1.0 (aliases into the next digit) | Replicate; consistent between build and solve, only adds collisions |
| (i) | Wall-clock timeout makes results machine-dependent | Primary bound: deterministic work budget (image patterns, table probes, candidates verified). Optional deadline through an injectable `Clock`. Parity tests use the work budget |

Other behaviour we replicate exactly because it changes match counts and so the probability
test: first-index (not nearest) match assignment, image-then-catalog dedup, nearby trim to N.

## 5. Parity definition

* Harness: Python venv with esa/tetra3 @ f9fa2eb, **`numpy<2`**, scipy, Pillow; pinned in
  `tools/parity/requirements.txt`. Runs in CI.
* Inputs: ESA's sample images (fetched, SHA-256 checked, never committed) and ESA's default
  database converted to our blob.
* Phase 3 feeds both sides tetra3's own centroids; phase 4 adds our centroider end to end.
* Pass criteria per image: same solve status, same set of matched catalog stars, boresight
  difference < 1 arcsec, roll difference < 5 arcsec, FOV relative difference < 1e-5.
* Results match within tolerance, not bit for bit (NumPy/SciPy vs. our float paths).
* Variants are tested separately against cedar-solve where it is a faithful reference (and
  against synthetic truth where cedar-solve's IndexError would fire).

## 6. Licensing plan

* Project license Apache-2.0. `NOTICE` credits ESA (tetra3), Steven Rosenthal (Cedar Solve)
  and brownj4 (Tetra, MIT notice reproduced). Files derived from tetra3/cedar-solve carry a
  header saying so.
* **The name must not contain "Cedar"** (trademark). Factual "variants based on Cedar Solve"
  in docs is allowed.
* **cedar-detect** (FSL-1.1-MIT): not ported and not read while implementing the centroider.
  The centroider uses general published techniques (local background, sigma thresholding,
  hot-pixel and elongation tests). Alternative: ask the author (smr@dt3.org) for an Apache
  grant.
* **LOST:** survey only. Weaknesses we avoid: per-frame `std::vector` / `unordered_set` /
  `unordered_multimap`, no work bound, global mean+5σ threshold, recursive flood fill,
  `exit(1)` in library code, a database with no version/checksum/endianness, no tracking,
  single images only.
* **Test data:** ESA sample images are CC BY-SA 3.0 IGO; other cedar sample images are
  unlicensed; Hipparcos comes from CDS. None of it is committed; `tools/fetch_fixtures.py`
  downloads it and checks pinned SHA-256 sums.

## 7. Project-wide conventions (proposed)

* Namespace `st`; library target `st_core`. Final project name is your call.
* Pixel coordinates follow tetra3: origin at the top-left corner of the top-left pixel (pixel
  centres at +0.5), image centre at (w/2, h/2). Public API uses (x, y); the parity layer
  converts to tetra3's (y, x).
* Precision (justified per function in each phase):
  * float32: pixel coordinates, centroiding, edge ratios.
  * Catalog unit vectors **stored** as float32, as tetra3 stores them (~6e-8 rad, 0.012"
    quantization, far below expected accuracy; halves the star table). Promoted to double
    for all attitude math. **Your brief said double for catalog vectors — please confirm.**
  * double: rotation/quaternion, Wahba/SVD, FOV/distortion refinement, binomial
    probability, rate estimation, time.
* Determinism: same input + same config → same output bits on x86-64 and ARM64
  (`-ffp-contract=off`, no fast-math, no unordered containers, fixed iteration orders).
* Minimum compilers: GCC 11, Clang 14.
