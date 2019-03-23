// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

/// Parse command line options

#include <stdbool.h>
#include <xcb/render.h>        // for xcb_render_fixed_t

#include "compiler.h"
#include "config.h"
#include "types.h"
#include "win.h"        // for wintype_t

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
 */
void get_cfg(options_t *opt, int argc, char *const *argv, bool shadow_enable,
             bool fading_enable, bool conv_kern_hasneg, win_option_mask_t *winopt_mask);

// vim: set noet sw=8 ts=8:
