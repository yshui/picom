#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <xcb/render.h>
#include <xcb/xcb_renderutil.h>

#include "region.h"

typedef struct session session_t;
typedef struct winprop winprop_t;

#define XCB_SYNCED_VOID(func, c, ...) xcb_request_check(c, func##_checked(c, __VA_ARGS__));
#define XCB_SYNCED(func, c, ...) ({ \
  xcb_generic_error_t *e = NULL; \
  __auto_type r = func##_reply(c, func(c, __VA_ARGS__), &e); \
  if (e) { \
    x_print_error(e->sequence, e->major_code, e->minor_code, e->error_code); \
    free(e); \
  } \
  r; \
})

/**
 * Get a specific attribute of a window.
 *
 * Returns a blank structure if the returned type and format does not
 * match the requested type and format.
 *
 * @param ps current session
 * @param w window
 * @param atom atom of attribute to fetch
 * @param length length to read
 * @param rtype atom of the requested type
 * @param rformat requested format
 * @return a <code>winprop_t</code> structure containing the attribute
 *    and number of items. A blank one on failure.
 */
winprop_t
wid_get_prop_adv(const session_t *ps, Window w, Atom atom, long offset,
    long length, Atom rtype, int rformat);

/**
 * Get the value of a type-<code>Window</code> property of a window.
 *
 * @return the value if successful, 0 otherwise
 */
Window
wid_get_prop_window(session_t *ps, Window wid, Atom aprop);

/**
 * Get the value of a text property of a window.
 */
bool wid_get_text_prop(session_t *ps, Window wid, Atom prop,
    char ***pstrlst, int *pnstr);

xcb_render_pictforminfo_t *x_get_pictform_for_visual(session_t *, xcb_visualid_t);

xcb_render_picture_t x_create_picture_with_pictfmt_and_pixmap(
  session_t *ps, xcb_render_pictforminfo_t *pictfmt,
  xcb_pixmap_t pixmap, unsigned long valuemask,
  const xcb_render_create_picture_value_list_t *attr)
__attribute__((nonnull(1, 2)));

xcb_render_picture_t x_create_picture_with_visual_and_pixmap(
  session_t *ps, xcb_visualid_t visual,
  xcb_pixmap_t pixmap, unsigned long valuemask,
  const xcb_render_create_picture_value_list_t *attr)
__attribute__((nonnull(1)));

xcb_render_picture_t x_create_picture_with_standard_and_pixmap(
  session_t *ps, xcb_pict_standard_t standard,
  xcb_pixmap_t pixmap, unsigned long valuemask,
  const xcb_render_create_picture_value_list_t *attr)
__attribute__((nonnull(1)));

/**
 * Create an picture.
 */
xcb_render_picture_t
x_create_picture(session_t *ps, int wid, int hei,
  xcb_render_pictforminfo_t *pictfmt, unsigned long valuemask,
  const xcb_render_create_picture_value_list_t *attr);

/// Fetch a X region and store it in a pixman region
bool x_fetch_region(session_t *ps, XserverRegion r, region_t *res);

void x_set_picture_clip_region(session_t *ps, xcb_render_picture_t,
  int clip_x_origin, int clip_y_origin, const region_t *);

/**
 * X11 error handler function.
 *
 * XXX consider making this error to string
 */
void
x_print_error(unsigned long serial, uint8_t major, uint8_t minor, uint8_t error_code);
