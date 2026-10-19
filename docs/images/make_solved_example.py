#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Draws docs/images/solved_example.jpg: one real photo, solved by startracker-cpp, with the
catalogue stars drawn where the solution says they are. If the solution is right, every ring
sits on a star.

  STARTRACKER_LIB=build/release/src/libstartracker.dylib \\
      python3 docs/images/make_solved_example.py [--photo NAME] [--out docs/images/solved_example.jpg]

Needs numpy, Pillow, matplotlib and the fixtures (tools/fetch_fixtures.py). The photo is
ESA/Gustav Pettersson, CC BY-SA 3.0 IGO (from ESA tetra3's test data).
"""
import argparse
import math
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "python"))

# Names for the bright stars in the default photo (Hipparcos ID -> name), checked against the
# catalogue positions.
NAMES = {97649: "Altair", 97278: "Tarazed", 98036: "Alshain"}


def quat_matrix(q):
    """Rotation matrix for q = (w, x, y, z), ICRS -> camera."""
    w, x, y, z = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
            [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
            [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--photo", default="2019-07-29T204726_Alt40_Azi135_Try1")
    ap.add_argument("--fixtures", default=os.path.join(REPO, "fixtures"))
    ap.add_argument("--out", default=os.path.join(HERE, "solved_example.jpg"))
    ap.add_argument("--mag", type=float, default=6.0, help="faintest catalogue star to draw")
    a = ap.parse_args()

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
    from PIL import Image
    import startracker as st

    img = Image.open(os.path.join(a.fixtures, "real_sky", a.photo + ".jpg")).convert("L")
    pix = np.ascontiguousarray(np.asarray(img))
    h, w = pix.shape
    tracker = st.Tracker(open(os.path.join(a.fixtures, "cedar_default.stdb"), "rb").read(),
                         st.Camera.ideal(w, h, 20.0), variant="cedar")
    t0 = time.perf_counter()
    cents = tracker.extract_centroids(pix)
    r = tracker.solve_lost_in_space(cents, w, h)
    total_ms = (time.perf_counter() - t0) * 1e3
    if not r.ok:
        sys.exit(f"{a.photo}: not solved ({r.status})")

    # Catalogue stars projected with the solved attitude and field of view (ideal pinhole).
    db = np.load(os.path.join(a.fixtures, "cedar_default_database.npz"))
    stars, ids = db["star_table"], db["star_catalog_IDs"]
    cam = stars[:, 2:5].astype(np.float64) @ np.array(quat_matrix(r.q_cam_icrs)).T
    f = (w / 2) / math.tan(math.radians(r.fov_deg) / 2)
    keep = (cam[:, 2] > 0) & (stars[:, 5] <= a.mag)
    px = cam[keep, 0] / cam[keep, 2] * f + w / 2
    py = cam[keep, 1] / cam[keep, 2] * f + h / 2
    mags, hips = stars[keep, 5], ids[keep]
    inside = (px > -10) & (px < w + 10) & (py > -10) & (py < h + 10)
    px, py, mags, hips = px[inside], py[inside], mags[inside], hips[inside]

    # Display only: remove the smooth sky glow, soften the 8-bit noise, put black above it.
    def box(arr, r, axis):
        pad = [(0, 0), (0, 0)]
        pad[axis] = (r + 1, r)
        c = np.cumsum(np.pad(arr, pad, mode="edge"), axis=axis)
        n = arr.shape[axis]
        hi = np.take(c, np.arange(2 * r + 1, 2 * r + 1 + n), axis=axis)
        lo = np.take(c, np.arange(0, n), axis=axis)
        return (hi - lo) / (2 * r + 1)

    def blur(arr, radius):  # three box passes per axis, close to a Gaussian of this sigma
        r = max(1, int(round(radius)))
        for axis in (0, 1):
            for _ in range(3):
                arr = box(arr, r, axis)
        return arr

    v = pix.astype(np.float64)
    v = blur(v, 1.0) - blur(v, 40)
    lo, hi = np.percentile(v, 97), np.percentile(v, 99.98)
    shown = np.clip((v - lo) / max(hi - lo, 1e-9), 0, 1) ** 0.5

    dpi = 100
    fig = plt.figure(figsize=(w / dpi, h / dpi), dpi=dpi)
    ax = fig.add_axes([0, 0, 1, 1])
    ax.imshow(shown, cmap="gray", vmin=0, vmax=1, interpolation="lanczos")
    ax.set_xlim(0, w)
    ax.set_ylim(h, 0)
    ax.axis("off")
    ring = "#4fd1c5"
    for x, y, m, hip in zip(px, py, mags, hips):
        ax.add_patch(plt.Circle((x, y), 11 - 0.9 * m, fill=False, lw=1.3, color=ring, alpha=0.95))
        name = NAMES.get(int(hip))
        if name:
            ax.text(x + 14, y - 10, name, color=ring, fontsize=13, fontweight="bold")
    text = (f"startracker-cpp, lost in space: {total_ms:.1f} ms (find stars + identify)\n"
            f"RA {r.ra_deg:.2f}°  Dec {r.dec_deg:+.2f}°  roll {r.roll_deg:.1f}°  "
            f"field {r.fov_deg:.2f}°  {r.num_matches} stars matched, {r.rmse_arcsec:.0f}\" RMS\n"
            f"rings: Hipparcos stars to magnitude {a.mag:g}, placed by the solution")
    ax.text(14, h - 14, text, color="white", fontsize=12, va="bottom", family="monospace",
            bbox=dict(facecolor="black", alpha=0.65, edgecolor="none", pad=6))
    ax.text(w - 10, 10, "Photo: ESA/Gustav Pettersson, CC BY-SA 3.0 IGO", color="#bbbbbb",
            fontsize=9, ha="right", va="top")
    fig.savefig(a.out, dpi=dpi, pil_kwargs={"quality": 88})
    print(f"{a.out}: RA {r.ra_deg:.4f} Dec {r.dec_deg:+.4f} FOV {r.fov_deg:.3f} matches {r.num_matches} "
          f"{total_ms:.1f} ms, {len(px)} catalogue stars drawn, named: "
          f"{[NAMES[int(i)] for i in hips if int(i) in NAMES]}")


if __name__ == "__main__":
    main()
