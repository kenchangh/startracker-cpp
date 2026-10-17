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
  real_sky       23 real night-sky photos from cedar-solve's examples (see credits there)
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
    # Real night-sky photographs from cedar-solve's examples (ESA images: CC BY-SA 3.0 IGO; the
    # others were released by their authors as tetra3 test images). Fetched, never committed.
    "real_sky": [
        ("real_sky/2019-07-29T204726_Alt40_Azi-135_Try1.jpg", CEDAR + "/examples/data/medium_fov/2019-07-29T204726_Alt40_Azi-135_Try1.jpg",
         "8c1f29b49e6e041ded847a9b17a78d9bb7959169c96db0873a62d10ec2d639d9"),
        ("real_sky/2019-07-29T204726_Alt40_Azi-45_Try1.jpg", CEDAR + "/examples/data/medium_fov/2019-07-29T204726_Alt40_Azi-45_Try1.jpg",
         "ff259417146d4c140780f313e5d04fb72ea335dd4daa2e711c1a055289d28a93"),
        ("real_sky/2019-07-29T204726_Alt40_Azi135_Try1.jpg", CEDAR + "/examples/data/medium_fov/2019-07-29T204726_Alt40_Azi135_Try1.jpg",
         "4e4b1c77595d32b9479ed18e80bce59f5bbf010db754dd3d223b9d9710538f4c"),
        ("real_sky/2019-07-29T204726_Alt40_Azi45_Try1.jpg", CEDAR + "/examples/data/medium_fov/2019-07-29T204726_Alt40_Azi45_Try1.jpg",
         "30d077e366fa0f34bea8b4192039c1e2236c77f0b2ab6c4fa1ff75ee60c0a9f0"),
        ("real_sky/2019-07-29T204726_Alt60_Azi-135_Try1.jpg", CEDAR + "/examples/data/medium_fov/2019-07-29T204726_Alt60_Azi-135_Try1.jpg",
         "787000d9088f5768c38d3f2df4a910492b472128f6c155f1b4b9e6752ab81d75"),
        ("real_sky/2019-07-29T204726_Alt60_Azi-45_Try1.jpg", CEDAR + "/examples/data/medium_fov/2019-07-29T204726_Alt60_Azi-45_Try1.jpg",
         "160db9a6bf0f1184b0c339e245d6f1ba8cdebfaea0aa73816340f217cc947e4f"),
        ("real_sky/2019-07-29T204726_Alt60_Azi135_Try1.jpg", CEDAR + "/examples/data/medium_fov/2019-07-29T204726_Alt60_Azi135_Try1.jpg",
         "831f49d1c20f706cde249539546e6b56cf93485de5dbdc44340e8268c664e19b"),
        ("real_sky/2019-07-29T204726_Alt60_Azi45_Try1.jpg", CEDAR + "/examples/data/medium_fov/2019-07-29T204726_Alt60_Azi45_Try1.jpg",
         "24ef450f045954c70782106139596b06e4e0bd70ab16e6390da8c12e9fd9b365"),
        ("real_sky/crappy.jpg", CEDAR + "/examples/data/medium_fov/crappy.jpg",
         "d6f7e1fcf5192124c4dab5c1ccacc6f511e6e860fc8ce86de816f741b73169f2"),
        ("real_sky/discord_seoul3.jpg", CEDAR + "/examples/data/medium_fov/discord_seoul3.jpg",
         "2532e9cf80f3183182d7b929dece45d82b01cd66641656e74f37558f0fdf534d"),
        ("real_sky/discord_seoul4.jpg", CEDAR + "/examples/data/medium_fov/discord_seoul4.jpg",
         "70c6ce68f7ce67edd393fbe7f9ff3ffe3ed038c861fc20956e9439863d4c5817"),
        ("real_sky/hale_bopp.jpg", CEDAR + "/examples/data/medium_fov/hale_bopp.jpg",
         "9fd946abb74b1387e828351b3522eb559aa7eb4be611c19629d3f2beac5de85e"),
        ("real_sky/jan1_g90_e100ms.jpg", CEDAR + "/examples/data/medium_fov/jan1_g90_e100ms.jpg",
         "ccd5e7233694e4b2464d096c7cf984cd6e388096a5cd04447431afd51f5f9d09"),
        ("real_sky/jan1_g90_e50ms.jpg", CEDAR + "/examples/data/medium_fov/jan1_g90_e50ms.jpg",
         "13187b98e2b48249dccebd5996f1168c157d9fff269c53eab564c8a23ff29b75"),
        ("real_sky/orion2.jpg", CEDAR + "/examples/data/medium_fov/orion2.jpg",
         "94aa6fb73e9787529f74f9b43eb2477e5cbfc85ee0ef9bda9348fd02df124090"),
        ("real_sky/orion_belt.jpg", CEDAR + "/examples/data/medium_fov/orion_belt.jpg",
         "247eac12723be3ba2ab17aac527c4bbc65c0064188dbd6d3d07a417d784204f4"),
        ("real_sky/orion_trees.jpg", CEDAR + "/examples/data/medium_fov/orion_trees.jpg",
         "6e466338874f36221a0cd96547c87d59bcf1ece5fd88b3d2b546ae251a4739bb"),
        ("real_sky/pleiades.jpg", CEDAR + "/examples/data/medium_fov/pleiades.jpg",
         "89e67cc5000162f97c83fe9818875b87135565e1cfbf90c43299bf9e427b529c"),
        ("real_sky/roof.jpg", CEDAR + "/examples/data/medium_fov/roof.jpg",
         "992f8e1b3d8173e0f48e33db3a0daa3daa3d4578384d1f080d7a20e3c21be64e"),
        ("real_sky/test2_g100_e50ms.jpg", CEDAR + "/examples/data/medium_fov/test2_g100_e50ms.jpg",
         "60b3511a970ae9480dccb984dee48413ee4a32365d1817275a11920f7c735f97"),
        ("real_sky/test_5mp_g100_e50ms.jpg", CEDAR + "/examples/data/medium_fov/test_5mp_g100_e50ms.jpg",
         "8724ffeebe0fa4ef8e648eaf648587bf8fb0ea6306198f7bf2c3d4599a0c3b87"),
        ("real_sky/tree.jpg", CEDAR + "/examples/data/medium_fov/tree.jpg",
         "d7340daf36f7a8a130715a81845549ef2ec519f59fb1a40aa422cd108fe9d26e"),
        ("real_sky/virgo.jpg", CEDAR + "/examples/data/medium_fov/virgo.jpg",
         "e658341f011fe8b6129b573d442124cddc12cd59ca0ad7c2e410af9ed9a78515"),
    ],
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
    os.makedirs(os.path.dirname(path), exist_ok=True)
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
    if a.ndim == 3:  # colour: ITU-R 601 luma, kept 8-bit
        a = np.clip(np.rint(a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114), 0, 255).astype(np.uint8)
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
            rs = os.path.join(a.dir, "real_sky")
            if os.path.isdir(rs):
                for f in sorted(os.listdir(rs)):
                    if f.endswith(".jpg") and not os.path.exists(os.path.join(rs, f[:-4] + ".pgm")):
                        to_pgm(os.path.join(rs, f), os.path.join(rs, f[:-4] + ".pgm"))
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
