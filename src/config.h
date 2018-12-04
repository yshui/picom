#pragma once
// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>

#include <stdbool.h>

#ifdef CONFIG_LIBCONFIG
#include <libconfig.h>
#endif

#include "common.h"

bool parse_long(const char *, long *);
const char *parse_matrix_readnum(const char *, double *);
xcb_render_fixed_t *parse_matrix(session_t *, const char *, const char **);
xcb_render_fixed_t *parse_conv_kern(session_t *, const char *, const char **);
bool parse_conv_kern_lst(session_t *, const char *, xcb_render_fixed_t **, int);
bool parse_geometry(session_t *, const char *, region_t *);
bool parse_rule_opacity(session_t *, const char *);

/**
 * Add a pattern to a condition linked list.
 */
bool condlst_add(session_t *, c2_lptr_t **, const char *);

#ifdef CONFIG_LIBCONFIG
FILE *
open_config_file(char *cpath, char **path);

void
parse_cfg_condlst(session_t *ps, const config_t *pcfg, c2_lptr_t **pcondlst,
    const char *name);

void
parse_config(session_t *ps, bool *shadow_enable,
  bool *fading_enable, win_option_mask_t *winopt_mask);
#else
static inline void parse_config() {}
#endif
