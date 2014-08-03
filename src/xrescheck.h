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

static inline Pixmap
XCompositeNameWindowPixmap_(Display *dpy, Window window, M_POS_DATA_PARAMS) {
  Pixmap ret = XCompositeNameWindowPixmap(dpy, window);
  if (ret)
    xrc_add_xid_(ret, "PixmapC", M_POS_DATA_PASSTHROUGH);
  return ret;
}

#define XCompositeNameWindowPixmap(dpy, window) \
  XCompositeNameWindowPixmap_(dpy, window, M_POS_DATA)

static inline void
XFreePixmap_(Display *dpy, Pixmap pixmap, M_POS_DATA_PARAMS) {
  XFreePixmap(dpy, pixmap);
  xrc_delete_xid_(pixmap, M_POS_DATA_PASSTHROUGH);
}

#define XFreePixmap(dpy, pixmap) XFreePixmap_(dpy, pixmap, M_POS_DATA);

#endif
