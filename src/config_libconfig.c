// SPDX-License-Identifier: MIT
// Copyright (c) 2012-2014 Richard Grenville <pyxlcy@gmail.com>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <libgen.h>
#include <libconfig.h>
#include <basedir_fs.h>

#include "common.h"
#include "config.h"
#include "string_utils.h"
#include "log.h"

/**
 * Wrapper of libconfig's <code>config_lookup_int</code>.
 *
 * So it takes a pointer to bool.
 */
static inline int
lcfg_lookup_bool(const config_t *config, const char *path, bool *value) {
  int ival;

  int ret = config_lookup_bool(config, path, &ival);
  if (ret)
    *value = ival;

  return ret;
}

/**
 * Get a file stream of the configuration file to read.
 *
 * Follows the XDG specification to search for the configuration file.
 */
FILE *
open_config_file(char *cpath, char **ppath) {
  static const char *config_paths[] = {
    "/compton.conf",
    "/compton/compton.conf"
  };
  static const char config_filename_legacy[] = "/.compton.conf";

  if (cpath) {
    FILE *ret = fopen(cpath, "r");
    if (ret && ppath)
      *ppath = cpath;
    return ret;
  }

  for (size_t i = 0; i < ARR_SIZE(config_paths); i++) {
    char *path = xdgConfigFind(config_paths[i], NULL);
    FILE *ret = fopen(path, "r");
    if (ret && ppath) {
        *ppath = strdup(path);
    }
    free(path);
    if (ret) {
      return ret;
    }
  }

  // Fall back to legacy config file names
  const char *home = getenv("HOME");
  if (home && strlen(home)) {
    auto path = ccalloc(strlen(home)+strlen(config_filename_legacy)+1, char);
    strcpy(path, home);
    strcpy(path+strlen(home), config_filename_legacy);
    FILE *ret = fopen(path, "r");
    if (ret && ppath)
      *ppath = path;
    else
      free(path);
    return ret;
  }
  return NULL;
}

/**
 * Parse a condition list in configuration file.
 */
void
parse_cfg_condlst(session_t *ps, const config_t *pcfg, c2_lptr_t **pcondlst,
    const char *name) {
  config_setting_t *setting = config_lookup(pcfg, name);
  if (setting) {
    // Parse an array of options
    if (config_setting_is_array(setting)) {
      int i = config_setting_length(setting);
      while (i--)
        condlst_add(ps, pcondlst, config_setting_get_string_elem(setting, i));
    }
    // Treat it as a single pattern if it's a string
    else if (CONFIG_TYPE_STRING == config_setting_type(setting)) {
      condlst_add(ps, pcondlst, config_setting_get_string(setting));
    }
  }
}

/**
 * Parse an opacity rule list in configuration file.
 */
static inline void
parse_cfg_condlst_opct(session_t *ps, const config_t *pcfg, const char *name) {
  config_setting_t *setting = config_lookup(pcfg, name);
  if (setting) {
    // Parse an array of options
    if (config_setting_is_array(setting)) {
      int i = config_setting_length(setting);
      while (i--)
        if (!parse_rule_opacity(ps, config_setting_get_string_elem(setting,
                i)))
          exit(1);
    }
    // Treat it as a single pattern if it's a string
    else if (CONFIG_TYPE_STRING == config_setting_type(setting)) {
      parse_rule_opacity(ps, config_setting_get_string(setting));
    }
  }
}

/**
 * Parse a configuration file from default location.
 */
