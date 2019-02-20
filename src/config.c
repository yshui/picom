// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>

#include <math.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <xcb/render.h> // for xcb_render_fixed_t, XXX

#include "compiler.h"
#include "common.h"
#include "utils.h"
#include "c2.h"
#include "string_utils.h"
#include "log.h"
#include "region.h"
#include "types.h"
#include "kernel.h"
#include "win.h"

#include "config.h"

/**
 * Parse a long number.
 */
bool
parse_long(const char *s, long *dest) {
  const char *endptr = NULL;
  long val = strtol(s, (char **) &endptr, 0);
  if (!endptr || endptr == s) {
    log_error("Invalid number: %s", s);
    return false;
  }
  while (isspace(*endptr))
    ++endptr;
  if (*endptr) {
    log_error("Trailing characters: %s", s);
    return false;
  }
  *dest = val;
  return true;
}

/**
 * Parse a floating-point number in from a string,
 * also strips the trailing space and comma after the number.
 *
 * @param[in]  src  string to parse
 * @param[out] dest return the number parsed from the string
 * @return          pointer to the last character parsed
 */
const char *
parse_readnum(const char *src, double *dest) {
  const char *pc = NULL;
  double val = strtod_simple(src, &pc);
  if (!pc || pc == src) {
    log_error("No number found: %s", src);
    return src;
  }
  while (*pc && (isspace(*pc) || *pc == ',')) {
    ++pc;
  }
  *dest = val;
  return pc;
}

/**
 * Parse a matrix.
 *
 * @param[in]  src    the blur kernel string
 * @param[out] endptr return where the end of kernel is in the string
 * @param[out] hasneg whether the kernel has negative values
 */
conv *
parse_blur_kern(const char *src, const char **endptr, bool *hasneg) {
  int width = 0, height = 0;
  *hasneg = false;

  const char *pc = NULL;

  // Get matrix width and height
  double val = 0.0;
  if (src == (pc = parse_readnum(src, &val)))
    goto err1;
  src = pc;
  width = val;
  if (src == (pc = parse_readnum(src, &val)))
    goto err1;
  src = pc;
  height = val;

  // Validate matrix width and height
  if (width <= 0 || height <= 0) {
    log_error("Blue kernel width/height can't be negative.");
    goto err1;
  }
  if (!(width % 2 && height % 2)) {
    log_error("Blur kernel idth/height must be odd.");
    goto err1;
  }
  if (width > 16 || height > 16)
    log_warn("Blur kernel width/height too large, may slow down"
             "rendering, and/or consume lots of memory");

  // Allocate memory
  conv *matrix = cvalloc(sizeof(conv) + width * height * sizeof(double));

  // Read elements
  int skip = height / 2 * width + width / 2;
  for (int i = 0; i < width * height; ++i) {
    // Ignore the center element
    if (i == skip) {
      matrix->data[i] = 0;
      continue;
    }
    if (src == (pc = parse_readnum(src, &val))) {
      goto err2;
    }
    src = pc;
    if (val < 0) {
      *hasneg = true;
    }
    matrix->data[i] = val;
  }

  // Detect trailing characters
  for (;*pc && *pc != ';'; pc++) {
    if (!isspace(*pc) && *pc != ',') {
      // TODO isspace is locale aware, be careful
      log_error("Trailing characters in blur kernel string.");
      goto err2;
    }
  }

  // Jump over spaces after ';'
  if (*pc == ';') {
    pc++;
    while (*pc && isspace(*pc)) {
      ++pc;
    }
  }

  // Require an end of string if endptr is not provided, otherwise
  // copy end pointer to endptr
  if (endptr) {
    *endptr = pc;
  } else if (*pc) {
    log_error("Only one blur kernel expected.");
    goto err2;
  }

  // Fill in width and height
  matrix->w = width;
  matrix->h = height;
  return matrix;

err2:
  free(matrix);
err1:
  return NULL;
}

/**
 * Parse a list of convolution kernels.
 *
 * @param[in]  src    string to parse
 * @param[out] dest   pointer to an array of kernels, must points to an array
 *                    of `max` elements.
 * @param[in]  max    maximum number of kernels supported
 * @param[out] hasneg whether any of the kernels have negative values
 * @return            if the `src` string is a valid kernel list string
 */
