#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Checks that a converted blob holds exactly the arrays of the source tetra3 .npz.

  converter_check.py <in.npz> <blob.stdb>
"""
import struct
import sys

import numpy as np


def main(npz, blob):
    b = open(blob, "rb").read()
    assert b[:8] == b"STDB\r\n\x1a\n", "bad magic"
    num_stars, num_slots = struct.unpack_from("<II", b, 32)
    idx_bytes = b[56]
    sections = [struct.unpack_from("<QQ", b, 152 + 16 * i) for i in range(5)]
    with np.load(npz) as d:
        st = d["star_table"]
        pc = d["pattern_catalog"]
        assert st.shape == (num_stars, 6) and pc.shape == (num_slots, 4)
        off, size = sections[0]
        got = np.frombuffer(b, dtype="<f4", count=num_stars * 6, offset=off).reshape(-1, 6)
        assert np.array_equal(got, st), "star table differs"
        off, size = sections[2]
        dt = "<u2" if idx_bytes == 2 else "<u4"
        got = np.frombuffer(b, dtype=dt, count=num_slots * 4, offset=off).reshape(-1, 4)
        assert np.array_equal(got, pc.astype(dt)), "pattern table differs"
        if "star_catalog_IDs" in d.files and d["star_catalog_IDs"].ndim == 1:
            off, size = sections[1]
            got = np.frombuffer(b, dtype="<u4", count=num_stars, offset=off)
            assert np.array_equal(got, d["star_catalog_IDs"]), "catalog IDs differ"
    print(f"converter check ok: {num_stars} stars, {num_slots} slots identical")


if __name__ == "__main__":
    main(*sys.argv[1:3])
