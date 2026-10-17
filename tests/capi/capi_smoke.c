/* SPDX-License-Identifier: Apache-2.0 */
/* C99 smoke test of st.h: the header compiles as C, and a real image solves.
 *   st_capi_smoke <db.stdb> <image.pgm> <fov_deg>      (exit 77 = fixtures missing) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st/c/st.h"

static void* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  void* p = aligned_alloc(64, ((size_t)n + 63) / 64 * 64);
  if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
    free(p);
    p = NULL;
  }
  fclose(f);
  *size = (size_t)n;
  return p;
}

int main(int argc, char** argv) {
  if (argc < 4) return 2;
  size_t db_size = 0, img_size = 0;
  void* db = read_file(argv[1], &db_size);
  unsigned char* pgm = (unsigned char*)read_file(argv[2], &img_size);
  if (!db || !pgm) {
    fprintf(stderr, "fixtures missing; skipping\n");
    return 77;
  }
  unsigned w = 0, h = 0, maxval = 0;
  int off = 0;
  if (sscanf((const char*)pgm, "P5 %u %u %u%n", &w, &h, &maxval, &off) != 3) return 1;
  const unsigned char* pixels = pgm + off + 1;
  /* 16-bit PGM is big-endian; swap in place to host order. */
  if (maxval > 255) {
    unsigned char* p = (unsigned char*)pixels;
    for (size_t i = 0; i < (size_t)w * h; ++i) {
      unsigned char t = p[2 * i];
      p[2 * i] = p[2 * i + 1];
      p[2 * i + 1] = t;
    }
  }

  printf("startracker %s\n", st_version());
  st_config cfg;
  st_config_init(&cfg);
  cfg.camera = st_camera_ideal(w, h, atof(argv[3]));
  size_t need = st_tracker_memory_size(&cfg, db, db_size);
  if (need == 0) return 1;
  void* mem = aligned_alloc(64, (need + 63) / 64 * 64);
  st_tracker* t = NULL;
  st_status s = st_tracker_create(&cfg, db, db_size, mem, need, &t);
  if (s != ST_OK) {
    printf("create: %s\n", st_status_string(s));
    return 1;
  }
  /* Too little memory is reported, not overrun. */
  st_tracker* t2 = NULL;
  if (st_tracker_create(&cfg, db, db_size, mem, need / 2, &t2) != ST_ARENA_EXHAUSTED) return 1;
  s = st_tracker_create(&cfg, db, db_size, mem, need, &t);
  if (s != ST_OK) return 1;

  st_centroid c[64];
  uint32_t n = 0;
  st_pixel_format fmt = maxval > 255 ? ST_MONO16 : ST_MONO8;
  if (st_extract_centroids(t, pixels, w, h, 0, fmt, c, 64, &n) != ST_OK || n < 4) return 1;
  st_lis_result r;
  s = st_solve_lost_in_space(t, c, n, w, h, NULL, &r);
  printf("lis: %s ra %.4f dec %.4f roll %.4f fov %.4f matches %u (%.2f ms)\n", st_status_string(s),
         r.ra_deg, r.dec_deg, r.roll_deg, r.fov_deg, r.num_matches, r.solve_ms);
  if (s != ST_OK) return 1;
  st_solution sol;
  s = st_track_image(t, 0, pixels, w, h, 0, fmt, &sol);
  printf("track: %s mode %d ra %.4f dec %.4f roll %.4f matches %u\n", st_status_string(s),
         (int)sol.mode, sol.ra_deg, sol.dec_deg, sol.roll_deg, sol.matches);
  if (s != ST_OK || sol.mode != ST_MODE_LOST_IN_SPACE) return 1;
  s = st_track_image(t, 100000000, pixels, w, h, 0, fmt, &sol);
  printf("track: %s mode %d rois %d matches %u\n", st_status_string(s), (int)sol.mode,
         sol.used_rois, sol.matches);
  if (s != ST_OK || sol.mode != ST_MODE_TRACKING) return 1;
  free(mem);
  free(db);
  free(pgm);
  return 0;
}
