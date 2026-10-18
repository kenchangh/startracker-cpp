#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Speed comparison with ESA tetra3 and cedar-solve on the real-sky photos (same host).

  OMP_NUM_THREADS=1 <parity env>/venv/bin/python tests/parity/speed_vs_tetra3.py \\
      --env <parity env> --build build/release [--reps 5] [--json out.json]

Two pairs, each on one shared star database (cedar-solve cannot load ESA's database):
  ESA database    tetra3  ESA's Python library: get_centroids_from_image + solve_from_centroids
                  ours    startracker-cpp on esa_default.stdb
  cedar database  cedar   cedar-solve's Python library, same two calls (its Python centroider,
                          not the separate cedar-detect server)
                  ours    startracker-cpp on cedar_default.stdb, variant="cedar"
startracker-cpp runs through its Python bindings (ctypes + C API overhead included).
Each time is the median of --reps runs. Also checks that each pair gives the same answer.
cedar-solve stops at its default 5 s solve timeout. Photos where it times out are reported
apart ("timeouts"): their time is the timeout, not a finished search, so they are left out of
the no-match medians. ESA tetra3 has no default timeout.
"""
import argparse
import glob
import json
import logging
import os
import platform
import statistics as stt
import subprocess
import sys
import time
from pathlib import Path

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def cpu_model():
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    try:
        return subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return platform.processor() or "unknown"


def import_tetra3(path):
    """Imports the `tetra3` package found in `path`. ESA tetra3 and cedar-solve both use that name
    (cedar-solve imports itself as `tetra3.<module>`), so the previous copy is unloaded first;
    modules already returned keep working."""
    for k in [k for k in sys.modules if k == "tetra3" or k.startswith("tetra3.")]:
        del sys.modules[k]
    sys.path.insert(0, path)
    try:
        import tetra3
    finally:
        sys.path.remove(path)
    return tetra3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--env", required=True)
    ap.add_argument("--build", required=True, help="build dir with src/libstartracker.{so,dylib}")
    ap.add_argument("--fixtures", default=os.path.join(REPO, "fixtures"))
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--json")
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(REPO, "python"))
    for lib in ("libstartracker.so", "libstartracker.dylib"):
        if os.path.exists(os.path.join(a.build, "src", lib)):
            os.environ["STARTRACKER_LIB"] = os.path.abspath(os.path.join(a.build, "src", lib))
    import numpy as np
    from PIL import Image
    import startracker as st

    esa = import_tetra3(os.path.join(a.env, "tetra3"))
    cedar = import_tetra3(os.path.join(a.env, "cedar-solve"))
    fx = os.path.abspath(a.fixtures)
    pairs = []  # (key, library module, solver, our tracker)
    for key, mod, npz, stdb in (("esa", esa, "default_database", "esa_default.stdb"),
                                ("cedar", cedar, "cedar_default_database", "cedar_default.stdb")):
        t = mod.Tetra3(load_database=Path(os.path.join(fx, npz)))
        t._logger.setLevel(logging.WARNING)
        ours = st.Tracker(open(os.path.join(fx, stdb), "rb").read(), st.Camera.ideal(2048, 2048, 20),
                          variant=key)
        pairs.append((key, mod, t, ours))

    def med(f):
        ts, r = [], None
        for _ in range(a.reps):
            t0 = time.perf_counter()
            r = f()
            ts.append((time.perf_counter() - t0) * 1e3)
        return stt.median(ts), r

    rows = {"esa": [], "cedar": []}
    for p in sorted(glob.glob(os.path.join(fx, "real_sky", "*.jpg"))):
        name = os.path.basename(p)[:-4]
        img = Image.open(p).convert("L")
        arr = np.ascontiguousarray(np.asarray(img))
        h, w = arr.shape
        line = f"{name[:30]:30s} {w}x{h}"
        for key, mod, t, ours in pairs:
            tc_ms, tc = med(lambda: mod.get_centroids_from_image(img))
            ts_ms, tr = med(lambda: t.solve_from_centroids(tc, (h, w)))
            oc_ms, oc = med(lambda: ours.extract_centroids(arr))
            os_ms, orr = med(lambda: ours.solve_lost_in_space(oc, w, h))
            same = (tr["RA"] is None) == (not orr.ok) and (
                tr["RA"] is None or (abs(tr["RA"] - orr.ra_deg) < 1e-3 and abs(tr["Dec"] - orr.dec_deg) < 1e-3))
            ref_timeout = tr.get("status") == 3  # cedar-solve TIMEOUT
            rows[key].append(dict(image=name, width=w, height=h, solved=bool(orr.ok), same=bool(same),
                                  ref_timeout=bool(ref_timeout),
                                  ref_centroid_ms=tc_ms, ref_solve_ms=ts_ms,
                                  ours_centroid_ms=oc_ms, ours_solve_ms=os_ms))
            line += (f"  | {key}: ref {tc_ms + ts_ms:7.2f} ours {oc_ms + os_ms:6.2f} ms "
                     f"{'solved' if orr.ok else 'no match':8s} same={same}{' (ref timeout)' if ref_timeout else ''}")
        print(line, flush=True)

    def summary(sel):
        out = dict(n=len(sel))
        for p in ("ref", "ours"):
            for k in ("centroid", "solve"):
                out[f"{p}_{k}_ms"] = stt.median(r[f"{p}_{k}_ms"] for r in sel) if sel else None
            out[f"{p}_total_ms"] = stt.median(r[f"{p}_centroid_ms"] + r[f"{p}_solve_ms"] for r in sel) if sel else None
        return out

    out = dict(host=cpu_model(), cpus=os.cpu_count(), reps=a.reps)
    ok = True
    for key, label in (("esa", "ESA tetra3"), ("cedar", "cedar-solve")):
        rs = rows[key]
        out[key] = dict(solved=summary([r for r in rs if r["solved"]]),
                        unsolved=summary([r for r in rs if not r["solved"] and not r["ref_timeout"]]),
                        timeouts=[r for r in rs if r["ref_timeout"]],
                        all_same=all(r["same"] for r in rs), images=rs)
        ok &= out[key]["all_same"]
        for k in ("solved", "unsolved"):
            s = out[key][k]
            if not s["n"]:
                continue
            print(f"\n{key} database, {k} photos ({s['n']}), medians in ms:   {label}   startracker-cpp")
            for part in ("centroid", "solve", "total"):
                t = s[f"ref_{part}_ms"]
                print(f"  {part:9s} {t:9.3f} {s[f'ours_{part}_ms']:9.3f} ({t / s[f'ours_{part}_ms']:4.1f}x)")
        for r in out[key]["timeouts"]:
            print(f"  {r['image']}: {label} hit its timeout at {r['ref_centroid_ms'] + r['ref_solve_ms']:.0f} ms; "
                  f"startracker-cpp finished its search in {r['ours_centroid_ms'] + r['ours_solve_ms']:.1f} ms")
        print(f"all answers identical to {label}:", out[key]["all_same"])
    if a.json:
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
