#pragma once
#include <xcb/xcb_image.h>
#include "common.h"

bool build_shadow(session_t *ps, double opacity, const int width, const int height,
                  xcb_render_picture_t shadow_pixel, xcb_pixmap_t *pixmap,
                  xcb_render_picture_t *pict);

xcb_render_picture_t
solid_picture(session_t *ps, bool argb, double a, double r, double g, double b);
