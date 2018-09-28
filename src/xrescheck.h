#ifndef COMPTON_XRESCHECK_H
#define COMPTON_XRESCHECK_H

#include "common.h"
#include <uthash.h>

typedef struct {
  XID xid;
  const char *type;
  const char *file;
  const char *func;
  int line;
  UT_hash_handle hh;
} xrc_xid_record_t;

#define M_POS_DATA_PARAMS const char *file, int line, const char *func
#define M_POS_DATA_PASSTHROUGH file, line, func
#define M_POS_DATA __FILE__, __LINE__, __func__

void
xrc_add_xid_(XID xid, const char *type, M_POS_DATA_PARAMS);

#define xrc_add_xid(xid, type) xrc_add_xid_(xid, type, M_POS_DATA)

void
xrc_delete_xid_(XID xid, M_POS_DATA_PARAMS);

#define xrc_delete_xid(xid) xrc_delete_xid_(xid, M_POS_DATA)

void
xrc_report_xid(void);

void
xrc_clear_xid(void);

// Pixmap

static inline Pixmap
XCreatePixmap_(Display *dpy, Drawable drawable,
    unsigned int width, unsigned int height, unsigned int depth,
    M_POS_DATA_PARAMS) {
  Pixmap ret = XCreatePixmap(dpy, drawable, width, height, depth);
  if (ret)
    xrc_add_xid_(ret, "Pixmap", M_POS_DATA_PASSTHROUGH);
  return ret;
}

#define XCreatePixmap(dpy, drawable, width, height, depth) \
  XCreatePixmap_(dpy, drawable, width, height, depth, M_POS_DATA)

static inline xcb_pixmap_t
xcb_composite_name_window_pixmap_(xcb_connection_t *c, xcb_window_t window, xcb_pixmap_t pixmap, M_POS_DATA_PARAMS) {
  xcb_pixmap_t ret = xcb_composite_name_window_pixmap(c, window, pixmap);
  if (ret)
    xrc_add_xid_(ret, "PixmapC", M_POS_DATA_PASSTHROUGH);
  return ret;
}

#define xcb_composite_name_window_pixmap(dpy, window, pixmap) \
  xcb_composite_name_window_pixmap_(dpy, window, pixmap, M_POS_DATA)

static inline void
XFreePixmap_(Display *dpy, Pixmap pixmap, M_POS_DATA_PARAMS) {
  XFreePixmap(dpy, pixmap);
  xrc_delete_xid_(pixmap, M_POS_DATA_PASSTHROUGH);
}

#define XFreePixmap(dpy, pixmap) XFreePixmap_(dpy, pixmap, M_POS_DATA);

#endif