bool
parse_blur_kern_lst(const char *src, conv **dest, int max, bool *hasneg) {
  // TODO just return a predefined kernels, not parse predefined strings...
  static const struct {
    const char *name;
    const char *kern_str;
  } CONV_KERN_PREDEF[] = {
    { "3x3box", "3,3,1,1,1,1,1,1,1,1," },
    { "5x5box", "5,5,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1," },
    { "7x7box", "7,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1," },
    { "3x3gaussian", "3,3,0.243117,0.493069,0.243117,0.493069,0.493069,0.243117,0.493069,0.243117," },
    { "5x5gaussian", "5,5,0.003493,0.029143,0.059106,0.029143,0.003493,0.029143,0.243117,0.493069,0.243117,0.029143,0.059106,0.493069,0.493069,0.059106,0.029143,0.243117,0.493069,0.243117,0.029143,0.003493,0.029143,0.059106,0.029143,0.003493," },
    { "7x7gaussian", "7,7,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0.000003,0.000102,0.003493,0.029143,0.059106,0.029143,0.003493,0.000102,0.000849,0.029143,0.243117,0.493069,0.243117,0.029143,0.000849,0.001723,0.059106,0.493069,0.493069,0.059106,0.001723,0.000849,0.029143,0.243117,0.493069,0.243117,0.029143,0.000849,0.000102,0.003493,0.029143,0.059106,0.029143,0.003493,0.000102,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0.000003," },
    { "9x9gaussian", "9,9,0.000000,0.000000,0.000001,0.000006,0.000012,0.000006,0.000001,0.000000,0.000000,0.000000,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0.000003,0.000000,0.000001,0.000102,0.003493,0.029143,0.059106,0.029143,0.003493,0.000102,0.000001,0.000006,0.000849,0.029143,0.243117,0.493069,0.243117,0.029143,0.000849,0.000006,0.000012,0.001723,0.059106,0.493069,0.493069,0.059106,0.001723,0.000012,0.000006,0.000849,0.029143,0.243117,0.493069,0.243117,0.029143,0.000849,0.000006,0.000001,0.000102,0.003493,0.029143,0.059106,0.029143,0.003493,0.000102,0.000001,0.000000,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0.000003,0.000000,0.000000,0.000000,0.000001,0.000006,0.000012,0.000006,0.000001,0.000000,0.000000," },
    { "11x11gaussian", "11,11,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000001,0.000006,0.000012,0.000006,0.000001,0.000000,0.000000,0.000000,0.000000,0.000000,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0.000003,0.000000,0.000000,0.000000,0.000001,0.000102,0.003493,0.029143,0.059106,0.029143,0.003493,0.000102,0.000001,0.000000,0.000000,0.000006,0.000849,0.029143,0.243117,0.493069,0.243117,0.029143,0.000849,0.000006,0.000000,0.000000,0.000012,0.001723,0.059106,0.493069,0.493069,0.059106,0.001723,0.000012,0.000000,0.000000,0.000006,0.000849,0.029143,0.243117,0.493069,0.243117,0.029143,0.000849,0.000006,0.000000,0.000000,0.000001,0.000102,0.003493,0.029143,0.059106,0.029143,0.003493,0.000102,0.000001,0.000000,0.000000,0.000000,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0.000003,0.000000,0.000000,0.000000,0.000000,0.000000,0.000001,0.000006,0.000012,0.000006,0.000001,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000," },
  };

  *hasneg = false;
  for (unsigned int i = 0;
       i < sizeof(CONV_KERN_PREDEF) / sizeof(CONV_KERN_PREDEF[0]); ++i) {
    if (!strcmp(CONV_KERN_PREDEF[i].name, src))
      return parse_blur_kern_lst(CONV_KERN_PREDEF[i].kern_str, dest, max, hasneg);
  }

  int i = 0;
  const char *pc = src;

  // Free old kernels
  for (i = 0; i < max; ++i) {
    free(dest[i]);
    dest[i] = NULL;
  }

  // Continue parsing until the end of source string
  i = 0;
  while (pc && *pc && i < max - 1) {
    bool tmp_hasneg;
    dest[i] = parse_blur_kern(pc, &pc, &tmp_hasneg);
    if (!dest[i]) {
      return false;
    }
    i++;
    *hasneg |= tmp_hasneg;
  }

  if (i > 1) {
    log_warn("You are seeing this message because your are using multipassblur. Please "
             "report an issue to us so we know multipass blur is actually been used. "
             "Otherwise it might be removed in future releases");
  }

  if (*pc) {
    log_error("Too many blur kernels!");
    return false;
  }

  return true;
}

/**
 * Parse a X geometry.
 *
 * ps->root_width and ps->root_height must be valid
 */
