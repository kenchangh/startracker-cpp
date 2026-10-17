#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real-sky check: our lost-in-space solutions vs astrometry.net plate solutions.

  # 1. references (needs astrometry.net's solve-field / wcs-xy2rd / wcsinfo and index files):
  realsky_check.py make-references --images fixtures/real_sky --out refs.json
  # 2. comparison (needs libstartracker; STARTRACKER_LIB or the python package dir):
  realsky_check.py check --images fixtures/real_sky --refs refs.json --db fixtures/esa_default.stdb ...

For every image astrometry.net solves, the reference holds RA/Dec of the image centre, the four
corners and the four edge midpoints. Our solution (quaternion + fitted FOV, ideal pinhole, no
distortion, as tetra3 solves by default) predicts the same points. Pass criteria:
  * every solution we report agrees: centre within 3 px, all points within 1.5% of the image
    diagonal (lens distortion is not modelled, so corners carry its error);
  * --require NAME... must be solved (by at least one database).
A wrong solution is a failure; no solution is only reported.
"""
import argparse
import glob
import json
import math
import os
import re
import subprocess
import sys
import tempfile


def points(w, h):
    """Our pixel convention (origin at the top-left corner)."""
    return [(w / 2, h / 2), (0.5, 0.5), (w - 0.5, 0.5), (0.5, h - 0.5), (w - 0.5, h - 0.5),
            (w / 2, 0.5), (w / 2, h - 0.5), (0.5, h / 2), (w - 0.5, h / 2)]


def image_size(path):
    with open(path, "rb") as f:
        head = f.read(64).split()
    return int(head[1]), int(head[2])


def make_references(a):
    refs = {}
    work = tempfile.mkdtemp()
    for pgm in sorted(glob.glob(os.path.join(a.images, "*.pgm"))):
        name = os.path.basename(pgm)[:-4]
        src = pgm[:-4] + ".jpg" if os.path.exists(pgm[:-4] + ".jpg") else pgm
        w, h = image_size(pgm)
        cmd = ["solve-field", "--overwrite", "--no-plots", "--no-verify", "--downsample", "2",
               "--cpulimit", str(a.cpulimit), "--scale-units", "degwidth", "--scale-low", "2",
               "--scale-high", "90", "--dir", work, "--out", name, src]
        r = subprocess.run(cmd, capture_output=True, text=True)
        wcs = os.path.join(work, name + ".wcs")
        if r.returncode != 0 or not os.path.exists(wcs):
            print(f"{name}: astrometry.net did not solve")
            continue
        info = subprocess.run(["wcsinfo", wcs], capture_output=True, text=True, check=True).stdout
        scale = float(re.search(r"^pixscale (\S+)", info, re.M).group(1))
        pts = []
        for x, y in points(w, h):
            # FITS pixel (1, 1) is the centre of our first pixel (0.5, 0.5).
            o = subprocess.run(["wcs-xy2rd", "-w", wcs, "-x", repr(x + 0.5), "-y", repr(y + 0.5)],
                               capture_output=True, text=True, check=True).stdout
            m = re.search(r"RA,Dec \(([-\d.e+]+), ([-\d.e+]+)\)", o)
            pts.append([x, y, float(m.group(1)), float(m.group(2))])
        refs[name] = {"width": w, "height": h, "pixscale_arcsec": scale, "points": pts}
        print(f"{name}: solved, {scale:.2f}\"/px, centre RA {pts[0][2]:.5f} Dec {pts[0][3]:.5f}")
    with open(a.out, "w") as f:
        json.dump(refs, f, indent=1, sort_keys=True)
    print(f"{len(refs)} references written to {a.out}")


def vec(ra, dec):
    ra, dec = math.radians(ra), math.radians(dec)
    return (math.cos(ra) * math.cos(dec), math.sin(ra) * math.cos(dec), math.sin(dec))


def sep_arcsec(a, b):
    d = math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))
    return math.degrees(2 * math.asin(min(1.0, d / 2))) * 3600


def rotate_inverse(q, v):
    """R(q)^T v for q = (w, x, y, z) rotating ICRS -> camera."""
    w, x, y, z = q
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
         [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
         [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]
    return tuple(sum(r[i][k] * v[i] for i in range(3)) for k in range(3))


def read_pgm(path):
    data = open(path, "rb").read()
    parts, i = [], 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    w, h, maxval = int(parts[1]), int(parts[2]), int(parts[3])
    pix = bytearray(data[i:])
    if maxval > 255:
        pix[0::2], pix[1::2] = pix[1::2], pix[0::2]
    return w, h, maxval, pix


def check(a):
    sys.path.insert(0, a.python)
    import startracker as st
    refs = json.load(open(a.refs))
    trackers = [(os.path.basename(d), st.Tracker(open(d, "rb").read(), st.Camera.ideal(2048, 2048, 20)))
                for d in a.db]
    rows, wrong, solved_any = [], [], set()
    for pgm in sorted(glob.glob(os.path.join(a.images, "*.pgm"))):
        name = os.path.basename(pgm)[:-4]
        w, h, maxval, pix = read_pgm(pgm)
        ref = refs.get(name)
        for dbname, t in trackers:
            cents = t.extract_centroids(pix, w, h, st.MONO16 if maxval > 255 else st.MONO8)
            r = t.solve_lost_in_space(cents, w, h)
            if not r.ok:
                rows.append((name, dbname, r.status, "-", "-", "solved" if ref else "unsolved"))
                continue
            solved_any.add(name)
            if ref is None:
                rows.append((name, dbname, "OK", "-", "-", "no reference"))
                continue
            f = (w / 2) / math.tan(math.radians(r.fov_deg) / 2)
            errs = []
            for x, y, ra, dec in ref["points"]:
                ray = ((x - w / 2) / f, (y - h / 2) / f, 1.0)
                n = math.sqrt(sum(c * c for c in ray))
                v = rotate_inverse(r.q_cam_icrs, tuple(c / n for c in ray))
                errs.append(sep_arcsec(v, vec(ra, dec)) / ref["pixscale_arcsec"])
            diag = math.hypot(w, h)
            ok = errs[0] < 3 and max(errs) < 0.015 * diag
            rows.append((name, dbname, "OK", f"{errs[0]:.2f}", f"{max(errs):.1f}", "agrees" if ok else "WRONG"))
            if not ok:
                wrong.append(f"{name} ({dbname}): centre {errs[0]:.2f} px, worst point {max(errs):.1f} px")
    print("| image | database | ours | centre err (px) | worst point err (px) | vs astrometry.net |")
    print("|---|---|---|---|---|---|")
    for r in rows:
        print("| " + " | ".join(r) + " |")
    missing = [n for n in a.require if n not in solved_any]
    print(f"\nreferences {len(refs)}; images solved by us {len(solved_any)}; wrong {len(wrong)}; "
          f"required but unsolved {len(missing)}")
    for m in wrong + [f"required image unsolved: {n}" for n in missing]:
        print("  FAIL " + m)
    sys.exit(1 if wrong or missing else 0)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make-references")
    m.add_argument("--images", required=True)
    m.add_argument("--out", required=True)
    m.add_argument("--cpulimit", type=int, default=60)
    c = sub.add_parser("check")
    c.add_argument("--images", required=True)
    c.add_argument("--refs", required=True)
    c.add_argument("--db", action="append", required=True)
    c.add_argument("--python", default=os.path.join(os.path.dirname(__file__), "..", "..", "python"))
    c.add_argument("--require", nargs="*", default=[])
    a = ap.parse_args()
    make_references(a) if a.cmd == "make-references" else check(a)


if __name__ == "__main__":
    main()
