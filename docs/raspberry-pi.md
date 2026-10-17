# Running startracker-cpp on a Raspberry Pi

This guide covers Raspberry Pi 4, Pi 5 and Pi Zero 2 W running 64-bit Raspberry Pi OS
(Bookworm), with the official Raspberry Pi camera modules.

> **Status: not yet run on a Raspberry Pi or a real Pi camera.** What has been checked:
>
> * The library builds and its full test suite passes on arm64 Linux in CI (Ubuntu 22.04
>   container, GCC 11 and Clang 14).
> * The Python capture-to-attitude code in §4 was tested on simulated Bayer frames.
> * The wide-field database in §3 was tested on rendered 66° frames.
>
> If you run it on hardware, the `st_bench` output from §6 is the most useful thing to report
> back.

## 1. Pick a camera and lens

The solver needs to know roughly how wide the camera's view is (its horizontal field of
view, FOV) and needs a star database built for that FOV range. Both shipped databases cover
**10–30°**:

* `esa_default.stdb`: ESA tetra3's database, 99 MB, stars to magnitude 7.
* `cedar_default.stdb`: 38 MB, stars to magnitude 8.

Horizontal FOV = 2·atan(sensor width / (2·focal length)).

| camera | sensor | lens | horizontal FOV | database |
|---|---|---|---|---|
| **HQ Camera** (IMX477, 4056×3040, 1.55 µm) | 6.29 mm wide | 16 mm C-mount | 22.2° | shipped ✔ |
| | | 25 mm C-mount | 14.3° | shipped ✔ |
| | | 12 mm | 29.4° | shipped (at the edge) |
| **Global Shutter Camera** (IMX296, 1456×1088, 3.45 µm) | 5.02 mm wide | 16 mm C-mount | 17.8° | shipped ✔ |
| | | 12 mm | 23.6° | shipped ✔ |
| **Camera Module 3** (IMX708, 4608×2592, 1.4 µm) | 6.45 mm wide | built-in 4.74 mm | ≈66° | wide-field DB (§3) |
| **Camera Module 2** (IMX219, 3280×2464, 1.12 µm) | 3.67 mm wide | built-in 3.04 mm | ≈62° | wide-field DB (§3) |
| Camera Module 3 Wide | | built-in | ≈102° | not recommended |

Recommendations:

* **Best choice:** the HQ Camera or Global Shutter Camera with a 12–25 mm C-mount lens. A
  narrower field gives finer arcseconds per pixel, and a larger aperture shows more stars.
* **Global Shutter Camera:** its sensor exposes every pixel at the same moment, so a moving
  or vibrating platform does not skew the star field. With the rolling-shutter HQ Camera and
  Camera Modules, rows are exposed at slightly different times.
* **Camera Module 2/3:** these work for bright stars with a wide-field database. Accuracy
  per pixel is much coarser, and lens distortion at 60°+ matters, so calibrate the lens
  (§5).

## 2. Build on the Pi

```sh
sudo apt update
sudo apt install -y git cmake ninja-build g++ python3-numpy python3-picamera2
git clone https://github.com/kenchangh/startracker-cpp.git
cd startracker-cpp
cmake --preset release -DST_BUILD_TESTS=OFF -DST_WERROR=OFF
cmake --build --preset release
```

* **What it builds:**
  * `build/release/src/libstartracker.so`: the C library, also used by the Python bindings.
  * The static C++ libraries `st_core` and `st_platform`.
  * The tools in `build/release/tools/`.
* **Requirements:** CMake ≥ 3.21 and GCC ≥ 11. Bookworm ships CMake 3.25 and GCC 12.
* **Why `-DST_WERROR=OFF`:** a compiler version CI doesn't use may add new warnings, and this
  stops them from failing the build.
* **Pi Zero 2 W:** use `cmake --build --preset release -j2` to limit parallel compiles; its
  512 MB of RAM can run out with more.
* **Tests:** to run the tests on the Pi as well, drop `-DST_BUILD_TESTS=OFF` (this downloads
  GoogleTest), then run `ctest --preset release`.
* **32-bit Raspberry Pi OS** has not been tried. Use the 64-bit image.

## 3. Get a star database

**For a 10–30° field** (HQ / Global Shutter Camera with a 12–25 mm lens), fetch and convert
the tetra3 databases:

```sh
python3 tools/fetch_fixtures.py --only tetra3_db cedar_db --dbtool build/release/tools/st_dbtool
ls fixtures/*.stdb          # esa_default.stdb (99 MB), cedar_default.stdb (38 MB)
```

* **Pi Zero 2 W:** prefer the 38 MB `cedar_default.stdb`.
* **Converting elsewhere:** you can convert on a desktop and copy the `.stdb` file over. The
  format is little-endian, so the same file works on x86-64 and arm64.

**For a wide field (Camera Module 2/3)**, generate a database from the Yale Bright Star
Catalogue (BSC5). This takes about a second:

