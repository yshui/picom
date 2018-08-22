#include <stdlib.h>
#include <stdbool.h>

#include "common.h"
#include "config.h"
#include "c2.h"

/**
 * Parse a long number.
 */
bool
parse_long(const char *s, long *dest) {
  const char *endptr = NULL;
  long val = strtol(s, (char **) &endptr, 0);
  if (!endptr || endptr == s) {
    printf_errf("(\"%s\"): Invalid number.", s);
    return false;
  }
  while (isspace(*endptr))
    ++endptr;
  if (*endptr) {
    printf_errf("(\"%s\"): Trailing characters.", s);
    return false;
  }
  *dest = val;
  return true;
}

/**
 * Parse a floating-point number in matrix.
 */
const char *
parse_matrix_readnum(const char *src, double *dest) {
  char *pc = NULL;
  double val = strtod(src, &pc);
  if (!pc || pc == src) {
    printf_errf("(\"%s\"): No number found.", src);
    return src;
  }

  while (*pc && (isspace(*pc) || ',' == *pc))
    ++pc;

  *dest = val;

  return pc;
}

/**
 * Parse a matrix.
 */
XFixed *
parse_matrix(session_t *ps, const char *src, const char **endptr) {
  int wid = 0, hei = 0;
  const char *pc = NULL;
  XFixed *matrix = NULL;

  // Get matrix width and height
  {
    double val = 0.0;
    if (src == (pc = parse_matrix_readnum(src, &val)))
      goto parse_matrix_err;
    src = pc;
    wid = val;
    if (src == (pc = parse_matrix_readnum(src, &val)))
      goto parse_matrix_err;
    src = pc;
    hei = val;
  }

  // Validate matrix width and height
  if (wid <= 0 || hei <= 0) {
    printf_errf("(): Invalid matrix width/height.");
    goto parse_matrix_err;
  }
  if (!(wid % 2 && hei % 2)) {
    printf_errf("(): Width/height not odd.");
    goto parse_matrix_err;
  }
  if (wid > 16 || hei > 16)
    printf_errf("(): Matrix width/height too large, may slow down"
                "rendering, and/or consume lots of memory");

  // Allocate memory
  matrix = calloc(wid * hei + 2, sizeof(XFixed));
  if (!matrix) {
    printf_errf("(): Failed to allocate memory for matrix.");
    goto parse_matrix_err;
  }

  // Read elements
  {
    int skip = hei / 2 * wid + wid / 2;
    bool hasneg = false;
    for (int i = 0; i < wid * hei; ++i) {
      // Ignore the center element
      if (i == skip) {
        matrix[2 + i] = XDoubleToFixed(0);
        continue;
      }
      double val = 0;
      if (src == (pc = parse_matrix_readnum(src, &val)))
        goto parse_matrix_err;
      src = pc;
      if (val < 0) hasneg = true;
      matrix[2 + i] = XDoubleToFixed(val);
    }
    if (BKEND_XRENDER == ps->o.backend && hasneg)
      printf_errf("(): A convolution kernel with negative values "
          "may not work properly under X Render backend.");
  }

  // Detect trailing characters
  for ( ;*pc && ';' != *pc; ++pc)
    if (!isspace(*pc) && ',' != *pc) {
      printf_errf("(): Trailing characters in matrix string.");
      goto parse_matrix_err;
    }

  // Jump over spaces after ';'
  if (';' == *pc) {
    ++pc;
    while (*pc && isspace(*pc))
      ++pc;
  }

  // Require an end of string if endptr is not provided, otherwise
  // copy end pointer to endptr
  if (endptr)
    *endptr = pc;
  else if (*pc) {
    printf_errf("(): Only one matrix expected.");
    goto parse_matrix_err;
  }

  // Fill in width and height
  matrix[0] = XDoubleToFixed(wid);
  matrix[1] = XDoubleToFixed(hei);

  return matrix;

parse_matrix_err:
  free(matrix);
  return NULL;
}

/**
 * Parse a convolution kernel.
 */
