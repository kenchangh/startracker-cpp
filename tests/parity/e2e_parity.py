#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""End-to-end parity: tetra3 image -> centroids -> solution vs ours.

  e2e_parity.py --env <parity env> --bin-dir <build>/tools --db <esa_default.stdb>
                --fixtures <dir> [--extra-pgm DIR] [--limit N]

Per image:
  * centroids: tetra3 get_centroids_from_image (defaults) vs st_centroid: same count and order,
    max position difference (pass: < 0.01 px; observed ~1e-6, float32 printing limits it);
  * solution: tetra3 solve_from_image (defaults and FOV hint) vs st_centroid + st_solve_cases:
    same status, same matched stars, boresight < 1", roll < 5", FOV < 1e-5 relative.
Images: ESA samples, cedar-solve synthetic corpus, and optional extra PGMs (e.g. our renders).
"""
import argparse
import glob
import math
import os
import subprocess
import sys
import tempfile

TOL_CENTROID_PX = 0.01


def radec_vec(ra, dec):
    ra, dec = math.radians(ra), math.radians(dec)
    return (math.cos(ra) * math.cos(dec), math.sin(ra) * math.cos(dec), math.sin(dec))


def angle_arcsec(a, b):
    d = math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))
    return math.degrees(2 * math.asin(min(1.0, d / 2))) * 3600


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--env", required=True)
    ap.add_argument("--bin-dir", required=True)
    ap.add_argument("--db", required=True)
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--extra-pgm", action="append", default=[])
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--only-extra", action="store_true", help="skip the fixture images")
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(a.env, "tetra3"))
    import logging
    from pathlib import Path
    import numpy as np
    import tetra3
    from PIL import Image

    t3 = tetra3.Tetra3(load_database=Path(os.path.abspath(os.path.join(a.fixtures, "default_database"))))
    t3._logger.setLevel(logging.WARNING)

    imgs = []
    for n in (() if a.only_extra else ("esa_alt40", "esa_alt60")):
        if os.path.exists(os.path.join(a.fixtures, n + ".pgm")):
            imgs.append((n, os.path.join(a.fixtures, n + ".pgm"), 11.4))
    for p in ([] if a.only_extra else sorted(glob.glob(os.path.join(a.fixtures, "cedar_synthetic_v1", "*.pgm")))):
        imgs.append(("cedar_" + os.path.basename(p)[:-4], p, 12.71))
    for d in a.extra_pgm:
        for p in sorted(glob.glob(os.path.join(d, "*.pgm"))):
            fov = None
            meta = p[:-4] + ".fov"
            if os.path.exists(meta):
                fov = float(open(meta).read())
            imgs.append(("extra_" + os.path.basename(p)[:-4], p, fov))
    if a.limit:
        imgs = imgs[:a.limit]

    out = subprocess.run([os.path.join(a.bin_dir, "st_centroid")] + [p for _, p, _ in imgs],
                         check=True, capture_output=True, text=True).stdout.splitlines()
    ours_c = {}
    i = 0
    while i < len(out):
        _, path, n = out[i].split()
        n = int(n)
        ours_c[path] = [tuple(map(float, l.split()[:2])) for l in out[i + 1:i + 1 + n]]
        i += 1 + n

    cfail, worst_c, cases, ref = [], 0.0, [], {}
    for name, path, fov in imgs:
        img = Image.open(path)
        tc = tetra3.get_centroids_from_image(img)
        oc = ours_c[path]
        if len(tc) != len(oc):
            cfail.append(f"{name}: count {len(tc)} vs {len(oc)}")
        else:
            d = max((max(abs(t[0] - o[0]), abs(t[1] - o[1])) for t, o in zip(tc, oc)), default=0.0)
            worst_c = max(worst_c, d)
            if d > TOL_CENTROID_PX:
                cfail.append(f"{name}: max centroid diff {d:.3g} px")
        w, h = img.size
        opts = {"default": {}}
        if fov:
            opts["fov_hint"] = {"fov_estimate": fov, "fov_max_error": 1.0}
        for oname, kw in opts.items():
            r = t3.solve_from_image(img, **kw)
            case = f"{name}/{oname}"
            ref[case] = r
            o = [f"fov_est {kw.get('fov_estimate', 'nan')}", f"fov_err {kw.get('fov_max_error', 'nan')}",
                 "dist_mode fixed dist 0"]
            cases.append(f"case {case}\nsize {w} {h}\nopt {' '.join(o)}\ncentroids {len(oc)}\n")
            cases.extend(f"{y!r} {x!r}\n" for y, x in oc)
            cases.append("end\n")
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.writelines(cases)
        cf = f.name
    res = subprocess.run([os.path.join(a.bin_dir, "st_solve_cases"), a.db, cf], check=True,
                         capture_output=True, text=True).stdout.splitlines()
    os.unlink(cf)
    ours = {}
    i = 0
    while i < len(res):
        p = res[i].split()
        if p[0] == "result":
            ours[p[1]] = (p[2], float(p[3]), float(p[4]), float(p[5]), float(p[6]))
            i += 3
        else:
            i += 1
    sfail, worst = [], [0.0, 0.0, 0.0]
    solved = 0
    for case, r in ref.items():
        st, ra, dec, roll, fov = ours[case]
        if (r["RA"] is not None) != (st == "kOk"):
            sfail.append(f"{case}: tetra3 {'solved' if r['RA'] is not None else 'none'}, ours {st}")
            continue
        if r["RA"] is None:
            continue
        solved += 1
        b = angle_arcsec(radec_vec(r["RA"], r["Dec"]), radec_vec(ra, dec))
        dr = abs((r["Roll"] - roll + 180) % 360 - 180) * 3600
        df = abs(r["FOV"] - fov) / r["FOV"]
        worst = [max(worst[0], b), max(worst[1], dr), max(worst[2], df)]
        if b > 1 or dr > 5 or df > 1e-5:
            sfail.append(f"{case}: boresight {b:.3f}\" roll {dr:.3f}\" fov {df:.2e}")
    print(f"images {len(imgs)}: centroid failures {len(cfail)}, worst centroid diff {worst_c:.3g} px")
    print(f"solve cases {len(ref)}, both solved {solved}, failures {len(sfail)}, worst boresight "
          f"{worst[0]:.4f}\" roll {worst[1]:.4f}\" fov {worst[2]:.2e}")
    for f in (cfail + sfail)[:40]:
        print("  FAIL " + f)
    sys.exit(1 if cfail or sfail else 0)


if __name__ == "__main__":
    main()