```sh
python3 tools/fetch_fixtures.py --only bsc5
build/release/tools/st_dbtool generate --catalog fixtures/bsc5 \
    --min-fov 50 --max-fov 75 --mag 6 --epoch 2026 -o wide.stdb
```

* **Result:** this database is 7.8 MB. It solved 20 of 20 rendered 66° frames, with a median
  boresight error of 1.9″.
* **Range:** set `--min-fov` and `--max-fov` to bracket your actual FOV.
* **Fainter stars:** for faint stars or other fields, generate from Hipparcos instead
  (`--catalog hip_main.dat`, downloaded from CDS I/239).

## 4. Capture with Picamera2 and solve (Python)

The official Pi cameras are colour sensors behind the libcamera stack. The simplest reliable
path is Picamera2:

1. Read the **raw** sensor data. Raw skips the colour processing, denoising and sharpening,
   which smear and bias star centroids.
2. Add each 2×2 Bayer cell together into one grey pixel.
3. Hand the result to the tracker.

```python
import time
import numpy as np
from picamera2 import Picamera2
import startracker as st   # PYTHONPATH=python  STARTRACKER_LIB=build/release/src/libstartracker.so

RAW_W, RAW_H = 2028, 1520          # HQ Camera 2x2-binned raw mode; Global Shutter: 1456, 1088
FOV_DEG = 22.2                     # from the table in §1 (or calibrate, §5)

def raw_to_mono(raw_bytes, width, height):
    """Unpacked 16-bit Bayer raw (rows may be padded) -> 2x2-summed mono uint16."""
    raw = raw_bytes.view(np.uint16)[:height, :width]          # drop row padding
    mono = (raw[0::2, 0::2].astype(np.uint32) + raw[0::2, 1::2] +
            raw[1::2, 0::2] + raw[1::2, 1::2])                 # R + G + G + B
    return mono.astype(np.uint16)                              # 4 x 12-bit fits in 16 bits

picam2 = Picamera2()
config = picam2.create_still_configuration(
    raw={"format": "SRGGB12", "size": (RAW_W, RAW_H)},       # unpacked 12-bit Bayer
    buffer_count=3)
picam2.configure(config)
picam2.set_controls({"AeEnable": False, "AwbEnable": False,
                     "ExposureTime": 100_000,                  # µs; 50-200 ms at night
                     "AnalogueGain": 8.0})
picam2.start()

w, h = RAW_W // 2, RAW_H // 2
tracker = st.Tracker(open("fixtures/cedar_default.stdb", "rb").read(),
                     st.Camera.ideal(w, h, FOV_DEG))

while True:
    request = picam2.capture_request()
    raw = request.make_array("raw")
    md = request.get_metadata()
    request.release()
    # Tag the frame with mid-exposure time (SensorTimestamp is ns, ExposureTime is µs).
    t_ns = md["SensorTimestamp"] + md["ExposureTime"] * 500
    t0 = time.perf_counter()
    sol = tracker.track_image(raw_to_mono(raw, RAW_W, RAW_H), t_ns=t_ns)
    dt = (time.perf_counter() - t0) * 1e3
    print(f"{sol.status:9s} {sol.mode:13s} RA {sol.ra_deg:9.4f} Dec {sol.dec_deg:8.4f} "
          f"roll {sol.roll_deg:8.3f} stars {sol.matches:2d} {dt:6.1f} ms")
```

* **The first frame** runs a lost-in-space solve. Later frames track: they centroid only
  small windows around the predicted stars, which is much cheaper.
* **`sol` contains:**
  * `q_cam_icrs`: the attitude quaternion (w, x, y, z), from ICRS into the camera frame
    (+Z = boresight, +X = right, +Y = down);
  * `covariance`: 3×3, in rad²;
  * `rate`: rad/s in the camera frame, once enough frames have been tracked;
  * `ra_deg`, `dec_deg`, `roll_deg`: degrees, using tetra3's conventions.
* **Pi 5 raw format:** the default raw format is compressed (`PISP_COMP1`), so keep asking
  explicitly for an unpacked format such as `SRGGB12`.
* **Global Shutter Camera raw:** its raw format is 10-bit (`SRGGB10`). The 2×2 sum still fits
  in 16 bits.
* **Simpler alternative:** use the processed `"main"` stream in `"YUV420"` format and take the
  Y (brightness) plane as a `uint8` image. It is more convenient but less linear; set
  `"NoiseReductionMode"` off if you go this way.

**Test without a camera.** Run the same code on a saved frame:
`tracker.track_image(np.asarray(...uint16...))`. `st_render` makes synthetic star fields
(§6).

## 5. Calibrate the lens (recommended)

`st.Camera.ideal()` assumes a perfect pinhole lens. Real lenses, especially the 60°+ module
lenses, bend the image, and that distortion becomes attitude error near the edges. Measure
the focal length and distortion once, then pass them in:

