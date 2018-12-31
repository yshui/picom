#pragma once
#include <xcb/xcb_image.h>
#include <xcb/render.h>

#include <stdbool.h>

#include "region.h"

typedef struct session session_t;
typedef struct win win;

bool build_shadow(session_t *ps, double opacity, const int width, const int height,
                  xcb_render_picture_t shadow_pixel, xcb_pixmap_t *pixmap,
                  xcb_render_picture_t *pict);

xcb_render_picture_t
solid_picture(session_t *ps, bool argb, double a, double r, double g, double b);

void paint_all_new(session_t *ps, region_t *region, win *const t);
