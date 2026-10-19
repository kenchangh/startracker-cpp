#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Plate-solve JPEG/PNG photos with startracker-cpp (lost-in-space, one fresh solve per image).

  tools/solve_images.py --dir ~/Downloads/night_sky [--fov 14] [--db fixtures/cedar_default.stdb]
  tools/solve_images.py --file photo.jpg [--csv results.csv]

Needs Pillow (pip install pillow) and the built library: set STARTRACKER_LIB to
build/release/src/libstartracker.dylib (or .so), or put it next to python/startracker.
"""
import argparse
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))

EXTENSIONS = (".jpg", ".jpeg", ".png")


def find_images(args):
    if args.file:
        return [args.file]
    return sorted(os.path.join(args.dir, n) for n in os.listdir(args.dir)
                  if n.lower().endswith(EXTENSIONS) and not n.startswith("."))


def load_gray(path):
    """8-bit grayscale pixels (row-major bytes), width, height. 16-bit PNGs stay 16-bit."""
    from PIL import Image
    with Image.open(path) as im:
        if im.mode in ("I;16", "I;16B", "I;16L", "I"):
            im = im.convert("I;16")
            return im.tobytes("raw", "I;16"), im.width, im.height, 16
        im = im.convert("L")
        return im.tobytes(), im.width, im.height, 8


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--dir", help="folder of .jpg/.jpeg/.png images (not recursive)")
    src.add_argument("--file", help="one .jpg/.jpeg/.png image")
    p.add_argument("--db", default=os.path.join(HERE, "..", "fixtures", "cedar_default.stdb"),
                   help="star database (.stdb); default fixtures/cedar_default.stdb")
    p.add_argument("--variant", choices=["esa", "cedar"],
                   help="lost-in-space search; default: cedar for a cedar database, else esa")
    p.add_argument("--fov", type=float, default=0.0,
                   help="horizontal field of view guess in degrees (default: unknown, slower)")
    p.add_argument("--fov-error", type=float, default=0.0, help="allowed FOV error in degrees")
    p.add_argument("--csv", help="also write results to this CSV file")
    args = p.parse_args()

    if args.file and not args.file.lower().endswith(EXTENSIONS):
        p.error("--file must be a .jpg, .jpeg or .png")
    if args.dir and not os.path.isdir(args.dir):
        p.error(f"not a directory: {args.dir}")

    try:
        import PIL  # noqa: F401
    except ImportError:
        sys.exit("Pillow is required: pip install pillow")
    import startracker as st

    paths = find_images(args)
    if not paths:
        sys.exit(f"no .jpg/.jpeg/.png files in {args.dir}")

    variant = args.variant or ("cedar" if "cedar" in os.path.basename(args.db) else "esa")
    trackers = {}  # centroider buffers are sized by the camera, so one tracker per image size
    with open(args.db, "rb") as f:
        db = f.read()

    fields = ["image", "status", "ra_deg", "dec_deg", "roll_deg", "fov_deg", "matches",
              "rmse_arcsec", "stars", "solve_ms"]
    rows = []
    for path in paths:
        name = os.path.basename(path)
        try:
            pix, w, h, bits = load_gray(path)
        except Exception as e:
            print(f"{name}: cannot read ({e})")
            rows.append({"image": name, "status": "READ_ERROR"})
            continue
        fmt = st.MONO16 if bits == 16 else st.MONO8
        if (w, h) not in trackers:
            trackers[(w, h)] = st.Tracker(db, st.Camera.ideal(w, h, args.fov or 20.0), variant=variant)
        tracker = trackers[(w, h)]
        cents = tracker.extract_centroids(pix, w, h, fmt)
        r = tracker.solve_lost_in_space(cents, w, h, fov_estimate_deg=args.fov, fov_max_error_deg=args.fov_error)
        row = {"image": name, "status": r.status, "stars": len(cents), "solve_ms": f"{r.solve_ms:.1f}"}
        if r.ok:
            row.update(ra_deg=f"{r.ra_deg:.4f}", dec_deg=f"{r.dec_deg:.4f}", roll_deg=f"{r.roll_deg:.2f}",
                       fov_deg=f"{r.fov_deg:.3f}", matches=r.num_matches, rmse_arcsec=f"{r.rmse_arcsec:.1f}")
            print(f"{name}: OK  RA {r.ra_deg:9.4f}  Dec {r.dec_deg:+8.4f}  roll {r.roll_deg:7.2f}  "
                  f"FOV {r.fov_deg:6.3f}  matches {r.num_matches:2d}  RMSE {r.rmse_arcsec:5.1f}\"  "
                  f"({len(cents)} stars, {r.solve_ms:.1f} ms)")
        else:
            print(f"{name}: {r.status}  ({len(cents)} stars, {r.solve_ms:.1f} ms)")
        rows.append(row)

    solved = sum(1 for r in rows if r["status"] == "OK")
    print(f"\nsolved {solved}/{len(rows)}")

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            wr = csv.DictWriter(f, fieldnames=fields)
            wr.writeheader()
            wr.writerows(rows)
        print(f"wrote {args.csv}")


if __name__ == "__main__":
    main()
