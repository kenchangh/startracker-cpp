# Benchmark details

The summary is in the [README](../README.md). This page covers how the numbers were measured
and how to reproduce them.

## How it was measured

* **Test set:** 23 real night-sky photographs (cedar-solve's `examples/data/medium_fov`). Each
  photo is processed start to finish: find the stars in the image, then identify them.
* **Host:** Apple M3 Max, one thread each. Each number is the median of 21 runs per photo.
* **Databases:** each pair shares one database, ESA's for tetra3 and cedar-solve's for
  cedar-solve. cedar-solve cannot load ESA's database.
* **Star finding:** both libraries use their own Python centroider. cedar-solve can also use a
  separate star finder, cedar-detect, which was not tested.
* **Call overhead:** startracker-cpp was called through its Python bindings, so the ctypes and
  C API overhead counts against it.
* **Same answers:** in both pairs startracker-cpp gives the same answer as the reference
  library on every photo (RA and Dec within 0.001°, and the same photos unsolved).

## cedar-solve's timeout

cedar-solve stops searching after 5 s by default; ESA tetra3 has no limit.

* On 2 photos (roof, tree) cedar-solve stopped without finishing. Left to run, it took 43.6 s
  on one and crashed on the other (an indexing bug in cedar-solve).
* Its other 2 no-match photos have only 5 and 7 stars, so there is little to search:
  37.4 ms for cedar-solve, 8.3 ms for startracker-cpp.
* Timeouts are reported on their own, not folded into a median, because their time is the
  timeout and not a finished search.

## cedar-solve's search

With cedar-solve's database, pass `variant="cedar"` in Python (`ST_VARIANT_CEDAR` in C,
`SolverVariants::cedar()` in C++). It solves 19 of the 23 photos; ESA's search on the same
database solves 14. See [design note 08](design/08-c-api-solver-variants.md).

## Other hosts

* **Tracking:** once the attitude is known, the tracker skips the full-image search and looks
  only at small windows around the predicted stars: about 0.1 ms per frame on a desktop CPU.
* **arm64 Linux:** a GitHub-hosted Arm Neoverse N2 runner took 10.9 ms for a full 1280×960
  frame and 0.03 ms for tracking windows (synthetic database).
* **Raspberry Pi:** no numbers yet. Run `scripts/raspi/run.sh --camera <yours> --bench`.

## Reproduce

```sh
tests/parity/setup_env.sh <env>          # ESA tetra3 @ f9fa2eb and cedar-solve @ 607e3f8
OMP_NUM_THREADS=1 <env>/venv/bin/python tests/parity/speed_vs_tetra3.py \
    --env <env> --build build/release --reps 21 --json docs/images/bench_vs_tetra3.json
python3 docs/images/make_bench_chart.py --font xkcd-script.ttf
```

* The raw numbers are in [images/bench_vs_tetra3.json](images/bench_vs_tetra3.json).
* The xkcd font is not in the repository (its licence is non-commercial); download
  `xkcd-script.ttf` and pass its path with `--font`.
* The README's example photo is drawn by `docs/images/make_solved_example.py`.
