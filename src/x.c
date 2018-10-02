#include <stdbool.h>

#include <X11/Xlib.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/xfixes.h>
#include <pixman.h>

#include "common.h"
#include "x.h"

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
    long length, Atom rtype, int rformat) {
  Atom type = None;
  int format = 0;
  unsigned long nitems = 0, after = 0;
  unsigned char *data = NULL;

  if (Success == XGetWindowProperty(ps->dpy, w, atom, offset, length,
        False, rtype, &type, &format, &nitems, &after, &data)
      && nitems && (AnyPropertyType == type || type == rtype)
      && (!rformat || format == rformat)
      && (8 == format || 16 == format || 32 == format)) {
      return (winprop_t) {
        .data.p8 = data,
        .nitems = nitems,
        .type = type,
        .format = format,
      };
  }

  cxfree(data);

  return (winprop_t) {
    .data.p8 = NULL,
    .nitems = 0,
    .type = AnyPropertyType,
    .format = 0
  };
}

/**
 * Get the value of a type-<code>Window</code> property of a window.
 *
 * @return the value if successful, 0 otherwise
 */
Window
wid_get_prop_window(session_t *ps, Window wid, Atom aprop) {
  // Get the attribute
  Window p = None;
  winprop_t prop = wid_get_prop(ps, wid, aprop, 1L, XCB_ATOM_WINDOW, 32);

  // Return it
  if (prop.nitems) {
    p = *prop.data.p32;
  }

  free_winprop(&prop);

  return p;
}

/**
 * Get the value of a text property of a window.
 */
bool wid_get_text_prop(session_t *ps, Window wid, Atom prop,
    char ***pstrlst, int *pnstr) {
  XTextProperty text_prop = { NULL, None, 0, 0 };

  if (!(XGetTextProperty(ps->dpy, wid, &text_prop, prop) && text_prop.value))
    return false;

  if (Success !=
      XmbTextPropertyToTextList(ps->dpy, &text_prop, pstrlst, pnstr)
      || !*pnstr) {
    *pnstr = 0;
    if (*pstrlst)
      XFreeStringList(*pstrlst);
    cxfree(text_prop.value);
    return false;
  }

  cxfree(text_prop.value);
  return true;
}

static inline void x_get_server_pictfmts(session_t *ps) {
  if (ps->pictfmts)
    return;
  xcb_connection_t *c = XGetXCBConnection(ps->dpy);
  xcb_generic_error_t *e = NULL;
  // Get window picture format
  ps->pictfmts =
    xcb_render_query_pict_formats_reply(c,
        xcb_render_query_pict_formats(c), &e);
  if (e || !ps->pictfmts) {
    printf_errf("(): failed to get pict formats\n");
    abort();
  }
}

xcb_render_pictforminfo_t *x_get_pictform_for_visual(session_t *ps, xcb_visualid_t visual) {
  if (!ps->pictfmts)
    x_get_server_pictfmts(ps);

  xcb_render_pictvisual_t *pv = xcb_render_util_find_visual_format(ps->pictfmts, visual);
  for(xcb_render_pictforminfo_iterator_t i =
      xcb_render_query_pict_formats_formats_iterator(ps->pictfmts); i.rem;
      xcb_render_pictforminfo_next(&i)) {
    if (i.data->id == pv->format) {
      return i.data;
    }
  }
  return NULL;
}

xcb_render_picture_t
x_create_picture_with_pictfmt_and_pixmap(
  session_t *ps, xcb_render_pictforminfo_t * pictfmt,
  xcb_pixmap_t pixmap, unsigned long valuemask,
  const xcb_render_create_picture_value_list_t *attr)
{
  void *buf = NULL;
  if (attr) {
    xcb_render_create_picture_value_list_serialize(&buf, valuemask, attr);
    if (!buf) {
      printf_errf("(): failed to serialize picture attributes");
      return None;
    }
  }

  xcb_connection_t *c = XGetXCBConnection(ps->dpy);
  xcb_render_picture_t tmp_picture = xcb_generate_id(c);
  xcb_generic_error_t *e =
    xcb_request_check(c, xcb_render_create_picture_checked(c, tmp_picture,
      pixmap, pictfmt->id, valuemask, buf));
  free(buf);
  if (e) {
    printf_errf("(): failed to create picture");
    return None;
  }
  return tmp_picture;
}

