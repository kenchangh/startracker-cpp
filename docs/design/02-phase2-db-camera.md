# Phase 2: camera model, database format, generator and converter

Status: **implemented** (2026-10-27). The owner asked for all phases to be finished without
review stops, so this note records the design as built.

## 1. Components

| Component | Where | Heap | Notes |
|---|---|---|---|
| `Vec3`, `Mat3`, `Quat` | `include/st/math/vec.hpp` | no | double; Hamilton, scalar-first quaternions |
| `svd3`, `orthogonal_polar`, `proper_rotation`, `binomial_cdf` | `st/math/linalg.hpp` | no | bounded Jacobi sweeps |
| `CameraModel` | `st/camera/camera.hpp` | no | pinhole + Brown-Conrady (k1,k2,k3,p1,p2) + parity |
| blob format, CRC-32C, half floats | `st/db/format.hpp` | no | flat, little-endian, versioned |
| `Database` view | `st/db/database.hpp` | no | zero-copy over mmap or a copy |
| `MappedFile` | `st/platform/mapped_file.hpp` (st_platform) | no | POSIX mmap/mlock |
| `st_dbtool`, generator, writer, `.npy` reader | `tools/cpp` (st_tools) | yes | host only |
| `.npz` converter | `tools/convert_tetra3_npz.py` | n/a | numpy, then `st_dbtool pack` |
| fixture fetcher | `tools/fetch_fixtures.py` | n/a | pinned SHA-256, nothing committed |

## 2. Precision choices

* **Catalog vectors: float32** in the blob (tetra3 stores float32; 0.012" quantization; halves
  the star table and its cache footprint). Promoted to double for all attitude math.
* **Attitude, SVD, probability, camera: double.** float32 has 7 significant digits; a 4000 px
  sensor needs 1e-4 px over chains of trigonometry, and the binomial tail reaches 1e-60.
* `svd3` is one-sided (Hestenes) Jacobi on A itself, not an eigen-decomposition of AᵀA, so
  small singular values keep full relative accuracy (found by the rank-deficiency test: the
  AᵀA approach gave 9e-8 instead of 0).

## 3. Blob format (version 1)

256-byte header at fixed offsets (documented in `format.hpp`), then sections aligned to 64 B:
star table (f32 × 6: ra, dec, x, y, z, mag), catalog IDs (u32), pattern table (u16 or u32 × 4,
all-zero row = empty), optional key hashes (u16), optional largest edges (IEEE half, mrad).

Integrity: header CRC-32C (with its own field zeroed) and payload CRC-32C. Software slice-by-8:
the 99 MB ESA database verifies at load in about 0.1 s on the dev box.

`Database::open` checks, in order: host endianness, 8-byte alignment, magic, version, size,
checksums, enum values, section bounds/alignment/sizes against the counts, every star index
against `num_stars` (O(slots)), finite star vectors, and optionally the probe bound.

### Probe bound (new, not in tetra3)

`max_probe_chain` = the longest run of occupied slots any lookup can traverse before it hits an
empty slot, computed by the writer over every possible start index. It turns tetra3's
"probe until empty" loop into a loop with a stated worst case. Measured on the ESA default
database: **515** (average chain is ~1-2; the long tail comes from quadratic probing on a
non-prime table, quirk (c)). `validate_probe_bound` re-measures it at load if desired.

## 4. Worst-case bounds

| Function | Worst case |
|---|---|
| `Database::open` (defaults) | O(file size) CRC + O(num_slots) index check |
| `Database::pattern`, `star`, `start_slot`, `probe_slot` | O(1) |
| one key lookup | ≤ `max_probe_chain + 1` slot reads |
| `CameraModel::pixel_to_ray` | ≤ 20 Newton iterations |
| `svd3` | ≤ 32 sweeps × 3 rotations |
| `binomial_cdf(k, n, p)` | O(k) |
| `crc32c` | O(bytes) |

Memory: `Database` is 6 pointers + the 232-byte header; the blob itself is caller memory.

## 5. Generator

C++ port of ESA `generate_database`, reading `hip_main.dat` or `bsc5` exactly as tetra3 does
(field indices, blank checks, proper-motion rules, float32 trigonometry for vectors, chord-length
separations, `ceil(log_1.5)` scale schedule, verification thinning starting at index 1, 2N
slots, quadratic probing, presort by radius from the float32 centroid, float16 largest edge).

Divergences, all deliberate and tested:

1. **Stable magnitude sort.** numpy's `argsort` is unstable for the many equal magnitudes
   (catalog magnitudes have 0.01 resolution), so which of two tied neighbours survives thinning
   depends on numpy internals. We use a stable sort.
2. **Patterns deduplicated as unordered 4-sets.** tetra3 deduplicates ordered tuples, whose
   order comes from KD-tree traversal; the same 4 stars found at two scales can take two slots.
   On the BSC5 test below, tetra3 stored 7,241 such duplicate rows.
3. **Insertion order** is sorted, not Python `set` order, so the slot layout differs. Parity
   runs therefore use converted ESA databases (identical layout), and the generator is tested
   on pattern sets (quirk (g)).

Measured (BSC5, FOV 20-30°, mag ≤ 6, no proper motion; dev box, x86-64 container):

| | ESA tetra3 | ours |
|---|---|---|
| time | 155 s | 5.2 s |
| verification stars | 2,523 | 2,525 |
| unique pattern sets | 1,184,226 | 1,184,431 |
| common sets | 1,169,815 (98.8%) | |

**Stable-sort check:** with the single line in tetra3 changed to
`np.argsort(..., kind='stable')`, tetra3 produces **exactly** our 1,184,431 pattern sets
(0 differences) and the same 2,525 verification stars. The 1.2% gap is therefore entirely
the tie-order quirk (f). CI runs this comparison (`tests/parity/generator_parity.py`).

## 6. Converter

`tools/convert_tetra3_npz.py in.npz out.stdb` copies every array unchanged and maps
`props_packed` to the header (`threshold_patterns = slots // 2` as tetra3 divides by it;
`range_ra/dec` kept in radians, quirk (e)). Verified on the ESA default database: 8,818 stars,
12,369,092 slots, and for every sampled pattern, recomputing its key with tetra3's formula finds
its slot within the probe bound (100% of ~6,200 samples).

## 7. Tests

Unit (no fixtures): quaternion/matrix round trips, SVD reconstruction and rank deficiency,
numpy-like reflection in the polar factor, binomial CDF against scipy values, camera round trip
with distortion and mirror parity, CRC-32C test vector, half floats, header round trip, blob
build/open with zero allocations, corruption cases (checksums, truncation, misalignment,
version, out-of-range index with a valid checksum, understated probe bound), generator
invariants on a synthetic catalog (FOV limit, key reaches slot, presort, largest edge).

Fixture tests (skipped without `tools/fetch_fixtures.py`): ESA database header and key lookups.

Fuzzing: `st_fuzz_db_open` (libFuzzer + ASan + UBSan): 17.8 M executions in 60 s, no findings.

Network note: CDS (Hipparcos) is blocked in the dev sandbox, so `hip_main` is an optional
fixture; CI downloads it.