bool
parse_geometry(session_t *ps, const char *src, region_t *dest) {
  pixman_region32_clear(dest);
  if (!src)
    return true;
  if (!ps->root_width || !ps->root_height)
    return true;

  geometry_t geom = { .wid = ps->root_width, .hei = ps->root_height, .x = 0, .y = 0 };
  long val = 0L;
  char *endptr = NULL;

  src = skip_space(src);
  if (!*src)
    goto parse_geometry_end;

  // Parse width
  // Must be base 10, because "0x0..." may appear
  if (!('+' == *src || '-' == *src)) {
    val = strtol(src, &endptr, 10);
    assert(endptr);
    if (src != endptr) {
      geom.wid = val;
      if (geom.wid < 0) {
        log_error("Invalid width: %s", src);
        return false;
      }
      src = endptr;
    }
    src = skip_space(src);
  }

  // Parse height
  if ('x' == *src) {
    ++src;
    val = strtol(src, &endptr, 10);
    assert(endptr);
    if (src != endptr) {
      geom.hei = val;
      if (geom.hei < 0) {
        log_error("Invalid height: %s", src);
        return false;
      }
      src = endptr;
    }
    src = skip_space(src);
  }

  // Parse x
  if ('+' == *src || '-' == *src) {
    val = strtol(src, &endptr, 10);
    if (endptr && src != endptr) {
      geom.x = val;
      if (*src == '-')
        geom.x += ps->root_width - geom.wid;
      src = endptr;
    }
    src = skip_space(src);
  }

  // Parse y
  if ('+' == *src || '-' == *src) {
    val = strtol(src, &endptr, 10);
    if (endptr && src != endptr) {
      geom.y = val;
      if (*src == '-')
        geom.y += ps->root_height - geom.hei;
      src = endptr;
    }
    src = skip_space(src);
  }

  if (*src) {
    log_error("Trailing characters: %s", src);
    return false;
  }

parse_geometry_end:
  pixman_region32_union_rect(dest, dest, geom.x, geom.y, geom.wid, geom.hei);
  return true;
}

/**
 * Parse a list of opacity rules.
 */
bool parse_rule_opacity(c2_lptr_t **res, const char *src) {
  // Find opacity value
  char *endptr = NULL;
  long val = strtol(src, &endptr, 0);
  if (!endptr || endptr == src) {
    log_error("No opacity specified: %s", src);
    return false;
  }
  if (val > 100 || val < 0) {
    log_error("Opacity %ld invalid: %s", val, src);
    return false;
  }

  // Skip over spaces
  while (*endptr && isspace(*endptr))
    ++endptr;
  if (':' != *endptr) {
    log_error("Opacity terminator not found: %s", src);
    return false;
  }
  ++endptr;

  // Parse pattern
  // I hope 1-100 is acceptable for (void *)
  return c2_parse(res, endptr, (void *) val);
}

/**
 * Add a pattern to a condition linked list.
 */
bool
condlst_add(c2_lptr_t **pcondlst, const char *pattern) {
  if (!pattern)
    return false;

  if (!c2_parse(pcondlst, pattern, NULL))
    exit(1);

  return true;
}

void set_default_winopts(options_t *opt, win_option_mask_t *mask, bool shadow_enable, bool fading_enable) {
  // Apply default wintype options.
  if (!mask[WINTYPE_DESKTOP].shadow) {
    // Desktop windows are always drawn without shadow by default.
    mask[WINTYPE_DESKTOP].shadow = true;
    opt->wintype_option[WINTYPE_DESKTOP].shadow = false;
  }

  // Focused/unfocused state only apply to a few window types, all other windows
  // are always considered focused.
  const wintype_t nofocus_type[] =
    { WINTYPE_UNKNOWN, WINTYPE_NORMAL, WINTYPE_UTILITY };
  for (unsigned long i = 0; i < ARR_SIZE(nofocus_type); i++) {
    if (!mask[nofocus_type[i]].focus) {
      mask[nofocus_type[i]].focus = true;
      opt->wintype_option[nofocus_type[i]].focus = false;
    }
  }
  for (unsigned long i = 0; i < NUM_WINTYPES; i++) {
    if (!mask[i].shadow) {
      mask[i].shadow = true;
      opt->wintype_option[i].shadow = shadow_enable;
    }
    if (!mask[i].fade) {
      mask[i].fade = true;
      opt->wintype_option[i].fade = fading_enable;
    }
    if (!mask[i].focus) {
      mask[i].focus = true;
      opt->wintype_option[i].focus = true;
    }
    if (!mask[i].full_shadow) {
      mask[i].full_shadow = true;
      opt->wintype_option[i].full_shadow = false;
    }
    if (!mask[i].redir_ignore) {
      mask[i].redir_ignore = true;
      opt->wintype_option[i].redir_ignore = false;
    }
    if (!mask[i].opacity) {
      mask[i].opacity = true;
      // Opacity is not set to a concrete number here because the opacity logic
      // is complicated, and needs an "unset" state
      opt->wintype_option[i].opacity = NAN;
    }
  }
}

char *parse_config(options_t *opt, const char *config_file,
                   bool *shadow_enable, bool *fading_enable, bool *hasneg,
                   win_option_mask_t *winopt_mask) {
  char *ret = NULL;
#ifdef CONFIG_LIBCONFIG
  ret = parse_config_libconfig(opt, config_file, shadow_enable, fading_enable,
                               hasneg, winopt_mask);
#endif
  return ret;
}
