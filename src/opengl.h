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
 * Check if a word is in string.
 */
static inline bool
wd_is_in_str(const char *haystick, const char *needle) {
  if (!haystick)
    return false;

  assert(*needle);

  const char *pos = haystick - 1;
  while ((pos = strstr(pos + 1, needle))) {
    // Continue if it isn't a word boundary
    if (((pos - haystick) && !isspace(*(pos - 1)))
        || (strlen(pos) > strlen(needle) && !isspace(pos[strlen(needle)])))
      continue;
    return true;
  }

  return false;
}

/**
 * Check if a GLX extension exists.
 */
static inline bool
glx_hasglxext(session_t *ps, const char *ext) {
  const char *glx_exts = glXQueryExtensionsString(ps->dpy, ps->scr);
  if (!glx_exts) {
    printf_errf("(): Failed get GLX extension list.");
    return false;
  }

  bool found = wd_is_in_str(glx_exts, ext);
  if (!found)
    printf_errf("(): Missing GLX extension %s.", ext);

  return found;
}

/**
 * Check if a GLX extension exists.
 */
static inline bool
glx_hasglext(session_t *ps, const char *ext) {
  const char *gl_exts = (const char *) glGetString(GL_EXTENSIONS);
  if (!gl_exts) {
    printf_errf("(): Failed get GL extension list.");
    return false;
  }

  bool found = wd_is_in_str(gl_exts, ext);
  if (!found)
    printf_errf("(): Missing GL extension %s.", ext);

  return found;
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