xcb_render_picture_t
x_create_picture_with_visual_and_pixmap(
  session_t *ps, xcb_visualid_t visual,
  xcb_pixmap_t pixmap, unsigned long valuemask,
  const xcb_render_create_picture_value_list_t *attr)
{
  xcb_render_pictforminfo_t *pictfmt = x_get_pictform_for_visual(ps, visual);
  return x_create_picture_with_pictfmt_and_pixmap(ps, pictfmt, pixmap, valuemask, attr);
}

xcb_render_picture_t
x_create_picture_with_standard_and_pixmap(
  session_t *ps, xcb_pict_standard_t standard,
  xcb_pixmap_t pixmap, unsigned long valuemask,
  const xcb_render_create_picture_value_list_t *attr)
{
  if (!ps->pictfmts)
    x_get_server_pictfmts(ps);

  xcb_render_pictforminfo_t *pictfmt =
    xcb_render_util_find_standard_format(ps->pictfmts, standard);
  assert(pictfmt);
  return x_create_picture_with_pictfmt_and_pixmap(ps, pictfmt, pixmap, valuemask, attr);
}

/**
 * Create an picture.
 */
xcb_render_picture_t
x_create_picture(session_t *ps, int wid, int hei,
  xcb_render_pictforminfo_t *pictfmt, unsigned long valuemask,
  const xcb_render_create_picture_value_list_t *attr)
{
  if (!pictfmt)
    pictfmt = x_get_pictform_for_visual(ps, ps->vis);

  if (!pictfmt) {
    printf_errf("(): default visual is invalid");
    abort();
  }

  int depth = pictfmt->depth;

  xcb_pixmap_t tmp_pixmap = x_create_pixmap(ps, depth, ps->root, wid, hei);
  if (!tmp_pixmap)
    return None;

  xcb_render_picture_t picture =
    x_create_picture_with_pictfmt_and_pixmap(ps, pictfmt, tmp_pixmap, valuemask, attr);

  xcb_connection_t *c = XGetXCBConnection(ps->dpy);
  xcb_free_pixmap(c, tmp_pixmap);

  return picture;
}

bool x_fetch_region(session_t *ps, xcb_xfixes_region_t r, pixman_region32_t *res) {
  xcb_generic_error_t *e = NULL;
  xcb_connection_t *c = XGetXCBConnection(ps->dpy);
  xcb_xfixes_fetch_region_reply_t *xr = xcb_xfixes_fetch_region_reply(c,
    xcb_xfixes_fetch_region(c, r), &e);
  if (!xr) {
    printf_errf("(): failed to fetch rectangles");
    return false;
  }

  int nrect = xcb_xfixes_fetch_region_rectangles_length(xr);
  pixman_box32_t *b = calloc(nrect, sizeof *b);
  xcb_rectangle_t *xrect = xcb_xfixes_fetch_region_rectangles(xr);
  for (int i = 0; i < nrect; i++) {
    b[i] = (pixman_box32_t) {
      .x1 = xrect[i].x,
      .y1 = xrect[i].y,
      .x2 = xrect[i].x + xrect[i].width,
      .y2 = xrect[i].y + xrect[i].height
    };
  }
  bool ret = pixman_region32_init_rects(res, b, nrect);
  free(b);
  free(xr);
  return ret;
}

void x_set_picture_clip_region(session_t *ps, xcb_render_picture_t pict,
    int clip_x_origin, int clip_y_origin, const region_t *reg) {
  int nrects;
  const rect_t *rects = pixman_region32_rectangles((region_t *)reg, &nrects);
  xcb_rectangle_t *xrects = calloc(nrects, sizeof *xrects);
  for (int i = 0; i < nrects; i++)
    xrects[i] = (xcb_rectangle_t){
      .x = rects[i].x1,
      .y = rects[i].y1,
      .width = rects[i].x2 - rects[i].x1,
      .height = rects[i].y2 - rects[i].y1,
    };

  xcb_connection_t *c = XGetXCBConnection(ps->dpy);
  xcb_generic_error_t *e =
    xcb_request_check(c, xcb_render_set_picture_clip_rectangles_checked(c, pict,
      clip_x_origin, clip_y_origin, nrects, xrects));
  if (e)
    printf_errf("(): failed to set clip region");
  free(e);
  free(xrects);
  return;
}

