#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generator parity: ESA tetra3 (stable magnitude sort) vs st_dbtool generate on BSC5.

  generator_parity.py --env <parity env dir> --dbtool <st_dbtool> --bsc5 <bsc5> [--unpatched]

Exit 0 when the unordered pattern sets are identical. With --unpatched, also reports the
overlap with unmodified tetra3 (informational: tie order differs, quirk f).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

GEN = """
import sys, numpy as np, logging, pathlib
sys.path.insert(0, sys.argv[1])
import tetra3
t3 = tetra3.Tetra3(load_database=None)
t3._logger.setLevel(logging.WARNING)
t3.generate_database(max_fov=30, min_fov=20, star_catalog='bsc5', star_max_magnitude=6,
                     epoch_proper_motion=None)
pc = t3.pattern_catalog
rows = pc[np.any(pc != 0, axis=1)]
sets = np.unique(np.sort(t3.star_catalog_IDs[rows], axis=1), axis=0)
np.savetxt(sys.argv[2], sets, fmt='%d')
print('tetra3: stars', len(t3.star_catalog_IDs), 'rows', len(rows), 'unique sets', len(sets))
"""


CEDAR_GEN = """
import sys, numpy as np, logging
sys.path.insert(0, sys.argv[1])
import tetra3
t3 = tetra3.Tetra3(load_database=None)
t3._logger.setLevel(logging.WARNING)
t3.generate_database(max_fov=30, min_fov=20, star_catalog=sys.argv[3], star_max_magnitude=6,
                     epoch_proper_motion=None, lattice_field_oversampling=20,
                     patterns_per_lattice_field=50, verification_stars_per_fov=50,
                     pattern_max_error=0.001, linear_probe=True)
pc = t3.pattern_catalog
rows = pc[np.any(pc != 0, axis=1)]
sets = np.unique(np.sort(t3.star_catalog_IDs[rows], axis=1), axis=0)
np.savetxt(sys.argv[2], sets, fmt='%d')
print('cedar-solve: stars', len(t3.star_catalog_IDs), 'rows', len(rows), 'unique sets', len(sets),
      'slots', pc.shape[0])
"""


def cedar_sets(py, pkg_root, bsc5, out):
    subprocess.check_call([py, "-c", CEDAR_GEN, pkg_root, out, os.path.abspath(bsc5)])
    return set(tuple(map(int, l.split())) for l in open(out))


def tetra3_sets(py, pkg_root, bsc5, out):
    shutil.copy(bsc5, os.path.join(pkg_root, "tetra3", "bsc5"))
    subprocess.check_call([py, "-c", GEN, pkg_root, out])
    return set(tuple(map(int, l.split())) for l in open(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--env", required=True)
    ap.add_argument("--dbtool", required=True)
    ap.add_argument("--bsc5", required=True)
    ap.add_argument("--unpatched", action="store_true")
    a = ap.parse_args()
    py = os.path.join(a.env, "venv", "bin", "python")
    with tempfile.TemporaryDirectory() as tmp:
        ours_txt = os.path.join(tmp, "ours.txt")
        subprocess.check_call([a.dbtool, "generate", "--catalog", a.bsc5, "--max-fov", "30",
                               "--min-fov", "20", "--mag", "6", "--epoch", "none",
                               "--sets-out", ours_txt, "-o", os.path.join(tmp, "ours.stdb")])
        ours = set(tuple(map(int, l.split())) for l in open(ours_txt))
        ref = tetra3_sets(py, os.path.join(a.env, "tetra3_stable"), a.bsc5,
                          os.path.join(tmp, "t3s.txt"))
        print(f"ours {len(ours)}, tetra3(stable) {len(ref)}, common {len(ours & ref)}, "
              f"only ours {len(ours - ref)}, only tetra3 {len(ref - ours)}")
        if a.unpatched:
            raw = tetra3_sets(py, os.path.join(a.env, "tetra3"), a.bsc5, os.path.join(tmp, "t3.txt"))
            print(f"unpatched tetra3: {len(raw)} sets, common with ours {len(ours & raw)} "
                  f"({100.0 * len(ours & raw) / len(raw):.2f}%)")
        ok = ours == ref
        # cedar-solve lattice variant (stable-sort cedar-solve vs st_dbtool --lattice).
        lat_txt = os.path.join(tmp, "lat.txt")
        subprocess.check_call([a.dbtool, "generate", "--catalog", a.bsc5, "--max-fov", "30",
                               "--min-fov", "20", "--mag", "6", "--epoch", "none", "--lattice",
                               "--linear-probe", "--verification-stars", "50",
                               "--patterns-per-field", "50", "--lattice-oversampling", "20",
                               "--max-error", "0.001", "--sets-out", lat_txt,
                               "-o", os.path.join(tmp, "lat.stdb")])
        lat = set(tuple(map(int, l.split())) for l in open(lat_txt))
        cref = cedar_sets(py, os.path.join(a.env, "cedar_stable"), a.bsc5, os.path.join(tmp, "c.txt"))
        print(f"lattice: ours {len(lat)}, cedar-solve(stable) {len(cref)}, common {len(lat & cref)}")
        ok = ok and lat == cref
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
