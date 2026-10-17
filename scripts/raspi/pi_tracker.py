#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Live star tracking on a Raspberry Pi camera (Picamera2), or on saved PGM frames.

Usually started by scripts/raspi/run.sh, which builds the library and picks the database and
field of view for your camera. Direct use:

  python3 scripts/raspi/pi_tracker.py --db fixtures/cedar_default.stdb --fov 22.2 --mode 2028x1520
  python3 scripts/raspi/pi_tracker.py --db fixtures/cedar_default.stdb --fov 11.4 --from-pgm DIR

Camera path: reads the raw Bayer stream (unpacked, no ISP processing), adds each 2x2 Bayer cell
into one grey pixel, and tracks: lost-in-space on the first frame, then windowed tracking.
"""
import argparse
import glob
import os
import sys
import time

import numpy as np

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "python"))


def raw_to_mono(raw_bytes, width, height):
    """Unpacked 16-bit Bayer raw (rows may be padded) -> 2x2-summed mono uint16."""
    raw = raw_bytes.view(np.uint16)[:height, :width]
    mono = (raw[0::2, 0::2].astype(np.uint32) + raw[0::2, 1::2] + raw[1::2, 0::2] + raw[1::2, 1::2])
    return mono.astype(np.uint16)


def read_pgm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts, i = [], 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    w, h, maxval = int(parts[1]), int(parts[2]), int(parts[3])
    dtype = ">u2" if maxval > 255 else "u1"
    return np.frombuffer(data[i + 1:], dtype=dtype, count=w * h).reshape(h, w).astype(
        np.uint16 if maxval > 255 else np.uint8)


def write_pgm(path, img):
    maxval = 255 if img.dtype == np.uint8 else 65535
    with open(path, "wb") as f:
        f.write(f"P5\n{img.shape[1]} {img.shape[0]}\n{maxval}\n".encode())
        f.write(img.astype(">u2" if maxval > 255 else "u1").tobytes())


def pgm_frames(directory, period_ns):
    files = sorted(glob.glob(os.path.join(directory, "*.pgm")))
    if not files:
        sys.exit(f"no .pgm files in {directory}")
    for k, p in enumerate(files):
        yield read_pgm(p), k * period_ns, os.path.basename(p)


def camera_frames(mode_w, mode_h, exposure_ms, gain):
    from picamera2 import Picamera2  # apt: python3-picamera2
    cam = Picamera2()
    modes = [m for m in cam.sensor_modes if tuple(m["size"]) == (mode_w, mode_h)]
    if not modes:
        sizes = ", ".join(f"{m['size'][0]}x{m['size'][1]}" for m in cam.sensor_modes)
        sys.exit(f"sensor has no {mode_w}x{mode_h} raw mode; available: {sizes}")
    mode = max(modes, key=lambda m: m.get("bit_depth", 0))
    fmt = str(mode.get("unpacked") or mode["format"]).replace("_CSI2P", "")
    cfg = cam.create_still_configuration(raw={"format": fmt, "size": (mode_w, mode_h)}, buffer_count=3)
    cam.configure(cfg)
    cam.set_controls({"AeEnable": False, "AwbEnable": False,
                      "ExposureTime": int(exposure_ms * 1000), "AnalogueGain": float(gain)})
    cam.start()
    print(f"camera: {cam.camera_properties.get('Model', '?')}, raw {fmt} {mode_w}x{mode_h}, "
          f"exposure {exposure_ms} ms, gain {gain}", flush=True)
    try:
        while True:
            req = cam.capture_request()
            raw = req.make_array("raw")
            md = req.get_metadata()
            req.release()
            t_ns = int(md.get("SensorTimestamp", time.monotonic_ns())) + int(md.get("ExposureTime", 0)) * 500
            yield raw_to_mono(raw, mode_w, mode_h), t_ns, None
    finally:
        cam.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", required=True, help=".stdb database")
    ap.add_argument("--fov", type=float, required=True, help="horizontal field of view, degrees")
    ap.add_argument("--mode", default="2028x1520", help="sensor raw mode WxH (camera path)")
    ap.add_argument("--exposure-ms", type=float, default=100)
    ap.add_argument("--gain", type=float, default=8.0)
    ap.add_argument("--frames", type=int, default=0, help="stop after N frames (0 = run until Ctrl-C)")
    ap.add_argument("--from-pgm", help="track saved PGM frames from this directory instead of a camera")
    ap.add_argument("--period-ms", type=float, default=100, help="frame spacing for --from-pgm")
    ap.add_argument("--csv", help="append one line per frame to this CSV file")
    ap.add_argument("--save-pgm", help="save every frame here (replay later with --from-pgm or st_replay)")
    ap.add_argument("--no-roi", action="store_true", help="always centroid the full frame")
    a = ap.parse_args()

    os.environ.setdefault("STARTRACKER_LIB", os.path.join(REPO, "build", "pi", "src", "libstartracker.so"))
    import startracker as st

    if a.from_pgm:
        frames = pgm_frames(a.from_pgm, int(a.period_ms * 1e6))
        first, _, _ = next(pgm_frames(a.from_pgm, 0))
        h, w = first.shape
    else:
        mw, mh = (int(v) for v in a.mode.lower().split("x"))
        w, h = mw // 2, mh // 2
        frames = camera_frames(mw, mh, a.exposure_ms, a.gain)

    tracker = st.Tracker(open(a.db, "rb").read(), st.Camera.ideal(w, h, a.fov), roi_tracking=not a.no_roi)
    print(f"tracking {w}x{h} frames, FOV {a.fov} deg, database {os.path.basename(a.db)}; Ctrl-C to stop",
          flush=True)
    if a.save_pgm:
        os.makedirs(a.save_pgm, exist_ok=True)
    csv = open(a.csv, "a") if a.csv else None
    if csv and csv.tell() == 0:
        csv.write("t_ns,status,mode,ra_deg,dec_deg,roll_deg,qw,qx,qy,qz,matches,rmse_arcsec,ms\n")
    n, ok, times = 0, 0, []
    try:
        for img, t_ns, name in frames:
            t0 = time.perf_counter()
            sol = tracker.track_image(img, t_ns=t_ns)
            ms = (time.perf_counter() - t0) * 1e3
            n += 1
            ok += sol.ok
            times.append(ms)
            label = f"{name} " if name else ""
            print(f"{label}{sol.status:9s} {sol.mode:13s} RA {sol.ra_deg:9.4f}  Dec {sol.dec_deg:8.4f}  "
                  f"roll {sol.roll_deg:8.3f}  stars {sol.matches:2d}  {ms:6.1f} ms"
                  f"{'  (windows)' if sol.used_rois else ''}", flush=True)
            if csv:
                if sol.ok:
                    q = sol.q_cam_icrs
                    att = (f"{sol.ra_deg:.6f},{sol.dec_deg:.6f},{sol.roll_deg:.6f},"
                           f"{q[0]:.9f},{q[1]:.9f},{q[2]:.9f},{q[3]:.9f}")
                else:
                    att = ",,,,,,"  # no attitude this frame
                csv.write(f"{t_ns},{sol.status},{sol.mode},{att},{sol.matches},{sol.rmse_arcsec:.2f},{ms:.3f}\n")
            if a.save_pgm:
                write_pgm(os.path.join(a.save_pgm, f"frame_{n:06d}.pgm"), img)
            if a.frames and n >= a.frames:
                break
    except KeyboardInterrupt:
        pass
    finally:
        if csv:
            csv.close()
    if times:
        times.sort()
        print(f"\n{n} frames, {ok} solved; processing time median {times[len(times) // 2]:.1f} ms, "
              f"max {times[-1]:.1f} ms")


if __name__ == "__main__":
    main()
