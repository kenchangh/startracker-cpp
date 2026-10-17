/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ST_C_ST_H
#define ST_C_ST_H

/*
 * startracker-cpp C API (libstartracker).
 *
 * One st_tracker bundles a database, a centroider, the lost-in-space solver and the tracker.
 * Every byte it uses lives in caller memory:
 *   size_t need = st_tracker_memory_size(&cfg, db, db_size);
 *   void* mem = aligned_alloc(64, need);   // any memory, 64-byte aligned
 *   st_tracker* t; st_tracker_create(&cfg, db, db_size, mem, need, &t);
 * The database blob (an .stdb file, e.g. memory-mapped) must stay valid and unchanged while
 * the tracker exists. No function allocates, throws, or keeps global state. A tracker is not
 * thread-safe; separate trackers are independent.
 *
 * Conventions: pixel coordinates have their origin at the image's top-left corner, as in the
 * C++ API (st::Centroid) and tetra3. The camera frame
 * is +Z boresight, +X right, +Y down. q_cam_icrs rotates ICRS vectors into the camera frame
 * (scalar first: w, x, y, z). RA/Dec/roll are in degrees, tetra3 conventions.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(ST_C_BUILD)
#define ST_API __attribute__((visibility("default")))
#else
#define ST_API
#endif

/* Same values as st::Status. */
typedef enum st_status {
  ST_OK = 0,
  ST_INVALID_ARGUMENT = 1,
  ST_CAPACITY_EXCEEDED = 2,
  ST_ARENA_EXHAUSTED = 3,
  ST_NOT_INITIALIZED = 4,
  ST_CORRUPT_DATA = 5,
  ST_CHECKSUM_MISMATCH = 6,
  ST_VERSION_MISMATCH = 7,
  ST_UNSUPPORTED = 8,
  ST_TOO_FEW_STARS = 9,
  ST_NO_MATCH = 10,
  ST_TIMEOUT = 11,
  ST_WORK_BUDGET_EXHAUSTED = 12,
  ST_CANCELLED = 13,
  ST_IO_ERROR = 14
} st_status;

typedef enum st_pixel_format { ST_MONO8 = 0, ST_MONO16 = 1 } st_pixel_format;
typedef enum st_mode { ST_MODE_LOST = 0, ST_MODE_LOST_IN_SPACE = 1, ST_MODE_TRACKING = 2 } st_mode;

typedef struct st_camera {
  uint32_t width, height;    /* pixels */
  double fx, fy, cx, cy;     /* pixels */
  double k1, k2, k3, p1, p2; /* Brown-Conrady distortion */
  int mirrored;              /* non-zero: x axis parity flip */
} st_camera;

typedef struct st_config {
  st_camera camera;
  uint32_t max_centroids;     /* per frame, brightest kept; 0 = 64 */
  uint32_t roi_half_size;     /* ROI tracking window half size; 0 = 8 */
  uint32_t max_rois;          /* 0 = 32 */
  int roi_tracking;           /* non-zero: centroid only predicted ROIs while tracking */
  float centroid_sigma;       /* detection threshold in sigma; 0 = 2 (tetra3 default) */
  uint32_t centroid_min_area; /* 0 = 5 */
  uint32_t centroid_max_area; /* 0 = 100 */
  double match_radius_px;     /* tracker search radius; 0 = 4 */
  uint32_t min_matches;       /* 0 = 5 */
  uint32_t rate_window;       /* 0 = 16 */
} st_config;

typedef struct st_centroid {
  double x, y; /* pixels */
} st_centroid;

typedef struct st_solution {
  st_status status; /* ST_OK when the attitude is valid for this frame */
  st_mode mode;
  int64_t t_ns;
  double q_cam_icrs[4]; /* w, x, y, z */
  double covariance[9]; /* rad^2, camera frame, row-major */
  double rate[3];       /* rad/s, camera frame */
  int rate_valid;
  uint32_t matches;
  double rmse_arcsec;
  double ra_deg, dec_deg, roll_deg;
  uint32_t num_centroids; /* centroids used this frame (st_track_image) */
  int used_rois;          /* st_track_image: ROI centroiding was used */
} st_solution;

