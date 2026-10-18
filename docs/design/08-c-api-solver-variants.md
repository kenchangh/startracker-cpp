# 08: Solver variants in the C API and Python bindings

Status: **implemented** (2026-10-28). Approved with `st_config` included.

## Why

The C++ solver can run either ESA tetra3's search or cedar-solve's (`SolverVariants::esa()` /
`SolverVariants::cedar()`, phase 3, 303/303 parity cases). The C API and the Python bindings
cannot choose: `st_solve_lost_in_space` always runs the ESA search. So a C or Python user with
cedar's database does not get cedar-solve's behaviour, and the README's speed comparison
against cedar-solve cannot show matching answers. Measured on the 23 real-sky photos through
the bindings on `cedar_default.stdb`, 4 answers differ from cedar-solve (orion_trees,
pleiades, test_5mp_g100_e50ms, virgo). The ESA pair matches on all 23.

## Change

1. **C (`include/st/c/st.h`)**: one field is appended to `st_lis_options` and one to
   `st_config`.

   ```c
   typedef enum st_solver_variant { ST_VARIANT_ESA = 0, ST_VARIANT_CEDAR = 1 } st_solver_variant;

   typedef struct st_lis_options {
     ...existing fields unchanged...
     uint32_t variant;  /* st_solver_variant; 0 = ESA tetra3 (unchanged behaviour) */
   } st_lis_options;

   typedef struct st_config {
     ...existing fields unchanged...
     uint32_t variant;  /* the tracker's lost-in-space step; 0 = ESA */
   } st_config;
   ```

   * It is a preset rather than the nine individual switches. Those switches exist for parity
     testing, and a preset keeps the C surface small. A finer API can be added later if
     someone needs it.
   * `ST_VARIANT_CEDAR` selects `SolverVariants::cedar()`. When `match_threshold` is 0 it
     also defaults the threshold to 1e-5, which is cedar-solve's default; ESA's is 1e-3. An
     explicit threshold still wins.
   * Unknown values return `ST_INVALID_ARGUMENT`, from `st_solve_lost_in_space` and from
     `st_tracker_create`.
   * The variant changes no capacities: the default `SolverLimits` already allow cedar's
     `nearby_trim_factor` of 2, so `st_tracker_memory_size` is the same for both.
2. **Python**:
   * `Tracker(..., variant="esa" | "cedar")` sets `st_config.variant`.
   * `solve_lost_in_space(..., variant=None)` uses the tracker's variant unless one is given.
   * An unknown name raises `ValueError`.
3. **Tests**:
   * The C smoke test keeps passing with zero-initialised options, so ESA stays the default.
   * A new `CApi.CedarVariantMatchesCxx` (gtest, synthetic database, no fixtures) checks:
     * a solve through the C API and through `LostInSpaceSolver` agrees exactly, for both
       variants (`SolverVariants::cedar()` with a 1e-5 threshold on the C++ side);
     * unknown variants are rejected by both entry points;
     * `st_track_image` on a tracker created with `ST_VARIANT_CEDAR` solves the first frame.
   * Python: `python.bindings_esa_alt40` also solves with `variant="cedar"`, through both
     `solve_lost_in_space` and a cedar tracker, and checks that a bad name raises.

## Compatibility

* **ABI**: appending fields changes `sizeof(st_lis_options)` and `sizeof(st_config)`. A
  caller compiled against the old header would pass structs that are too short, and the
  library would read past them, so callers must recompile. The library is 0.1.0 and has no
  stable-ABI promise yet, so the change was taken without a size or version field. The
  alternative, a `struct_size` first member, would break every caller now.
* **Behaviour**: unchanged for anyone who zero-initialises the options or passes NULL.
* **Exports**: there are no new functions, so there are still 12 `st_*` symbols, and the
  symbol audit stays as it is.

## Benchmark that uses it

`tests/parity/speed_vs_tetra3.py` times two pairs on the 23 real-sky photos, one thread, 21 runs
each:

* **ESA pair**: ESA tetra3 vs startracker-cpp on `esa_default.stdb`.
* **cedar pair**: cedar-solve vs startracker-cpp with `variant="cedar"` on `cedar_default.stdb`.
  * With the variant, the answers match cedar-solve on all 23 photos, and 19 solve. Before
    the variant, 4 answers differed and 14 solved.
  * cedar-solve cannot load ESA's database (it crashes with an `IndexError`), so each pair
    uses its own database.
  * cedar-solve runs with its own defaults, including its 5 s solve timeout. Two photos
    (roof, tree) reach it.
  * Both pairs use the libraries' Python centroider, not the separate cedar-detect server.

The README table and the xkcd chart are then redrawn on one host (this Mac) with both pairs.
The previous Xeon numbers are replaced, not mixed in.
