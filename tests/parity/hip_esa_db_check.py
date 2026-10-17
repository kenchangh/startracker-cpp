#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Informational: our generator on Hipparcos (CDS I/239 hip_main.dat) with the ESA default
database's parameters vs the shipped ESA database (default_database.npz).

  hip_esa_db_check.py --dbtool <st_dbtool> --hip fixtures/hip_main.dat --npz fixtures/default_database.npz

Reports the overlap of unordered pattern star-ID sets. The shipped database was built by an
earlier tetra3 run whose tie order in the magnitude sort we do not reproduce exactly (quirk f),
so a small difference is expected; generator correctness is gated by generator_parity.py.
Exits 77 when hip_main.dat is missing (CDS is blocked on some networks).
"""
import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dbtool", required=True)
    ap.add_argument("--hip", required=True)
    ap.add_argument("--npz", required=True)
    a = ap.parse_args()
    if not os.path.exists(a.hip):
        print("hip_main.dat missing; skipping")
        sys.exit(77)
    d = np.load(a.npz)
    p = d["props_packed"]
    ids = d["star_catalog_IDs"]
    pc = d["pattern_catalog"]
    rows = pc[np.any(pc != 0, axis=1)]
    ref = set(map(tuple, np.unique(np.sort(ids[rows], axis=1), axis=0).tolist()))
    args = ["--max-fov", str(float(p["max_fov"])), "--min-fov", str(float(p["min_fov"])),
            "--mag", str(float(p["star_max_magnitude"])),
            "--epoch", str(float(p["epoch_proper_motion"])),
            "--pattern-stars", str(int(p["pattern_stars_per_fov"])),
            "--verification-stars", str(int(p["verification_stars_per_fov"])),
            "--max-error", str(float(p["pattern_max_error"]))]
    with tempfile.TemporaryDirectory() as tmp:
        sets = os.path.join(tmp, "sets.txt")
        subprocess.check_call([a.dbtool, "generate", "--catalog", a.hip, *args, "--sets-out", sets,
                               "-o", os.path.join(tmp, "hip.stdb")])
        ours = set(tuple(map(int, l.split())) for l in open(sets))
    common = len(ours & ref)
    print(f"ESA database: {len(ref)} pattern sets; ours from hip_main: {len(ours)}; common {common} "
          f"({100.0 * common / max(1, len(ref)):.2f}% of ESA)")


if __name__ == "__main__":
    main()
