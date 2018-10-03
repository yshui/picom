// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE-mit for more information.
 *
 */

#pragma once

#include <stdbool.h>

typedef struct _c2_lptr c2_lptr_t;
typedef struct session session_t;
typedef struct win win;

c2_lptr_t *
c2_parsed(session_t *ps, c2_lptr_t **pcondlst, const char *pattern,
    void *data);

#define c2_parse(ps, pcondlst, pattern) c2_parsed((ps), (pcondlst), (pattern), NULL)

c2_lptr_t *
c2_free_lptr(c2_lptr_t *lp);

bool
c2_matchd(session_t *ps, win *w, const c2_lptr_t *condlst,
    const c2_lptr_t **cache, void **pdata);

#define c2_match(ps, w, condlst, cache) c2_matchd((ps), (w), (condlst), \
    (cache), NULL)
