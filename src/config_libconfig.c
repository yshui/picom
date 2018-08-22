#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <libgen.h>
#include <libconfig.h>

#include "common.h"
#include "config.h"

/**
 * Wrapper of libconfig's <code>config_lookup_int</code>.
 *
 * To convert <code>int</code> value <code>config_lookup_bool</code>
 * returns to <code>bool</code>.
 */
static inline void
lcfg_lookup_bool(const config_t *config, const char *path,
    bool *value) {
  int ival;

  if (config_lookup_bool(config, path, &ival))
    *value = ival;
}

/**
 * Wrapper of libconfig's <code>config_lookup_int</code>.
 *
 * To deal with the different value types <code>config_lookup_int</code>
 * returns in libconfig-1.3 and libconfig-1.4.
 */
static inline int
lcfg_lookup_int(const config_t *config, const char *path, int *value) {
  return config_lookup_int(config, path, value);
}

/**
 * Get a file stream of the configuration file to read.
 *
 * Follows the XDG specification to search for the configuration file.
 */
FILE *
open_config_file(char *cpath, char **ppath) {
  const static char *config_filename = "/compton.conf";
  const static char *config_filename_legacy = "/.compton.conf";
  const static char *config_home_suffix = "/.config";
  const static char *config_system_dir = "/etc/xdg";

  char *dir = NULL, *home = NULL;
  char *path = cpath;
  FILE *f = NULL;

  if (path) {
    f = fopen(path, "r");
    if (f && ppath)
      *ppath = path;
    return f;
  }

  // Check user configuration file in $XDG_CONFIG_HOME firstly
  if (!((dir = getenv("XDG_CONFIG_HOME")) && strlen(dir))) {
    if (!((home = getenv("HOME")) && strlen(home)))
      return NULL;

    path = mstrjoin3(home, config_home_suffix, config_filename);
  }
  else
    path = mstrjoin(dir, config_filename);

  f = fopen(path, "r");

  if (f && ppath)
    *ppath = path;
  else
    free(path);
  if (f)
    return f;

  // Then check user configuration file in $HOME
  if ((home = getenv("HOME")) && strlen(home)) {
    path = mstrjoin(home, config_filename_legacy);
    f = fopen(path, "r");
    if (f && ppath)
      *ppath = path;
    else
      free(path);
    if (f)
      return f;
  }

  // Check system configuration file in $XDG_CONFIG_DIRS at last
  if ((dir = getenv("XDG_CONFIG_DIRS")) && strlen(dir)) {
    char *part = strtok(dir, ":");
    while (part) {
      path = mstrjoin(part, config_filename);
      f = fopen(path, "r");
      if (f && ppath)
        *ppath = path;
      else
        free(path);
      if (f)
        return f;
      part = strtok(NULL, ":");
    }
  }
  else {
    path = mstrjoin(config_system_dir, config_filename);
    f = fopen(path, "r");
    if (f && ppath)
      *ppath = path;
    else
      free(path);
    if (f)
      return f;
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
void
parse_config(session_t *ps, struct options_tmp *pcfgtmp) {
  char *path = NULL;
  FILE *f;
  config_t cfg;
  int ival = 0;
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
    char *path2 = mstrcpy(path);
    char *parent = dirname(path2);

    if (parent)
      config_set_include_dir(&cfg, parent);

    free(path2);
  }

  {
    int read_result = config_read(&cfg, f);
    fclose(f);
    f = NULL;
    if (CONFIG_FALSE == read_result) {
      printf("Error when reading configuration file \"%s\", line %d: %s\n",
          path, config_error_line(&cfg), config_error_text(&cfg));
      config_destroy(&cfg);
      free(path);
      return;
    }
  }
  config_set_auto_convert(&cfg, 1);

  if (path != ps->o.config_file) {
    free(ps->o.config_file);
    ps->o.config_file = path;
  }

  // Get options from the configuration file. We don't do range checking
  // right now. It will be done later

  // -D (fade_delta)
  if (lcfg_lookup_int(&cfg, "fade-delta", &ival))
    ps->o.fade_delta = ival;
  // -I (fade_in_step)
  if (config_lookup_float(&cfg, "fade-in-step", &dval))
    ps->o.fade_in_step = normalize_d(dval) * OPAQUE;
  // -O (fade_out_step)
  if (config_lookup_float(&cfg, "fade-out-step", &dval))
    ps->o.fade_out_step = normalize_d(dval) * OPAQUE;
  // -r (shadow_radius)
  lcfg_lookup_int(&cfg, "shadow-radius", &ps->o.shadow_radius);
  // -o (shadow_opacity)
  config_lookup_float(&cfg, "shadow-opacity", &ps->o.shadow_opacity);
  // -l (shadow_offset_x)
  lcfg_lookup_int(&cfg, "shadow-offset-x", &ps->o.shadow_offset_x);
  // -t (shadow_offset_y)
  lcfg_lookup_int(&cfg, "shadow-offset-y", &ps->o.shadow_offset_y);
  // -i (inactive_opacity)
  if (config_lookup_float(&cfg, "inactive-opacity", &dval))
    ps->o.inactive_opacity = normalize_d(dval) * OPAQUE;
  // --active_opacity
  if (config_lookup_float(&cfg, "active-opacity", &dval))
    ps->o.active_opacity = normalize_d(dval) * OPAQUE;
  // -e (frame_opacity)
  config_lookup_float(&cfg, "frame-opacity", &ps->o.frame_opacity);
  // -z (clear_shadow)
  lcfg_lookup_bool(&cfg, "clear-shadow", &ps->o.clear_shadow);
  // -c (shadow_enable)
  if (config_lookup_bool(&cfg, "shadow", &ival) && ival)
    wintype_arr_enable(ps->o.wintype_shadow);
  // -C (no_dock_shadow)
  lcfg_lookup_bool(&cfg, "no-dock-shadow", &pcfgtmp->no_dock_shadow);
  // -G (no_dnd_shadow)
  lcfg_lookup_bool(&cfg, "no-dnd-shadow", &pcfgtmp->no_dnd_shadow);
  // -m (menu_opacity)
  config_lookup_float(&cfg, "menu-opacity", &pcfgtmp->menu_opacity);
  // -f (fading_enable)
  if (config_lookup_bool(&cfg, "fading", &ival) && ival)
    wintype_arr_enable(ps->o.wintype_fade);
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
  if (config_lookup_string(&cfg, "shadow-exclude-reg", &sval)
      && !parse_geometry(ps, sval, &ps->o.shadow_exclude_reg_geom))
    exit(1);
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
  lcfg_lookup_int(&cfg, "refresh-rate", &ps->o.refresh_rate);
  // --vsync
  if (config_lookup_string(&cfg, "vsync", &sval) && !parse_vsync(ps, sval))
    exit(1);
  // --backend
  if (config_lookup_string(&cfg, "backend", &sval) && !parse_backend(ps, sval))
    exit(1);
  // --alpha-step
  config_lookup_float(&cfg, "alpha-step", &ps->o.alpha_step);
  // --dbe
  lcfg_lookup_bool(&cfg, "dbe", &ps->o.dbe);
  // --paint-on-overlay
  lcfg_lookup_bool(&cfg, "paint-on-overlay", &ps->o.paint_on_overlay);
  // --sw-opti
  lcfg_lookup_bool(&cfg, "sw-opti", &ps->o.sw_opti);
  // --use-ewmh-active-win
  lcfg_lookup_bool(&cfg, "use-ewmh-active-win",
      &ps->o.use_ewmh_active_win);
  // --unredir-if-possible
  lcfg_lookup_bool(&cfg, "unredir-if-possible",
      &ps->o.unredir_if_possible);
  // --unredir-if-possible-delay
  if (lcfg_lookup_int(&cfg, "unredir-if-possible-delay", &ival))
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
  lcfg_lookup_int(&cfg, "resize-damage", &ps->o.resize_damage);
  // --glx-no-stencil
  lcfg_lookup_bool(&cfg, "glx-no-stencil", &ps->o.glx_no_stencil);
  // --glx-copy-from-front
  lcfg_lookup_bool(&cfg, "glx-copy-from-front", &ps->o.glx_copy_from_front);
  // --glx-use-copysubbuffermesa
  lcfg_lookup_bool(&cfg, "glx-use-copysubbuffermesa", &ps->o.glx_use_copysubbuffermesa);
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
  // Wintype settings

  for (wintype_t i = 0; i < NUM_WINTYPES; ++i) {
    char *str = mstrjoin("wintypes.", WINTYPES[i]);
    config_setting_t *setting = config_lookup(&cfg, str);
    free(str);
    if (setting) {
      if (config_setting_lookup_bool(setting, "shadow", &ival))
        ps->o.wintype_shadow[i] = (bool) ival;
      if (config_setting_lookup_bool(setting, "fade", &ival))
        ps->o.wintype_fade[i] = (bool) ival;
      if (config_setting_lookup_bool(setting, "focus", &ival))
        ps->o.wintype_focus[i] = (bool) ival;

      double fval;
      if (config_setting_lookup_float(setting, "opacity", &fval))
        ps->o.wintype_opacity[i] = normalize_d(fval);
    }
  }

  config_destroy(&cfg);
}
