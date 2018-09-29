#pragma once
#include <pixman.h>
#include <stdio.h>
#include "utils.h"
typedef struct pixman_region32 pixman_region32_t;
typedef struct pixman_box32 pixman_box32_t;
typedef pixman_region32_t region_t;
typedef pixman_box32_t rect_t;

RC_TYPE(region_t, rc_region, pixman_region32_init, pixman_region32_fini, static inline)

/// copy a region_t
static inline void
copy_region_(region_t *dst, const region_t *p) {
  pixman_region32_copy(dst, (region_t *)p);
}

static inline void
dump_region_(const region_t *x) {
  int nrects;
  const rect_t *rects = pixman_region32_rectangles((region_t *)x, &nrects);
  fprintf(stderr, "nrects: %d\n", nrects);
  for (int i = 0; i < nrects; i++)
    fprintf(stderr, "(%d, %d) - (%d, %d)\n", rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2);
}
