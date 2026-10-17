# Design overview and reference-source findings

Status: **draft, awaiting review**. This note records what was learned from the reference
sources (read 2026-10-26) and the project-wide decisions that every phase depends on.

## 1. Reference sources read

| Project | Commit read | License | How we use it |
|---|---|---|---|
| esa/tetra3 | f9fa2eb (2023-12-09) | Apache-2.0 | Algorithm reference (legacy format) |
| smroid/cedar-solve | 607e3f8 (2026-08-05) | Apache-2.0 (code) | **Primary** algorithm reference |
| oakamil/cedar-solve | 74ad65c (2025-11-13) | Apache-2.0 | Fork of smroid/cedar-solve; 2 net-zero commits ahead, 8 behind. No algorithm changes of its own. |
| brownj4/Tetra | (cited in headers) | MIT | Original algorithm; attribution only |
| smroid/cedar-detect | 2f403b8 (2026-08-28) | **FSL-1.1-MIT** (MIT after 5 years per version) | **Cannot be ported.** Ideas only, see §4 |
| UWCubeSat/lost | 4b1bdc2 (2026-10-15) | No LICENSE file; MIT text in the `Makefile` header and `nix/package.nix` (implied MIT) | Survey only; no code reuse |

## 2. The cedar-solve algorithm as implemented (from `tetra3/tetra3.py`)

### 2.1 Database generation (`generate_database`, l.903-1445)
1. Load catalog (hip_main: positions at J1991.25, linear RA/Dec proper-motion propagation to
   `epoch_proper_motion`, skipped where |dec| > ~87 deg (`cos_delta <= 0.05`); entries with
   missing PM are dropped when propagating). Sort by magnitude.
2. Magnitude cutoff: if not given, from a 100-bin histogram so that
   `0.7 * ceil(4*pi/min_fov^2) * verification_stars_per_fov` stars are kept.
3. FOV scales: single scale if `max_fov/min_fov < sqrt(multiscale_step)`, else
   `ceil(log_{1.5}(ratio))+1` log-spaced scales.
4. Per scale: "cluster buster" thinning (greedy in brightness order, min separation
   `0.6*fov/sqrt(verification_stars_per_fov)`), then a Fibonacci lattice of
   `ceil(4*pi/fov^2) * lattice_field_oversampling` field centres (`2n+1` points). In each field
   (radius fov/2), take the first `patterns_per_lattice_field` 4-star combinations in
   **breadth-first (colex) order** of brightness, dedup into a global set.
5. Pattern key: 6 edge angles `2*asin(d/2)`, sort, divide the 5 smallest by the largest,
   `int(ratio*bins)` with `bins = round(0.25/pattern_max_error)` (250 by default).
   Key hash = sum(key[i] * bins^i) in uint64. Table index = `hash % size` (linear probe)
   or `hash*2654435761 % size` (quadratic probe, legacy).
6. Pattern stars stored ordered by distance from the pattern's mean vector ("presorted").
   Per slot also: low 16 bits of key hash (uint16) and largest edge in mrad (float16).
7. Table size `next_prime(3n)` linear probe, `next_prime(2n)` quadratic.

### 2.2 Solving (`solve_from_centroids`, l.1584-2247)
1. Cluster-buster on the image centroids (same separation rule, in pixels).
2. Iterate 4-combinations of the thinned centroids in breadth-first order; for each:
   edge ratios, tolerance box ±`match_max_error`, enumerate all keys in the box sorted by
   squared distance to the measured key, probe the hash table for each, filter by 16-bit
   hash, then FOV (if `fov_estimate` and `fov_max_error` given), then exact ratio box.
3. For each surviving catalog pattern: coarse FOV from largest edge, sort the image pattern
   by radius from its centroid, Kabsch/SVD rotation, reject `det < 0`.
4. Project catalog stars within the diagonal FOV (brightness order, kept to `2*N`),
   match within `match_radius*width` px, compute
   `P = binom.cdf(N - (M - 2), N, 1 - N_cat*match_radius^2)`; accept when
   `P < match_threshold / num_patterns`.
5. Refine: SVD rotation on all matches; refine FOV (mean angle ratio) or, if distortion is
   enabled, least squares for focal length + one radial term `k`; compute RMSE/P90E/MAXE.
6. Termination: first accepted candidate wins. Wall-clock timeout checked once per image
   pattern. Statuses: MATCH_FOUND, NO_MATCH, TIMEOUT, CANCELLED, TOO_FEW.

### 2.3 Measured database sizes (headers parsed from the shipped `.npz` files)

| Database | Patterns | Slots | Star idx | Stars | Uncompressed |
|---|---|---|---|---|---|
| cedar-solve default (hip_main, 10-30 deg, mag <= 8.03, linear probe, epoch 2025) | 1,010,981 | 3,032,963 | uint16 | 42,212 | 37.6 MB |
| esa/tetra3 default (legacy, quadratic probe, no key hashes / largest edge) | n/a | 12,369,092 | uint16 | 8,818 | 99.2 MB |

### 2.4 How esa/tetra3 differs (what "parity with tetra3" would mean)

ESA tetra3 is a different algorithm generation, not just an older copy:
* DB: patterns from every 3-neighbour combination around each pattern star (no lattice
  fields), default 50 bins, quadratic probe with a **non-prime** table of 2N, star table
  thinned to verification density (30/FOV), no key hashes, largest edge optional.
* Solve: only the 8 brightest centroids, lexicographic `itertools.combinations` (70 patterns),
  no cluster thinning, no `det<0` rejection, nearby stars trimmed to N (not 2N), match
  dedup in the opposite column order, and a scalar distortion estimate that returns the
  *residual* k rather than the total.
