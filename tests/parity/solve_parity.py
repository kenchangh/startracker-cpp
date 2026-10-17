#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Solver parity: ESA tetra3 vs startracker-cpp on tetra3's own centroids.

  solve_parity.py --env <parity env> --cases-bin <st_solve_cases> --db <esa_default.stdb>
                  --fixtures <fixtures dir> [--limit N] [--report out.md]

For every image (ESA samples + cedar-solve synthetic corpus) tetra3 extracts centroids with its
default settings; both solvers then run on the same centroids with several option sets.
Pass criteria per case (docs/design/00-overview.md §5): same status, same matched catalog stars
in the same order, boresight < 1 arcsec, roll < 5 arcsec, FOV relative difference < 1e-5.
"""
import argparse
import glob
import math
import os
import subprocess
import sys
import tempfile

OPTION_SETS = {
    "esa": {
        "default": {},
        "fov_hint": {"fov_estimate": "FOV", "fov_max_error": 1.0},
        "no_distortion": {"distortion": None},
        "dist_range": {"distortion": (-0.2, 0.1)},
    },
    # cedar-solve dropped the distortion range search; its timeout is disabled for determinism.
    "cedar": {
        "default": {},
        "fov_hint": {"fov_estimate": "FOV", "fov_max_error": 1.0},
        "no_distortion": {"distortion": None},
    },
}
TOL_BORESIGHT = 1.0   # arcsec
TOL_ROLL = 5.0        # arcsec
TOL_FOV = 1e-5        # relative


def images(fixtures, limit):
    out = []
    for name, fov in (("esa_alt40.tiff", 11.4), ("esa_alt60.tiff", 11.4)):
        p = os.path.join(fixtures, name)
        if os.path.exists(p):
            out.append((name.split(".")[0], p, fov))
    for p in sorted(glob.glob(os.path.join(fixtures, "cedar_synthetic_v1", "*.png"))):
        out.append(("cedar_" + os.path.basename(p)[:-4], p, 12.71))
    return out[:limit] if limit else out


def radec_vec(ra, dec):
    ra, dec = math.radians(ra), math.radians(dec)
    return (math.cos(ra) * math.cos(dec), math.sin(ra) * math.cos(dec), math.sin(dec))


def angle_arcsec(a, b):
    d = math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))
    return math.degrees(2 * math.asin(min(1.0, d / 2))) * 3600


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--env", required=True)
    ap.add_argument("--cases-bin", required=True)
    ap.add_argument("--db", required=True)
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--report")
    ap.add_argument("--impl", choices=["esa", "cedar"], default="esa")
    a = ap.parse_args()

    sys.path.insert(0, os.path.join(a.env, "tetra3" if a.impl == "esa" else "cedar-solve"))
    import numpy as np
    import tetra3
    import logging
    from PIL import Image

    from pathlib import Path
    dbname = "default_database" if a.impl == "esa" else "cedar_default_database"
    t3 = tetra3.Tetra3(load_database=Path(os.path.abspath(os.path.join(a.fixtures, dbname))))
    t3._logger.setLevel(logging.WARNING)

    ref = {}
    lines = []
    skipped = []
    for name, path, fov in images(a.fixtures, a.limit):
        img = Image.open(path)
        cents = tetra3.get_centroids_from_image(img)
        w, h = img.size
        for oname, o in OPTION_SETS[a.impl].items():
            kw = {k: (fov if v == "FOV" else v) for k, v in o.items()}
            if a.impl == "cedar":
                kw["solve_timeout"] = None
            try:
                r = t3.solve_from_centroids(cents, (h, w), return_matches=True, **kw)
            except IndexError:
                # cedar-solve bug with more centroids than verification_stars_per_fov (Q1).
                skipped.append(f"{name}/{oname}")
                continue
            case = f"{name}/{oname}"
            ref[case] = r
            opts = []
            if a.impl == "cedar":
                opts.append("variants cedar threshold 1e-5")
            opts.append(f"fov_est {kw.get('fov_estimate', 'nan')}")
            opts.append(f"fov_err {kw.get('fov_max_error', 'nan')}")
            d = kw.get("distortion", 0)
            if d is None:
                opts.append("dist_mode none")
            elif isinstance(d, tuple):
                opts.append(f"dist_mode range dmin {d[0]} dmax {d[1]}")
            else:
                opts.append(f"dist_mode fixed dist {d}")
            lines.append(f"case {case}\nsize {w} {h}\nopt {' '.join(opts)}\n"
                         f"centroids {len(cents)}\n")
            lines.extend(f"{c[0]!r} {c[1]!r}\n" for c in cents)
            lines.append("end\n")

    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.writelines(lines)
        case_file = f.name
    out = subprocess.run([a.cases_bin, a.db, case_file], check=True, capture_output=True,
                         text=True).stdout.splitlines()
    os.unlink(case_file)

    ours = {}
    i = 0
    while i < len(out):
        p = out[i].split()
        if p[0] == "result":
            matched = [int(x) for x in out[i + 1].split()[1:]]
            ours[p[1]] = {"status": p[2], "ra": float(p[3]), "dec": float(p[4]),
                          "roll": float(p[5]), "fov": float(p[6]), "dist": float(p[7]),
                          "rmse": float(p[8]), "matches": int(p[9]), "prob": float(p[10]),
                          "us": float(p[11]), "matched": matched}
            i += 3
        else:
            i += 1

    rows, failures = [], []
    worst = {"bore": 0.0, "roll": 0.0, "fov": 0.0}
    solved = 0
    for case, r in ref.items():
        o = ours[case]
        t_ok = r["RA"] is not None
        o_ok = o["status"] == "kOk"
        problems = []
        if t_ok != o_ok:
            problems.append(f"status tetra3={'solved' if t_ok else 'none'} ours={o['status']}")
        elif t_ok:
            solved += 1
            ids = [int(x) for x in r["matched_catID"]]
            if ids != o["matched"]:
                problems.append(f"matched stars differ ({len(ids)} vs {len(o['matched'])})")
            bore = angle_arcsec(radec_vec(r["RA"], r["Dec"]), radec_vec(o["ra"], o["dec"]))
            droll = abs((r["Roll"] - o["roll"] + 180) % 360 - 180) * 3600
            dfov = abs(r["FOV"] - o["fov"]) / r["FOV"]
            worst["bore"] = max(worst["bore"], bore)
            worst["roll"] = max(worst["roll"], droll)
            worst["fov"] = max(worst["fov"], dfov)
            if bore > TOL_BORESIGHT:
                problems.append(f"boresight {bore:.3f}\"")
            if droll > TOL_ROLL:
                problems.append(f"roll {droll:.3f}\"")
            if dfov > TOL_FOV:
                problems.append(f"fov rel {dfov:.2e}")
        rows.append((case, t_ok, o["status"], problems))
        if problems:
            failures.append((case, problems))

    print(f"cases {len(ref)}, solved by tetra3 {sum(1 for r in ref.values() if r['RA'] is not None)}, "
          f"both solved {solved}, failures {len(failures)}")
    def pct(v, q):
        v = sorted(v)
        return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))] if v else float("nan")
    t_ref = [r["T_solve"] for r in ref.values()]
    t_ours = [o["us"] / 1000 for o in ours.values()]
    print(f"solve time ms (this machine, all cases incl. failures): reference median "
          f"{pct(t_ref, .5):.2f} p99 {pct(t_ref, .99):.2f} max {max(t_ref):.2f}; ours median "
          f"{pct(t_ours, .5):.3f} p99 {pct(t_ours, .99):.3f} max {max(t_ours):.3f}")
    if skipped:
        print(f"skipped {len(skipped)} cases where the reference raised IndexError: {skipped[:5]}")
    print(f"worst boresight {worst['bore']:.4f}\", roll {worst['roll']:.4f}\", "
          f"fov rel {worst['fov']:.2e}")
    for case, probs in failures[:40]:
        print(f"  FAIL {case}: {'; '.join(probs)}")
    if a.report:
        with open(a.report, "w") as f:
            f.write("| case | tetra3 | ours | problems |\n|---|---|---|---|\n")
            for case, t_ok, st, probs in rows:
                f.write(f"| {case} | {'solved' if t_ok else '-'} | {st} | {'; '.join(probs)} |\n")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
