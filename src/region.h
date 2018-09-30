#pragma once
#include <xcb/xcb.h>
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
copy_region(region_t *dst, const region_t *p) {
  pixman_region32_copy(dst, (region_t *)p);
}

static inline void
dump_region(const region_t *x) {
  int nrects;
  const rect_t *rects = pixman_region32_rectangles((region_t *)x, &nrects);
  fprintf(stderr, "nrects: %d\n", nrects);
  for (int i = 0; i < nrects; i++)
    fprintf(stderr, "(%d, %d) - (%d, %d)\n", rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2);
}

/// Convert one xcb rectangle to our rectangle type
static inline rect_t
from_x_rect(const xcb_rectangle_t *rect) {
  return (rect_t) {
    .x1 = rect->x,
    .y1 = rect->y,
    .x2 = rect->x + rect->width,
    .y2 = rect->y + rect->height,
  };
}

/// Convert an array of xcb rectangles to our rectangle type
/// Returning an array that needs to be freed
static inline rect_t *
from_x_rects(int nrects, const xcb_rectangle_t *rects) {
  rect_t *ret = calloc(nrects, sizeof *ret);
  for (int i = 0; i < nrects; i++)
    ret[i] = from_x_rect(rects+i);
  return ret;
}