* `np.math.factorial` (E:1395) breaks on NumPy >= 2.0, so ESA tetra3 needs a pinned NumPy 1.x
  to run in the parity harness.
* cedar-solve loads ESA databases through legacy paths (quadratic probe + `_MAGIC_RAND`, num_patterns =
  slots/2, no key-hash filter). ESA tetra3 **silently fails** on linear-probe cedar databases.
* `get_centroids_from_image` is code-identical in both (only whitespace differs).

## 3. Behaviour that looks buggy or ambiguous (needs your decision before phase 3)

Found by reading; **not yet confirmed by running** (that happens in phase 3).

* **Q1. IndexError with many centroids (cedar-solve only; confirmed by two independent reads).** `pattern_centroids_inds` is computed over
  all centroids (l.1779), then `image_centroids` is truncated to
  `verification_stars_per_fov` (l.1785). Breadth-first iteration eventually reaches pattern
  indices >= the truncated length, and `image_centroids_vectors[image_pattern_indices]` would
  raise. Options: (a) replicate the effective behaviour (stop at the first such
  combination, as Python does via the exception) or (b) restrict pattern candidates to the
  truncated set. Proposal: (b), and record it as a documented divergence.
* **Q2. Match assignment is not nearest-neighbour.** `_find_centroid_matches` keeps, per
  catalog star, the *lowest-index (brightest) image centroid* within radius, then per image
  centroid the lowest-index catalog star. Proposal: replicate exactly (it affects match
  counts and so the probability test), and offer a nearest-neighbour mode as an option
  that is off by default.
* **Q3. Wall-clock timeout makes results machine-dependent.** Proposal: primary bound is a
  deterministic work budget (image patterns and catalog candidates evaluated), plus an
  optional deadline through an injectable clock. Parity tests use the work budget.
* **Q4. Database pattern insertion order comes from Python `set` iteration**, which decides
  slot placement under probing. Our generator will produce the same *set* of patterns but not
  the same slot layout; parity runs therefore use the converter (identical tables), and
  generator tests compare the pattern sets.
* **Q5. `save_database` reads `_db_props` keys that only `generate_database` sets**
  (`anchor_stars_per_fov`, ...), so re-saving a loaded database fails in Python. No impact on
  us beyond the converter reading legacy keys.
* **Q6. Rotation from SVD has no reflection correction**; candidates with `det < 0` are
  dropped instead. We replicate it.
* **Q7. A key digit can equal `bins`** when a ratio is exactly 1.0, which aliases into the next
  base digit. It is consistent between build and solve, so it only adds collisions. We replicate it.
* **Q8. cedar-solve probes unsorted key tuples** from the tolerance box (wasted lookups, but they
  can never match). They affect only work counts, not results. We skip them, but count them so
  the work budget stays comparable.
* **Q9. ESA's quadratic probe on a non-prime table** is not guaranteed to terminate an insert in
  theory. The converter accepts such tables as given. Our generator always uses primes.

Results will match tetra3 to within tolerance, not bit for bit (NumPy/SciPy vs. our float
paths). Parity is defined as: same solve status, same set of matched catalog stars, and
attitude difference below a threshold (proposed 1 arcsec for the boresight, 5 arcsec for
roll) on the fixture set.

## 4. Licensing plan

* Project license Apache-2.0. `NOTICE` credits ESA (tetra3), Steven Rosenthal (Cedar Solve)
  and brownj4 (Tetra, MIT notice reproduced). Files derived from tetra3/cedar-solve code
  carry a header saying so.
* **Name: must not contain "Cedar"** (registered as a trademark in `TRADEMARKS.md`). Factual
  "based on Cedar Solve" in docs is allowed.
* **cedar-detect:** FSL-1.1-MIT forbids "Competing Use" and only becomes MIT 5 years after
  each release. Translating its code into our library would make our centroider a derivative
  under FSL, which is incompatible with Apache-2.0. Plan: an independent centroider designed
  from published, general techniques (local background, sigma thresholding, hot-pixel and
  elongation tests are standard); our team does not read the Rust source while
  implementing. Alternative: ask the author for an Apache grant (the license invites
  contact).
* **LOST:** used as a survey only. Problems to avoid: heap allocation every frame
  (`std::vector`, `unordered_set`, `unordered_multimap` in star-id loops), no work or time
  bound, global mean+5 sigma threshold with no local background or hot-pixel handling,
  recursive flood fill, `exit(1)` inside library code, a database with no version, checksum
  or endianness handling, no tracking mode, single images only.
* **Test data:** the ESA sample images are CC BY-SA 3.0 IGO, and the other cedar-solve sample
  images are released "as Tetra3 test images" with no stated license. Hipparcos and BSC5 are
  CDS/NASA data. None of this is committed; a `tools/fetch_fixtures.py` downloads it and
  checks pinned SHA-256 sums.

## 5. Project-wide conventions (proposed)

* Namespace `st`; library target `st_core`. The final name is up to you.
* Pixel coordinates follow tetra3: origin at the top-left corner of the top-left pixel (pixel
  centres at +0.5), image centre at (w/2, h/2). The public API uses (x, y); the parity layer
  converts to tetra3's (y, x).
* Precision (to be justified per function in each phase):
  * float32: pixel coordinates, centroiding, edge ratios.
  * Catalog unit vectors **stored** as float32, which is what tetra3 stores. Quantization is
    ~6e-8 rad (0.012 arcsec), far below expected accuracy, and it halves the star table.
    Promoted to double for all attitude math.
  * double: rotation/quaternion, Wahba/SVD, FOV/distortion refinement, binomial probability,
    rate estimation, time.
* Determinism: same input + same config -> same output bits on x86-64 and ARM64
  (`-ffp-contract=off`, no fast-math, no unordered containers, fixed iteration orders).