typedef struct st_lis_options {
  double fov_estimate_deg;         /* <= 0: unknown */
  double fov_max_error_deg;        /* <= 0: none */
  uint32_t pattern_checking_stars; /* 0 = 8 */
  double match_radius;             /* fraction of width; 0 = 0.01 */
  double match_threshold;          /* 0 = 1e-3 */
  double distortion;               /* fixed k */
} st_lis_options;

typedef struct st_lis_result {
  st_status status;
  double ra_deg, dec_deg, roll_deg;
  double fov_deg;
  double rmse_arcsec, p90e_arcsec, maxe_arcsec;
  double probability;
  uint32_t num_matches;
  double q_cam_icrs[4];
  double solve_ms; /* wall time of the solve */
} st_lis_result;

typedef struct st_tracker st_tracker;

/* Library version string, e.g. "0.1.0". */
ST_API const char* st_version(void);
/* Static name of a status ("ST_NO_MATCH"). */
ST_API const char* st_status_string(st_status status);
/* Defaults (zeros mean defaults); camera must still be filled in. */
ST_API void st_config_init(st_config* config);
/* Ideal pinhole camera with horizontal field of view fov_deg. */
ST_API st_camera st_camera_ideal(uint32_t width, uint32_t height, double fov_deg);

/* Bytes of caller memory needed by st_tracker_create (0 when the database is invalid).
 * Opens and validates the database: O(database size). */
ST_API size_t st_tracker_memory_size(const st_config* config, const void* db, size_t db_size);
/* Creates a tracker in `memory` (64-byte aligned, >= st_tracker_memory_size bytes).
 * Errors: ST_INVALID_ARGUMENT, ST_CORRUPT_DATA / ST_CHECKSUM_MISMATCH / ST_VERSION_MISMATCH
 * (database), ST_ARENA_EXHAUSTED (memory too small). Release by freeing `memory` (no destroy
 * call is needed; nothing else is held). */
ST_API st_status st_tracker_create(const st_config* config, const void* db, size_t db_size,
                                   void* memory, size_t memory_size, st_tracker** out);

/* Full frame: centroid (ROIs while tracking) then track / lost-in-space. `stride_bytes` 0 =
 * packed. The image must not exceed the camera size. Returns out->status. Worst case: O(W*H)
 * centroiding plus one budgeted lost-in-space solve. */
ST_API st_status st_track_image(st_tracker* t, int64_t t_ns, const void* pixels, uint32_t width,
                                uint32_t height, size_t stride_bytes, st_pixel_format format,
                                st_solution* out);
/* Tracking step from caller centroids (brightest first). */
ST_API st_status st_track_centroids(st_tracker* t, int64_t t_ns, const st_centroid* centroids,
                                    uint32_t count, st_solution* out);
/* Centroids only (brightest first); writes up to `capacity`, sets *count. */
ST_API st_status st_extract_centroids(st_tracker* t, const void* pixels, uint32_t width,
                                      uint32_t height, size_t stride_bytes, st_pixel_format format,
                                      st_centroid* out, uint32_t capacity, uint32_t* count);
/* One tetra3-compatible lost-in-space solve; options may be NULL. Returns out->status. */
ST_API st_status st_solve_lost_in_space(st_tracker* t, const st_centroid* centroids, uint32_t count,
                                        uint32_t width, uint32_t height,
                                        const st_lis_options* options, st_lis_result* out);
/* Seeds the attitude (w, x, y, z) at t_ns; the next frame is tracked. */
ST_API void st_tracker_set_attitude(st_tracker* t, int64_t t_ns, const double q_cam_icrs[4]);
/* Forgets the attitude; the next frame runs lost-in-space. */
ST_API void st_tracker_reset(st_tracker* t);

#ifdef __cplusplus
}
#endif

#endif /* ST_C_ST_H */