```python
# fx, fy, cx, cy in pixels of the image you hand to the tracker (after 2x2 summing);
# k1..k3, p1, p2: Brown-Conrady terms (OpenCV calibrateCamera's order k1, k2, p1, p2, k3).
cam = st.Camera(width=1014, height=760, fx=2580.6, fy=2580.6, cx=507.0, cy=380.0,
                k1=-0.05, k2=0.0, k3=0.0, p1=0.0, p2=0.0)
```

* **Without a calibration:** start from the datasheet. fx = focal length ÷ effective pixel
  pitch. For example, an HQ Camera with a 16 mm lens in the 2028×1520 raw mode, then 2×2
  summed, has an effective pitch of 6.2 µm, so fx ≈ 2580 px.
* **How to calibrate:** OpenCV's chessboard calibration gives all of these values.
* **Where it applies:** the tracker uses the calibrated model for every tracked frame. It
  also uses it to refine each lost-in-space solution.

## 6. Measure on your Pi

```sh
build/release/tools/st_bench --db fixtures/cedar_default.stdb --size 1014x760 --fov 22.2 --frames 40
build/release/tools/st_bench --size 728x544 --fov 17.8          # synthetic database, GS-camera size
```

* **What it reports:** median, p99 and max time for each step (full-frame centroiding, ROI
  centroiding, lost-in-space solve, tracker update), plus per-frame pipeline latency, memory
  use, and the CPU and compiler.
* **No Pi numbers yet:** none have been measured, so don't infer them from desktop results.
* **For reference:** on an x86-64 Xeon at 2.8 GHz, a 1280×960 frame took 15.5 ms to centroid
  in full, 0.08 ms in tracking windows, and 0.28 ms to solve lost-in-space. Expect the Pi to
  be several times slower; measure to know.

**Replaying recorded frames.** `st_replay pack` turns saved PGM frames into a replay file, and
`st_replay run` plays it through the full pipeline:

```sh
build/release/tools/st_replay pack --out night.strp --period-ms 200 frames/*.pgm
build/release/tools/st_replay run --db fixtures/cedar_default.stdb --in night.strp --fov 22.2
```

## 7. Memory

| item | size |
|---|---|
| database | 99 MB (ESA), 38 MB (cedar), 8 MB (wide BSC5); memory-mapped, read in place |
| centroider working memory | ≈ 14.3 bytes per pixel: 11 MB at 1014×760, 44 MB at 2028×1520, 176 MB at full HQ resolution |
| solver + tracker | < 0.2 MB |
| pipeline frame slots | 2 bytes/pixel per slot (4 slots by default) |

* **Pi Zero 2 W (512 MB):** use the cedar or BSC5 database and a binned or summed image of
  about 1 MP or less.
* **Pi 4 / Pi 5:** any combination fits.

## 8. Using C or C++ instead of Python

* **C:** link `libstartracker.so` and include `include/st/c/st.h`. Call
  `st_tracker_memory_size` and `st_tracker_create` once, then `st_track_image` per frame,
  using the same 2×2-summed `uint16` image as above. See [usage.md](usage.md) §4.
* **C++ streaming:**
  1. Implement `st::FrameSource` (`include/st/pipeline/frame_source.hpp`): its `acquire()`
     copies one frame into the buffer you are given and returns a timestamp.
  2. Get the frames from the libcamera C++ API (the library Picamera2 wraps).
  3. Run `st::Pipeline`, either with `step()` in your own loop or with `st::PipelineRunner`
     for three threads.

  The built-in `V4L2Source` reads only monochrome formats (GREY, Y10, Y12, Y16). That covers
  mono sensor boards such as the OV9281 and monochrome IMX296 modules. It does not cover the
  official colour Pi cameras, which deliver Bayer raw through libcamera. It has not been run
  against a real device yet.

## 9. Real-time tips

* **Keep the database in RAM:** in C++, call `MappedFile::open(path, /*prefault=*/true)` and
  then `lock()` (mlock). A lookup then never waits on the SD card. This needs a large enough
  `ulimit -l` or `CAP_IPC_LOCK`.
* **Thread priority and CPU pinning:** `PipelineRunner` can run each stage at SCHED_FIFO
  priority on a dedicated core (`StageThreadConfig{priority, cpu}`). This needs
  `CAP_SYS_NICE`, e.g. `sudo setcap cap_sys_nice+ep ./your_app`. You can also reserve cores
  with `isolcpus=2,3` in `/boot/firmware/cmdline.txt`.
* **Avoid throttling:**
  * `echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`
  * Fit a heatsink or fan: the Pi 5 throttles at 85 °C, which shows up as p99 latency spikes.
* **Exposure:** use the longest exposure that keeps stars round at your platform's motion.
  Turn auto-exposure and auto white balance off (as in §4) so frames are consistent.
* **Focus:** focus at infinity on a bright star. Defocused stars spread over more pixels, so
  fewer of them pass the centroider's area limits (5–100 pixels by default).