void parse_config_libconfig(session_t *ps, bool *shadow_enable,
  bool *fading_enable, win_option_mask_t *winopt_mask)
{
  char *path = NULL;
  FILE *f;
  config_t cfg;
  int ival = 0;
  bool bval;
  double dval = 0.0;
  // libconfig manages string memory itself, so no need to manually free
  // anything
  const char *sval = NULL;

  f = open_config_file(ps->o.config_file, &path);
  if (!f) {
    if (ps->o.config_file) {
      printf_errfq(1, "(): Failed to read configuration file \"%s\".",
          ps->o.config_file);
      free(ps->o.config_file);
      ps->o.config_file = NULL;
    }
    return;
  }

  config_init(&cfg);
  {
    // dirname() could modify the original string, thus we must pass a
    // copy
    char *path2 = strdup(path);
    char *parent = dirname(path2);

    if (parent)
      config_set_include_dir(&cfg, parent);

    free(path2);
  }

  {
    int read_result = config_read(&cfg, f);
    fclose(f);
    f = NULL;
    if (read_result == CONFIG_FALSE) {
      printf("Error when reading configuration file \"%s\", line %d: %s\n",
          path, config_error_line(&cfg), config_error_text(&cfg));
      config_destroy(&cfg);
      free(path);
      return;
    }
  }
  config_set_auto_convert(&cfg, 1);

  if (path != ps->o.config_file) {
    assert(ps->o.config_file == NULL);
    ps->o.config_file = path;
  }

  // Get options from the configuration file. We don't do range checking
  // right now. It will be done later

  // -D (fade_delta)
  if (config_lookup_int(&cfg, "fade-delta", &ival))
    ps->o.fade_delta = ival;
  // -I (fade_in_step)
  if (config_lookup_float(&cfg, "fade-in-step", &dval))
    ps->o.fade_in_step = normalize_d(dval) * OPAQUE;
  // -O (fade_out_step)
  if (config_lookup_float(&cfg, "fade-out-step", &dval))
    ps->o.fade_out_step = normalize_d(dval) * OPAQUE;
  // -r (shadow_radius)
  config_lookup_int(&cfg, "shadow-radius", &ps->o.shadow_radius);
  // -o (shadow_opacity)
  config_lookup_float(&cfg, "shadow-opacity", &ps->o.shadow_opacity);
  // -l (shadow_offset_x)
  config_lookup_int(&cfg, "shadow-offset-x", &ps->o.shadow_offset_x);
  // -t (shadow_offset_y)
  config_lookup_int(&cfg, "shadow-offset-y", &ps->o.shadow_offset_y);
  // -i (inactive_opacity)
  if (config_lookup_float(&cfg, "inactive-opacity", &dval))
    ps->o.inactive_opacity = normalize_d(dval) * OPAQUE;
  // --active_opacity
  if (config_lookup_float(&cfg, "active-opacity", &dval))
    ps->o.active_opacity = normalize_d(dval) * OPAQUE;
  // -e (frame_opacity)
  config_lookup_float(&cfg, "frame-opacity", &ps->o.frame_opacity);
  // -c (shadow_enable)
  if (config_lookup_bool(&cfg, "shadow", &ival))
    *shadow_enable = ival;
  // -C (no_dock_shadow)
  if (config_lookup_bool(&cfg, "no-dock-shadow", &ival)) {
    printf_errf("(): option `no-dock-shadow` is deprecated, and will be removed.\n"
      " Please use the wintype option `shadow` of `dock` instead.");
    ps->o.wintype_option[WINTYPE_DOCK].shadow = false;
    winopt_mask[WINTYPE_DOCK].shadow = true;
  }
  // -G (no_dnd_shadow)
  if (config_lookup_bool(&cfg, "no-dnd-shadow", &ival)) {
    printf_errf("(): option `no-dnd-shadow` is deprecated, and will be removed.\n"
      " Please use the wintype option `shadow` of `dnd` instead.");
    ps->o.wintype_option[WINTYPE_DND].shadow = false;
    winopt_mask[WINTYPE_DND].shadow = true;
  };
  // -m (menu_opacity)
  if (config_lookup_float(&cfg, "menu-opacity", &dval)) {
    printf_errf("(): option `menu-opacity` is deprecated, and will be removed.\n"
      "Please use the wintype option `opacity` of `popup_menu` and `dropdown_menu` instead.");
    ps->o.wintype_option[WINTYPE_DROPDOWN_MENU].opacity = dval;
    ps->o.wintype_option[WINTYPE_POPUP_MENU].opacity = dval;
    winopt_mask[WINTYPE_DROPDOWN_MENU].opacity = true;
    winopt_mask[WINTYPE_POPUP_MENU].opacity = true;
  }
  // -f (fading_enable)
  if (config_lookup_bool(&cfg, "fading", &ival))
    *fading_enable = ival;
  // --no-fading-open-close
  lcfg_lookup_bool(&cfg, "no-fading-openclose", &ps->o.no_fading_openclose);
  // --no-fading-destroyed-argb
  lcfg_lookup_bool(&cfg, "no-fading-destroyed-argb",
      &ps->o.no_fading_destroyed_argb);
  // --shadow-red
  config_lookup_float(&cfg, "shadow-red", &ps->o.shadow_red);
  // --shadow-green
  config_lookup_float(&cfg, "shadow-green", &ps->o.shadow_green);
  // --shadow-blue
  config_lookup_float(&cfg, "shadow-blue", &ps->o.shadow_blue);
  // --shadow-exclude-reg
  if (config_lookup_string(&cfg, "shadow-exclude-reg", &sval))
    ps->o.shadow_exclude_reg_str = strdup(sval);
  // --inactive-opacity-override
  lcfg_lookup_bool(&cfg, "inactive-opacity-override",
      &ps->o.inactive_opacity_override);
  // --inactive-dim
  config_lookup_float(&cfg, "inactive-dim", &ps->o.inactive_dim);
  // --mark-wmwin-focused
  lcfg_lookup_bool(&cfg, "mark-wmwin-focused", &ps->o.mark_wmwin_focused);
  // --mark-ovredir-focused
  lcfg_lookup_bool(&cfg, "mark-ovredir-focused",
      &ps->o.mark_ovredir_focused);
  // --shadow-ignore-shaped
  lcfg_lookup_bool(&cfg, "shadow-ignore-shaped",
      &ps->o.shadow_ignore_shaped);
  // --detect-rounded-corners
  lcfg_lookup_bool(&cfg, "detect-rounded-corners",
      &ps->o.detect_rounded_corners);
  // --xinerama-shadow-crop
  lcfg_lookup_bool(&cfg, "xinerama-shadow-crop",
      &ps->o.xinerama_shadow_crop);
  // --detect-client-opacity
  lcfg_lookup_bool(&cfg, "detect-client-opacity",
      &ps->o.detect_client_opacity);
  // --refresh-rate
  config_lookup_int(&cfg, "refresh-rate", &ps->o.refresh_rate);
  // --vsync
  if (config_lookup_string(&cfg, "vsync", &sval) && !parse_vsync(ps, sval))
    exit(1);
  // --backend
  if (config_lookup_string(&cfg, "backend", &sval) && !parse_backend(ps, sval))
    exit(1);
  // --log-level
  if (config_lookup_string(&cfg, "log-level", &sval)) {
    auto level = string_to_log_level(sval);
    if (level == LOG_LEVEL_INVALID) {
      log_warn("Invalid log level, defaults to WARN");
    } else {
      log_set_level_tls(level);
    }
  }
  // --sw-opti
  lcfg_lookup_bool(&cfg, "sw-opti", &ps->o.sw_opti);
  // --use-ewmh-active-win
  lcfg_lookup_bool(&cfg, "use-ewmh-active-win",
      &ps->o.use_ewmh_active_win);
  // --unredir-if-possible
  lcfg_lookup_bool(&cfg, "unredir-if-possible",
      &ps->o.unredir_if_possible);
  // --unredir-if-possible-delay
  if (config_lookup_int(&cfg, "unredir-if-possible-delay", &ival))
    ps->o.unredir_if_possible_delay = ival;
  // --inactive-dim-fixed
  lcfg_lookup_bool(&cfg, "inactive-dim-fixed", &ps->o.inactive_dim_fixed);
  // --detect-transient
  lcfg_lookup_bool(&cfg, "detect-transient", &ps->o.detect_transient);
  // --detect-client-leader
  lcfg_lookup_bool(&cfg, "detect-client-leader",
      &ps->o.detect_client_leader);
  // --shadow-exclude
  parse_cfg_condlst(ps, &cfg, &ps->o.shadow_blacklist, "shadow-exclude");
  // --fade-exclude
  parse_cfg_condlst(ps, &cfg, &ps->o.fade_blacklist, "fade-exclude");
  // --focus-exclude
  parse_cfg_condlst(ps, &cfg, &ps->o.focus_blacklist, "focus-exclude");
  // --invert-color-include
  parse_cfg_condlst(ps, &cfg, &ps->o.invert_color_list, "invert-color-include");
  // --blur-background-exclude
  parse_cfg_condlst(ps, &cfg, &ps->o.blur_background_blacklist, "blur-background-exclude");
  // --opacity-rule
  parse_cfg_condlst_opct(ps, &cfg, "opacity-rule");
  // --unredir-if-possible-exclude
  parse_cfg_condlst(ps, &cfg, &ps->o.unredir_if_possible_blacklist, "unredir-if-possible-exclude");
  // --blur-background
  lcfg_lookup_bool(&cfg, "blur-background", &ps->o.blur_background);
  // --blur-background-frame
  lcfg_lookup_bool(&cfg, "blur-background-frame",
      &ps->o.blur_background_frame);
  // --blur-background-fixed
  lcfg_lookup_bool(&cfg, "blur-background-fixed",
      &ps->o.blur_background_fixed);
  // --blur-kern
  if (config_lookup_string(&cfg, "blur-kern", &sval)
      && !parse_conv_kern_lst(ps, sval, ps->o.blur_kerns, MAX_BLUR_PASS))
    exit(1);
  // --resize-damage
  config_lookup_int(&cfg, "resize-damage", &ps->o.resize_damage);
  // --glx-no-stencil
  lcfg_lookup_bool(&cfg, "glx-no-stencil", &ps->o.glx_no_stencil);
  // --glx-no-rebind-pixmap
  lcfg_lookup_bool(&cfg, "glx-no-rebind-pixmap", &ps->o.glx_no_rebind_pixmap);
  // --glx-swap-method
  if (config_lookup_string(&cfg, "glx-swap-method", &sval)
      && !parse_glx_swap_method(ps, sval))
    exit(1);
  // --glx-use-gpushader4
  lcfg_lookup_bool(&cfg, "glx-use-gpushader4", &ps->o.glx_use_gpushader4);
  // --xrender-sync
  lcfg_lookup_bool(&cfg, "xrender-sync", &ps->o.xrender_sync);
  // --xrender-sync-fence
  lcfg_lookup_bool(&cfg, "xrender-sync-fence", &ps->o.xrender_sync_fence);

  if (lcfg_lookup_bool(&cfg, "clear-shadow", &bval))
    printf_errf("(): \"clear-shadow\" is removed as an option, and is always"
                " enabled now. Consider removing it from your config file");
  if (lcfg_lookup_bool(&cfg, "paint-on-overlay", &bval))
    printf_errf("(): \"paint-on-overlay\" has been removed as an option, and "
                "is enabled whenever possible");

  if (config_lookup_float(&cfg, "alpha-step", &dval))
    printf_errf("(): \"alpha-step\" has been removed, compton now tries to make use"
                " of all alpha values");

  const char *deprecation_message = "has been removed. If you encounter problems "
    "without this feature, please feel free to open a bug report";
  if (lcfg_lookup_bool(&cfg, "glx-use-copysubbuffermesa", &bval) && bval)
    printf_errf("(): \"glx-use-copysubbuffermesa\" %s", deprecation_message);
  if (lcfg_lookup_bool(&cfg, "glx-copy-from-front", &bval) && bval)
    printf_errf("(): \"glx-copy-from-front\" %s", deprecation_message);

  // Wintype settings

  // XXX ! Refactor all the wintype_* arrays into a struct
  for (wintype_t i = 0; i < NUM_WINTYPES; ++i) {
    char *str = mstrjoin("wintypes.", WINTYPES[i]);
    config_setting_t *setting = config_lookup(&cfg, str);
    free(str);

    win_option_t *o = &ps->o.wintype_option[i];
    win_option_mask_t *mask = &winopt_mask[i];
    if (setting) {
      if (config_setting_lookup_bool(setting, "shadow", &ival)) {
        o->shadow = ival;
        mask->shadow = true;
      }
      if (config_setting_lookup_bool(setting, "fade", &ival)) {
        o->fade = ival;
        mask->fade = true;
      }
      if (config_setting_lookup_bool(setting, "focus", &ival)) {
        o->focus = ival;
        mask->focus = true;
      }
      if (config_setting_lookup_bool(setting, "full-shadow", &ival)) {
        o->full_shadow = ival;
        mask->full_shadow = true;
      }
      if (config_setting_lookup_bool(setting, "redir-ignore", &ival)) {
        o->redir_ignore = ival;
        mask->redir_ignore = true;
      }

      double fval;
      if (config_setting_lookup_float(setting, "opacity", &fval)) {
        o->opacity = normalize_d(fval);
        mask->opacity = true;
      }
    }
  }

  config_destroy(&cfg);
}