/**
 * X11 error handler function.
 *
 * XXX consider making this error to string
 */
void
x_print_error(unsigned long serial, uint8_t major, uint8_t minor, uint8_t error_code) {
  session_t * const ps = ps_g;

  int o = 0;
  const char *name = "Unknown";

  if (major == ps->composite_opcode
      && minor == XCB_COMPOSITE_REDIRECT_SUBWINDOWS) {
    fprintf(stderr, "Another composite manager is already running "
        "(and does not handle _NET_WM_CM_Sn correctly)\n");
    exit(1);
  }

#define CASESTRRET2(s)   case s: name = #s; break

  o = error_code - ps->xfixes_error;
  switch (o) {
    CASESTRRET2(XCB_XFIXES_BAD_REGION);
  }

  o = error_code - ps->damage_error;
  switch (o) {
    CASESTRRET2(XCB_DAMAGE_BAD_DAMAGE);
  }

  o = error_code - ps->render_error;
  switch (o) {
    CASESTRRET2(XCB_RENDER_PICT_FORMAT);
    CASESTRRET2(XCB_RENDER_PICTURE);
    CASESTRRET2(XCB_RENDER_PICT_OP);
    CASESTRRET2(XCB_RENDER_GLYPH_SET);
    CASESTRRET2(XCB_RENDER_GLYPH);
  }

#ifdef CONFIG_OPENGL
  if (ps->glx_exists) {
    o = error_code - ps->glx_error;
    switch (o) {
      CASESTRRET2(GLX_BAD_SCREEN);
      CASESTRRET2(GLX_BAD_ATTRIBUTE);
      CASESTRRET2(GLX_NO_EXTENSION);
      CASESTRRET2(GLX_BAD_VISUAL);
      CASESTRRET2(GLX_BAD_CONTEXT);
      CASESTRRET2(GLX_BAD_VALUE);
      CASESTRRET2(GLX_BAD_ENUM);
    }
  }
#endif

#ifdef CONFIG_XSYNC
  if (ps->xsync_exists) {
    o = error_code - ps->xsync_error;
    switch (o) {
      CASESTRRET2(XSyncBadCounter);
      CASESTRRET2(XSyncBadAlarm);
      CASESTRRET2(XSyncBadFence);
    }
  }
#endif

  switch (error_code) {
    CASESTRRET2(BadAccess);
    CASESTRRET2(BadAlloc);
    CASESTRRET2(BadAtom);
    CASESTRRET2(BadColor);
    CASESTRRET2(BadCursor);
    CASESTRRET2(BadDrawable);
    CASESTRRET2(BadFont);
    CASESTRRET2(BadGC);
    CASESTRRET2(BadIDChoice);
    CASESTRRET2(BadImplementation);
    CASESTRRET2(BadLength);
    CASESTRRET2(BadMatch);
    CASESTRRET2(BadName);
    CASESTRRET2(BadPixmap);
    CASESTRRET2(BadRequest);
    CASESTRRET2(BadValue);
    CASESTRRET2(BadWindow);
  }

#undef CASESTRRET2

  print_timestamp(ps);
  {
    char buf[BUF_LEN] = "";
    XGetErrorText(ps->dpy, error_code, buf, BUF_LEN);
    printf("error %4d %-12s request %4d minor %4d serial %6lu: \"%s\"\n",
        error_code, name, major,
        minor, serial, buf);
  }

  // print_backtrace();
}

/**
 * Create a pixmap and check that creation succeeded.
 */
xcb_pixmap_t
x_create_pixmap(session_t *ps, uint8_t depth, xcb_drawable_t drawable, uint16_t width, uint16_t height) {
  xcb_connection_t *c = XGetXCBConnection(ps->dpy);
  xcb_pixmap_t pix = xcb_generate_id(c);
  xcb_void_cookie_t cookie = xcb_create_pixmap_checked(c, depth, pix, drawable, width, height);
  xcb_generic_error_t *err = xcb_request_check(c, cookie);
  if (err == NULL)
    return pix;

  printf_err("Failed to create pixmap:");
  ev_xcb_error(ps, err);
  free(err);
  return XCB_NONE;
}
