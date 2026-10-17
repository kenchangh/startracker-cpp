#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Download test fixtures into ./fixtures (never committed) and verify SHA-256 sums.

  tools/fetch_fixtures.py [--dir fixtures] [--only NAME ...] [--dbtool build/dev/tools/st_dbtool]

Fixtures and their licenses:
  tetra3_db      ESA tetra3 default database (Apache-2.0, esa/tetra3 @ f9fa2eb)
  esa_images     ESA tetra3 sample images (CC BY-SA 3.0 IGO, ESA/Gustav Pettersson)
  cedar_db       cedar-solve default database (Apache-2.0, smroid/cedar-solve @ 607e3f8)
  bsc5           Yale Bright Star Catalogue 5 (public catalogue; copy from cedar-solve tests)
  cedar_corpus   cedar-solve synthetic e2e corpus (99 fields rendered from Gaia DR3)
  hip_main       Hipparcos main catalogue from CDS (I/239). Optional: some networks block CDS.

With --dbtool, the tetra3 database is also converted to fixtures/esa_default.stdb.
Images are converted to 16-bit PGM (fixtures/*.pgm) for the C++ tests; this needs Pillow.
"""
import argparse
import hashlib
import io
import os
import subprocess
import sys
import tarfile
import urllib.request

ESA = "https://raw.githubusercontent.com/esa/tetra3/f9fa2eb"
CEDAR = "https://raw.githubusercontent.com/smroid/cedar-solve/607e3f8"

FIXTURES = {
    "tetra3_db": [("default_database.npz", ESA + "/tetra3/data/default_database.npz",
                   "4fad84d8d7d922e240abbb557dd6329ed94468c76657eebd4a3d9d042ab0f412")],
    "esa_images": [
        ("esa_alt40.tiff", ESA + "/examples/data/2019-07-29T204726_Alt40_Azi-135_Try1.tiff",
         "f5d6c27ac7e8e2114fbeb45196380703e138031819714db7e39f3e101b32b557"),
        ("esa_alt60.tiff", ESA + "/examples/data/2019-07-29T204726_Alt60_Azi-135_Try1.tiff",
         "f288f6e1fb3a1299626956e3d44740c0260348c65dd9296ac8e07bb27524a5e9"),
    ],
    "cedar_db": [("cedar_default_database.npz", CEDAR + "/tetra3/data/default_database.npz",
                  "3bbb922bfaae799788bedc1a3911c3e14210c95d04802700f40b4246c5c04081")],
    "bsc5": [("bsc5", CEDAR + "/tests/data/bsc5",
              "e471d02eaf4eecb61c12f879a1cb6432ba9d7b68a9a8c5654a1eb42a0c8cc340")],
    "cedar_corpus": [("cedar_e2e_synthetic_v1.tar.gz",
                      "https://github.com/smroid/cedar-solve/releases/download/e2e-corpus-v1/"
                      "cedar_e2e_synthetic_v1.tar.gz",
                      "98a54e2dea4984ad84d6eb129b56ff08317a6c935d3a8bf795ce3373e40dfee5")],
    # CDS is unreachable from some sandboxes (optional); hash pinned from a CI download.
    "hip_main": [("hip_main.dat", "https://cdsarc.cds.unistra.fr/ftp/I/239/hip_main.dat",
                  "58ceabb104d647160d9437ce6e513a02a036bb4ad9f8879a5a22fd52943616e0")],
}
OPTIONAL = {"hip_main"}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(path, url, digest):
    if os.path.exists(path) and (digest is None or sha256(path) == digest):
        return True
    tmp = path + ".part"
    print(f"downloading {url}")
    with urllib.request.urlopen(url, timeout=300) as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
    got = sha256(tmp)
    if digest is not None and got != digest:
        os.remove(tmp)
        raise RuntimeError(f"SHA-256 mismatch for {url}: got {got}, want {digest}")
    if digest is None:
        print(f"note: {os.path.basename(path)} is not pinned; sha256={got}")
    os.replace(tmp, path)
    return True


def to_pgm(src, dst):
    from PIL import Image
    import numpy as np
    img = Image.open(src)
    a = np.asarray(img)
    if a.ndim == 3:
        a = (a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114)
    maxval = 255 if a.dtype == np.uint8 else 65535
    a = a.astype(">u2" if maxval > 255 else "u1")
    with open(dst, "wb") as f:
        f.write(f"P5\n{a.shape[1]} {a.shape[0]}\n{maxval}\n".encode())
        f.write(a.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(os.path.dirname(__file__), "..", "fixtures"))
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--dbtool")
    ap.add_argument("--no-convert-images", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.dir, exist_ok=True)
    names = a.only or list(FIXTURES)
    failed = []
    for name in names:
        for fname, url, digest in FIXTURES[name]:
            path = os.path.join(a.dir, fname)
            try:
                fetch(path, url, digest)
            except Exception as e:  # noqa: BLE001
                if name in OPTIONAL:
                    print(f"warning: optional fixture {name} unavailable: {e}")
                    continue
                failed.append(f"{name}: {e}")
    if failed:
        sys.exit("failed:\n  " + "\n  ".join(failed))

    corpus = os.path.join(a.dir, "cedar_e2e_synthetic_v1.tar.gz")
    out = os.path.join(a.dir, "cedar_synthetic_v1")
    if os.path.exists(corpus) and not os.path.isdir(out):
        with tarfile.open(corpus) as t:
            t.extractall(out, filter="data")
    if not a.no_convert_images:
        try:
            for tif in ("esa_alt40", "esa_alt60"):
                src = os.path.join(a.dir, tif + ".tiff")
                if os.path.exists(src) and not os.path.exists(os.path.join(a.dir, tif + ".pgm")):
                    to_pgm(src, os.path.join(a.dir, tif + ".pgm"))
            if os.path.isdir(out):
                for f in sorted(os.listdir(out)):
                    if f.endswith(".png") and not os.path.exists(os.path.join(out, f[:-4] + ".pgm")):
                        to_pgm(os.path.join(out, f), os.path.join(out, f[:-4] + ".pgm"))
        except ImportError:
            print("note: Pillow not installed; images not converted to PGM")
    for npz, stdb in (("default_database.npz", "esa_default.stdb"),
                      ("cedar_default_database.npz", "cedar_default.stdb")):
        src, dst = os.path.join(a.dir, npz), os.path.join(a.dir, stdb)
        if a.dbtool and os.path.exists(src) and not os.path.exists(dst):
            subprocess.check_call([sys.executable, os.path.join(os.path.dirname(__file__),
                                   "convert_tetra3_npz.py"), src, dst, "--dbtool", a.dbtool])


if __name__ == "__main__":
    main()
