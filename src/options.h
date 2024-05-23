// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

/// Parse command line options

#include <stdbool.h>
#include <xcb/render.h>        // for xcb_render_fixed_t

#include <picom/types.h>

#include "compiler.h"
#include "config.h"

typedef struct session session_t;

/// Get config options that are needed to parse the rest of the options
/// Return true if we should quit
bool get_early_config(int argc, char *const *argv, char **config_file, bool *all_xerrors,
                      bool *fork, int *exit_code);

/**
 * Process arguments and configuration files.
 *
 * Parameters:
 *   shadow_enable    = Carry overs from parse_config
 *   fading_enable
 *   conv_kern_hasneg
 *   winopt_mask
 * Returns:
 *   Whether configuration are processed successfully.
 */
bool must_use get_cfg(options_t *opt, int argc, char *const *argv);
void options_postprocess_c2_lists(struct c2_state *state, struct x_connection *c,
                                  struct options *option);
void options_destroy(struct options *options);

// vim: set noet sw=8 ts=8:
