# Phase 4: centroider, synthetic renderer, end-to-end parity

Status: **implemented** (2026-10-27). Written as built.

## 1. Centroider (`include/st/centroid/centroider.hpp`)

Full-frame path = ESA tetra3 `get_centroids_from_image`, reproduced including its float32
arithmetic:

| Step | tetra3 | ours |
|---|---|---|
| input | `np.asarray(img, float32)` | 8-bit, or 16-bit container for 10/12/14/16-bit sensors; any stride |
| background | `uniform_filter(size=25, mode='reflect')`, axis 0 then 1, float32 intermediate | same; scipy's running mean in double (`tmp += (in - out) / size`) |
| noise | `sqrt(np.mean(residual**2))` (float32) | numpy's float32 sum order (8192-element chunks, pairwise inside; bit-exact against `np.sum` on 8/8 test arrays) |
| threshold | `residual > sigma * std` | same, float32 compare |
| opening | 3x3 cross, border 0 | same |
| labels | `scipy.ndimage.label`, 4-connected, raster numbering | two-pass union-find, same numbering |
| statistics | sum, first and second moments, area/sum/axis-ratio filters, +0.5 | same (numpy pairwise sums in double) |
| order | by sum, brightest first | same (ties by label order) |

Supported modes: background local-mean / global-mean / global-median / none; sigma
global-root-square / global-median-abs / local-root-square; fixed threshold; optional centroid
window. tetra3's `local_median` modes (slow) and crop/downsample are not provided; the ROI mode
covers windowed processing.

Flight additions: hot-pixel map (bit per pixel, replaced by the mean of non-hot neighbours),
trail rejection (max major/minor axis ratio), fixed maximum blob count (excess reported as
`blob_overflow`), and an ROI mode (per-window border mean and RMS, threshold, brightest
component) for tracking.

Memory: 14 bytes per pixel of the configured frame (float image, float scratch, two masks,
labels) plus about 8 bytes x 4 per blob of capacity. 1280x960 with 4096 blobs: 17.3 MB
(`Centroider::workspace_bytes`). Worst case per frame: O(W·H) for each pass; statistics
O(sum of accepted bounding boxes); sorting O(B·k) for k returned spots.

## 2. Synthetic renderer (`tools/cpp/render.hpp`, `st_render`)

Stars from a database plus optional random clutter; calibrated camera (Brown-Conrady
distortion, parity); attitude and body rate (motion smear sub-stepped at 0.25 px);
pixel-integrated Gaussian PSF; photon shot noise, read noise, gain, bias, 8/12/16-bit
quantisation and saturation; hot pixels. Deterministic per seed on every platform (own
normal/Poisson samplers on `std::mt19937_64`). Truth = mid-exposure star positions and
attitude.

## 3. End-to-end parity (`tests/parity/e2e_parity.py`)

tetra3 `get_centroids_from_image` + `solve_from_image` vs `st_centroid` + our solver on the
same image. Measured on the dev box:

| Images | Centroid lists identical (count, order) | Worst centroid difference | Solve cases | Both solved | Failures | Worst boresight |
|---|---|---|---|---|---|---|
| ESA samples + cedar corpus (101) | 101 / 101 | 6.1e-5 px | 202 | 182 | 0 | 0.0004" |
| our renders (20, FOV 14°) | 20 / 20 | 6.1e-5 px | 40 | 38 | 0 | 0.0000" |

(The 6e-5 px residual comes from summation order inside a region; tetra3 sorts pixels with an
unstable argsort.)

## 4. Synthetic sweeps (`st_sweep`)

Render -> centroider -> solver with tetra3 defaults (no FOV hint), ESA default database,
1280x960, nominal: FOV 14°, 12-bit, read noise 5 e-, background 100 e-, PSF sigma 1 px,
100 ms exposure, plus 100,000 random clutter stars of magnitude 7-9. "Correct" = boresight
error below 60". 20 trials per point. Dev box: Intel Xeon @ 2.8 GHz container, GCC 11 Release;
time = centroiding + solve.

| sweep | value | trials | solved | correct | boresight median / p95 (") | roll median (") | time median / max (ms) |
|---|---|---|---|---|---|---|---|
| nominal | - | 20 | 20 | 20 | 1.5 / 4.2 | 1.7 | 16.0 / 16.7 |
| fov_deg | 8.00 | 20 | 19 | 19 | 0.9 / 2.5 | 4.6 | 17.0 / 26.9 |
| fov_deg | 11.0 | 20 | 20 | 20 | 1.2 / 3.6 | 3.3 | 17.0 / 22.0 |
| fov_deg | 18.0 | 20 | 20 | 20 | 3.7 / 19.5 | 2.6 | 16.6 / 24.4 |
| fov_deg | 24.0 | 20 | 20 | 20 | 9.7 / 29.0 | 3.4 | 16.4 / 25.4 |
| sensitivity | 0.10 | 20 | 20 | 20 | 2.2 / 3.3 | 13.2 | 16.8 / 21.8 |
| sensitivity | 0.30 | 20 | 20 | 20 | 1.5 / 4.8 | 6.7 | 15.9 / 23.2 |
| sensitivity | 3.00 | 20 | 20 | 20 | 1.0 / 4.6 | 2.0 | 16.5 / 22.7 |
| sensitivity | 10.0 | 20 | 20 | 20 | 0.9 / 4.1 | 1.4 | 16.6 / 20.1 |
| read_noise_e | 20.00 | 20 | 20 | 20 | 1.3 / 3.8 | 4.0 | 17.4 / 23.7 |
| read_noise_e | 50.00 | 20 | 20 | 20 | 1.2 / 3.7 | 4.2 | 15.8 / 23.4 |
| read_noise_e | 100.0 | 20 | 20 | 20 | 2.2 / 4.2 | 13.1 | 16.1 / 21.5 |
| rate_deg_s | 0.20 | 20 | 20 | 20 | 1.4 / 4.4 | 2.4 | 16.1 / 23.5 |
| rate_deg_s | 0.50 | 20 | 20 | 20 | 1.4 / 4.6 | 2.0 | 16.1 / 22.2 |
| rate_deg_s | 1.00 | 20 | 20 | 20 | 1.9 / 4.6 | 8.3 | 16.4 / 23.0 |
| rate_deg_s | 2.00 | 20 | 20 | 19 | 7.6 / 16.7 | 59.2 | 16.1 / 24.1 |

Notes: the database covers 10-30°, so 8° is outside its design range (19/20 still solve).
Larger FOVs show tetra3's coarse-FOV attitude error (phase 3, §6). At 2°/s the stars are 18 px
trails; most still pass the area filter.

## 5. Tests

Unit (no fixtures): sub-pixel accuracy and order with zero allocations, 8-bit with stride,
max_returned, hot-pixel map, trail rejection, blob overflow, every background/sigma mode,
invalid input, ROI extraction (zero allocations), numpy sum. Fuzzing: `st_fuzz_centroider`
(random sizes, modes and content) ran 119k inputs in 60 s without findings. CI runs the
end-to-end parity and the full sweep (report in the job summary); ctest runs the nominal
point when the fixture is present.
