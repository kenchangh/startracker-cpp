# Phase 5: tracking mode

Status: **implemented** (2026-10-27). Written as built.

## 1. Components

* `SkyIndex` (`include/st/track/sky_index.hpp`): cube-map grid (6 × G² cells) over the star
  table, CSR arrays in the arena, cone query = cells whose bounding cap reaches the cone, then
  an exact dot-product test. Verified equal to brute force on 50 random cones.
* `Tracker` (`include/st/track/tracker.hpp`):
  1. **Predict** the attitude at the frame time: gyro hook (`RateSource::rotation_between`)
     when it answers, otherwise the estimated body rate, otherwise the last attitude.
  2. **Project** the brightest `max_predicted` catalog stars in the predicted cone through the
     calibrated `CameraModel` (Brown-Conrady, principal point, parity).
  3. **Match** each predicted star (brightest first) to the nearest unused centroid within
     `match_radius_px + rate_uncertainty·dt·f`.
  4. **Solve** Wahba's problem (SVD with reflection correction) on pixel rays from the camera
     model, drop residuals above `max(3·rms, 3·σ_px)`, re-solve, gate on RMS.
  5. **Covariance**: σ² (Σ (I − bᵢbᵢᵀ))⁻¹ in the camera frame (QUEST approximation), σ = max(RMS
     residual, `centroid_sigma_px` / f).
  6. **Rate**: least-squares slope of the rotation vectors of the last `rate_window` consecutive
     attitudes (relative to the newest), ω = −cov(τ, r)/var(τ). A two-frame finite difference
     was tried first and was 5× noisier (test failure), hence the window fit.
  7. **Fallback**: after `lost_after_failures` consecutive failures (or with no attitude), the
     centroids are undistorted with the calibrated model and handed to the lost-in-space
     solver (FOV hint from the camera); its attitude is refined at once with the calibrated
     model. This also removes tetra3's coarse-FOV attitude error (phase 3 §6).
  8. **ROI mode**: `predict_rois` gives windows around the brightest predicted stars for
     `Centroider::extract_rois`, so tracking frames need not be centroided in full.

Body-rate convention: q(t + dt) = exp(−ω dt) q(t), ω in the camera frame (same as the
renderer).

## 2. Memory and bounds

P = `max_predicted` (48), C = `max_centroids` (256), W = `rate_window` (≤ 32):
predicted stars 56 P, pairs 8 P, used flags C, ray/reference/residual 56 P, undistorted
centroid copy 16 C: about 10 KB. History ring 32 × 40 B inline. SkyIndex: 4 (6G² + 1 + stars)
+ 32 · 6G² bytes; G = 16 with the ESA database: 86 KB.

Per update: O(6G² + stars in cone) for prediction, O(P·C) matching, O(P) Wahba, O(W) rate; plus
one lost-in-space solve when falling back (bounded by its work budget).

## 3. Tests (synthetic sky, calibrated camera with k1 = −0.05 and offset principal point)

* Acquire with LIS, then 59 tracking frames at 10 Hz and ~0.43°/s, 0.15 px noise: boresight
  < 8", total error within 3σ of the reported covariance, roll the weakest axis, rate within
  3% of truth after 20 frames, zero allocations per update.
* Loss and reacquisition: coasts one bad frame, declares lost after two, reacquires a new sky
  region with LIS.
* Gyro hook: 4.6°/s (21 px per frame) tracked from the first frame.
* ROI tracking on rendered images (motion smear, noise): 6 frames, < 30" from the
  mid-exposure truth.
* Sidereal rotation: Earth-fixed camera, 1 Hz for 120 s: estimated rate within 5% of
  7.2921e-5 rad/s (15.04"/s) and within 5° of the celestial-pole direction.