XFixed *
parse_conv_kern(session_t *ps, const char *src, const char **endptr) {
  return parse_matrix(ps, src, endptr);
}

/**
 * Parse a list of convolution kernels.
 */
bool
parse_conv_kern_lst(session_t *ps, const char *src, XFixed **dest, int max) {
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
  for (unsigned int i = 0;
      i < sizeof(CONV_KERN_PREDEF) / sizeof(CONV_KERN_PREDEF[0]); ++i)
    if (!strcmp(CONV_KERN_PREDEF[i].name, src))
      return parse_conv_kern_lst(ps, CONV_KERN_PREDEF[i].kern_str, dest, max);

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
    if (!(dest[i++] = parse_conv_kern(ps, pc, &pc)))
      return false;
  }

  if (*pc) {
    printf_errf("(): Too many blur kernels!");
    return false;
  }

  return true;
}

/**
 * Parse a X geometry.
 */
bool
parse_geometry(session_t *ps, const char *src, geometry_t *dest) {
  geometry_t geom = { .wid = -1, .hei = -1, .x = -1, .y = -1 };
  long val = 0L;
  char *endptr = NULL;

#define T_STRIPSPACE() do { \
  while (*src && isspace(*src)) ++src; \
  if (!*src) goto parse_geometry_end; \
} while(0)

  T_STRIPSPACE();

  // Parse width
  // Must be base 10, because "0x0..." may appear
  if (!('+' == *src || '-' == *src)) {
    val = strtol(src, &endptr, 10);
    if (endptr && src != endptr) {
      geom.wid = val;
      assert(geom.wid >= 0);
      src = endptr;
    }
    T_STRIPSPACE();
  }

  // Parse height
  if ('x' == *src) {
    ++src;
    val = strtol(src, &endptr, 10);
    if (endptr && src != endptr) {
      geom.hei = val;
      if (geom.hei < 0) {
        printf_errf("(\"%s\"): Invalid height.", src);
        return false;
      }
      src = endptr;
    }
    T_STRIPSPACE();
  }

  // Parse x
  if ('+' == *src || '-' == *src) {
    val = strtol(src, &endptr, 10);
    if (endptr && src != endptr) {
      geom.x = val;
      if ('-' == *src && geom.x <= 0)
        geom.x -= 2;
      src = endptr;
    }
    T_STRIPSPACE();
  }

  // Parse y
  if ('+' == *src || '-' == *src) {
    val = strtol(src, &endptr, 10);
    if (endptr && src != endptr) {
      geom.y = val;
      if ('-' == *src && geom.y <= 0)
        geom.y -= 2;
      src = endptr;
    }
    T_STRIPSPACE();
  }

  if (*src) {
    printf_errf("(\"%s\"): Trailing characters.", src);
    return false;
  }

parse_geometry_end:
  *dest = geom;
  return true;
}

/**
 * Parse a list of opacity rules.
 */
bool parse_rule_opacity(session_t *ps, const char *src) {
  // Find opacity value
  char *endptr = NULL;
  long val = strtol(src, &endptr, 0);
  if (!endptr || endptr == src) {
    printf_errf("(\"%s\"): No opacity specified?", src);
    return false;
  }
  if (val > 100 || val < 0) {
    printf_errf("(\"%s\"): Opacity %ld invalid.", src, val);
    return false;
  }

  // Skip over spaces
  while (*endptr && isspace(*endptr))
    ++endptr;
  if (':' != *endptr) {
    printf_errf("(\"%s\"): Opacity terminator not found.", src);
    return false;
  }
  ++endptr;

  // Parse pattern
  // I hope 1-100 is acceptable for (void *)
  return c2_parsed(ps, &ps->o.opacity_rules, endptr, (void *) val);
}

/**
 * Add a pattern to a condition linked list.
 */
bool
condlst_add(session_t *ps, c2_lptr_t **pcondlst, const char *pattern) {
  if (!pattern)
    return false;

  if (!c2_parse(ps, pcondlst, pattern))
    exit(1);

  return true;
}
