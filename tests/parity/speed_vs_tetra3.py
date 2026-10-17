#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Speed comparison with ESA tetra3 on the real-sky photos (same database, same host).

  OMP_NUM_THREADS=1 <parity env>/venv/bin/python tests/parity/speed_vs_tetra3.py \\
      --env <parity env> --build build/release [--reps 5] [--json out.json]

Per photo, three ways (median of --reps runs each):
  tetra3    ESA's Python library: get_centroids_from_image + solve_from_centroids
  bindings  startracker-cpp through the Python bindings (ctypes + C API overhead included)
  direct    startracker-cpp called from C++ (tools/st_time_images), same pixels
Also checks that all three give the same answer.
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
import tempfile
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
    return platform.processor() or "unknown"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--env", required=True)
    ap.add_argument("--build", required=True, help="build dir with src/libstartracker.so and tools/")
    ap.add_argument("--fixtures", default=os.path.join(REPO, "fixtures"))
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--json")
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(a.env, "tetra3"))
    sys.path.insert(0, os.path.join(REPO, "python"))
    os.environ["STARTRACKER_LIB"] = os.path.abspath(os.path.join(a.build, "src", "libstartracker.so"))
    tmp = tempfile.mkdtemp()
    import numpy as np
    import tetra3
    from PIL import Image
    import startracker as st

    t3 = tetra3.Tetra3(load_database=Path(os.path.abspath(os.path.join(a.fixtures, "default_database"))))
    t3._logger.setLevel(logging.WARNING)
    ours = st.Tracker(open(os.path.join(a.fixtures, "esa_default.stdb"), "rb").read(),
                      st.Camera.ideal(2048, 2048, 20))

    def med(f):
        ts, r = [], None
        for _ in range(a.reps):
            t0 = time.perf_counter()
            r = f()
            ts.append((time.perf_counter() - t0) * 1e3)
        return stt.median(ts), r

    rows = []
    for p in sorted(glob.glob(os.path.join(a.fixtures, "real_sky", "*.jpg"))):
        name = os.path.basename(p)[:-4]
        img = Image.open(p).convert("L")
        arr = np.ascontiguousarray(np.asarray(img))
        h, w = arr.shape
        tc_ms, tc = med(lambda: tetra3.get_centroids_from_image(img))
        ts_ms, tr = med(lambda: t3.solve_from_centroids(tc, (h, w)))
        oc_ms, oc = med(lambda: ours.extract_centroids(arr))
        os_ms, orr = med(lambda: ours.solve_lost_in_space(oc, w, h))
        same = (tr["RA"] is None) == (not orr.ok) and (
            tr["RA"] is None or (abs(tr["RA"] - orr.ra_deg) < 1e-3 and abs(tr["Dec"] - orr.dec_deg) < 1e-3))
        pgm = os.path.join(tmp, name + ".pgm")  # the exact pixels, for the direct C++ run
        with open(pgm, "wb") as f:
            f.write(f"P5\n{w} {h}\n255\n".encode() + arr.astype(np.uint8).tobytes())
        rows.append(dict(image=name, width=w, height=h, solved=bool(orr.ok), same=bool(same), pgm=pgm,
                         ra=None if tr["RA"] is None else float(tr["RA"]), dec=None if tr["Dec"] is None else float(tr["Dec"]),
                         tetra3_centroid_ms=tc_ms, tetra3_solve_ms=ts_ms,
                         bindings_centroid_ms=oc_ms, bindings_solve_ms=os_ms))

    out = subprocess.run([os.path.join(a.build, "tools", "st_time_images"), "--db",
                          os.path.join(a.fixtures, "esa_default.stdb"), "--reps", str(a.reps)] +
                         [r["pgm"] for r in rows], check=True, capture_output=True, text=True).stdout
    direct = {}
    for line in out.splitlines():
        path, c_ms, s_ms, status, ra, dec = line.split()
        direct[path] = (float(c_ms), float(s_ms), status, float(ra), float(dec))
    for r in rows:
        c_ms, s_ms, status, ra, dec = direct[r["pgm"]]
        os.unlink(r.pop("pgm"))
        r["direct_centroid_ms"], r["direct_solve_ms"] = c_ms, s_ms
        ok = status == "kOk"
        r["same"] = bool(r["same"] and ok == r["solved"] and (
            not ok or (abs(ra - r["ra"]) < 1e-3 and abs(dec - r["dec"]) < 1e-3)))
        print(f"{r['image'][:30]:30s} {r['width']}x{r['height']}  total ms: tetra3 "
              f"{r['tetra3_centroid_ms'] + r['tetra3_solve_ms']:7.2f}  bindings "
              f"{r['bindings_centroid_ms'] + r['bindings_solve_ms']:6.2f}  direct "
              f"{c_ms + s_ms:6.2f}   {'solved' if r['solved'] else 'no match':8s} same={r['same']}")

    def summary(sel):
        out = dict(n=len(sel))
        for p in ("tetra3", "bindings", "direct"):
            for k in ("centroid", "solve"):
                out[f"{p}_{k}_ms"] = stt.median(r[f"{p}_{k}_ms"] for r in sel)
            out[f"{p}_total_ms"] = stt.median(r[f"{p}_centroid_ms"] + r[f"{p}_solve_ms"] for r in sel)
        return out

    out = dict(host=cpu_model(), cpus=os.cpu_count(), reps=a.reps,
               solved=summary([r for r in rows if r["solved"]]),
               unsolved=summary([r for r in rows if not r["solved"]]),
               all_same=all(r["same"] for r in rows), images=rows)
    for k in ("solved", "unsolved"):
        s = out[k]
        print(f"\n{k} photos ({s['n']}), medians in ms:   tetra3   bindings   direct")
        for part in ("centroid", "solve", "total"):
            t = s[f"tetra3_{part}_ms"]
            print(f"  {part:9s} {t:9.3f} {s[f'bindings_{part}_ms']:9.3f} ({t / s[f'bindings_{part}_ms']:4.1f}x)"
                  f" {s[f'direct_{part}_ms']:8.3f} ({t / s[f'direct_{part}_ms']:4.1f}x)")
    print("all answers identical to tetra3:", out["all_same"])
    if a.json:
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)
    sys.exit(0 if out["all_same"] else 1)


if __name__ == "__main__":
    main()
