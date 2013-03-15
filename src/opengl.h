/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE for more information.
 *
 */

#include "common.h"

#include <ctype.h>

/**
 * Check if a GLX extension exists.
 */
static inline bool
glx_hasext(session_t *ps, const char *ext) {
  const char *glx_exts = glXQueryExtensionsString(ps->dpy, ps->scr);
  const char *pos = strstr(glx_exts, ext);
  // Make sure the extension string is matched as a whole word
  if (!pos
      || ((pos - glx_exts) && !isspace(*(pos - 1)))
      || (strlen(pos) > strlen(ext) && !isspace(pos[strlen(ext)]))) {
    printf_errf("(): Missing OpenGL extension %s.", ext);
    return false;
  }

  return true;
}

static inline XVisualInfo *
get_visualinfo_from_visual(session_t *ps, Visual *visual) {
  XVisualInfo vreq = { .visualid = XVisualIDFromVisual(visual) };
  int nitems = 0;

  return XGetVisualInfo(ps->dpy, VisualIDMask, &vreq, &nitems);
}

static bool
glx_update_fbconfig(session_t *ps);

static int
glx_cmp_fbconfig(session_t *ps,
    const glx_fbconfig_t *pfbc_a, const glx_fbconfig_t *pfbc_b);
