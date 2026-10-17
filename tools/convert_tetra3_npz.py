#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Convert a tetra3 (ESA or cedar-solve) .npz database to a startracker-cpp blob.

Usage: convert_tetra3_npz.py <in.npz> <out.stdb> [--dbtool path/to/st_dbtool]

Arrays are copied unchanged (identical slot layout), so the converted database gives the
same lookups as tetra3. Requires numpy (any version).
"""
import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("npz")
    ap.add_argument("out")
    ap.add_argument("--dbtool", default="st_dbtool")
    a = ap.parse_args()
    with np.load(a.npz) as d, tempfile.TemporaryDirectory() as tmp:
        for key in ("star_table", "pattern_catalog", "star_catalog_IDs",
                    "pattern_largest_edge", "pattern_key_hashes"):
            if key in d.files:
                np.save(os.path.join(tmp, key + ".npy"), np.ascontiguousarray(d[key]))
        props = d["props_packed"]
        lines = []
        for name in props.dtype.names:
            v = props[name][()]
            if isinstance(v, np.ndarray):
                v = ",".join(repr(float(x)) for x in v.ravel())
            elif isinstance(v, (np.floating, float)):
                v = repr(float(v))
            elif isinstance(v, (np.bool_, bool)):
                v = "True" if v else "False"
            lines.append(f"{name}={v}")
        names = props.dtype.names
        # cedar-solve databases use linear probing and record their pattern count.
        if "pattern_key_hashes" in d.files or ("num_patterns" in names):
            lines.append("probe=linear")
        if "verification_stars_per_fov" not in names and "catalog_stars_per_fov" in names:
            lines.append(f"verification_stars_per_fov={props['catalog_stars_per_fov'][()]}")
        with open(os.path.join(tmp, "props.txt"), "w") as f:
            f.write("\n".join(lines) + "\n")
        r = subprocess.run([a.dbtool, "pack", tmp, a.out])
        sys.exit(r.returncode)


if __name__ == "__main__":
    main()
