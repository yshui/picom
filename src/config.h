#pragma once

#include <stdbool.h>

#include <libconfig.h>

#include "common.h"

bool parse_long(const char *, long *);
const char *parse_matrix_readnum(const char *, double *);
XFixed *parse_matrix(session_t *, const char *, const char **);
XFixed *parse_conv_kern(session_t *, const char *, const char **);
bool parse_conv_kern_lst(session_t *, const char *, XFixed **, int);
bool parse_geometry(session_t *, const char *, geometry_t *);
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
parse_config(session_t *ps, struct options_tmp *pcfgtmp);
#endif
