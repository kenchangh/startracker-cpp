#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Python bindings test on the ESA sample image (exit 77 when fixtures are missing).
  STARTRACKER_LIB=<libstartracker.so> test_bindings.py <python dir> <fixtures dir>"""
import os
import sys


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
    if maxval > 255:  # big-endian -> host (little-endian)
        pix[0::2], pix[1::2] = pix[1::2], pix[0::2]
    return w, h, maxval, pix


def main():
    sys.path.insert(0, sys.argv[1])
    fixtures = sys.argv[2]
    db_path, img_path = os.path.join(fixtures, "esa_default.stdb"), os.path.join(fixtures, "esa_alt40.pgm")
    if not (os.path.exists(db_path) and os.path.exists(img_path)):
        print("fixtures missing; skipping")
        sys.exit(77)
    import startracker as st
    print("startracker", st.version())
    w, h, maxval, pix = read_pgm(img_path)
    fmt = st.MONO16 if maxval > 255 else st.MONO8
    t = st.Tracker(open(db_path, "rb").read(), st.Camera.ideal(w, h, 11.4))
    cents = t.extract_centroids(pix, w, h, fmt)
    assert len(cents) >= 8, cents
    r = t.solve_lost_in_space(cents, w, h)
    print(r)
    assert r.ok and r.num_matches >= 8
    rc = t.solve_lost_in_space(cents, w, h, variant="cedar")  # cedar-solve's search, ESA database
    print(rc)
    assert rc.ok and abs(rc.ra_deg - r.ra_deg) < 0.01 and abs(rc.dec_deg - r.dec_deg) < 0.01
    try:
        t.solve_lost_in_space(cents, w, h, variant="nope")
        raise AssertionError("bad variant accepted")
    except ValueError:
        pass
    tc = st.Tracker(open(db_path, "rb").read(), st.Camera.ideal(w, h, 11.4), variant="cedar")
    sc = tc.track_image(pix, 0, w, h, fmt)
    assert sc.ok and sc.mode == "lost_in_space" and abs(sc.ra_deg - r.ra_deg) < 0.01, sc
    s1 = t.track_image(pix, 0, w, h, fmt)
    s2 = t.track_image(pix, 100_000_000, w, h, fmt)
    print(s1, s2)
    assert s1.ok and s1.mode == "lost_in_space"
    assert s2.ok and s2.mode == "tracking" and s2.used_rois
    assert abs(s1.ra_deg - r.ra_deg) < 0.01 and abs(s1.dec_deg - r.dec_deg) < 0.01
    t.reset()
    s3 = t.track_centroids(cents)
    assert s3.mode == "lost_in_space"
    try:
        st.Tracker(b"\0" * 1024, st.Camera.ideal(w, h, 11.4))
        raise AssertionError("bad database accepted")
    except st.StarTrackerError:
        pass
    print("ok")


if __name__ == "__main__":
    main()
