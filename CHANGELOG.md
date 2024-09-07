# v12-rc4 (2024-Sep-08)

## Bug fixes

* Windows become completely black when `rules` and `inactive-dim` are set at the same time
* Fix segmentation fault during unredirection if the geometry change animation is used (#1333, thanks to @monsterovich)
* Fix many rare race conditions in the window management code (#1334)

# v12-rc3 (2024-Aug-30)

## Bug fixes

* `transparent-clipping` has no effect (#1317)
* `unredir` in window rules not being parsed correctly
* Changing window opacity with `picom-trans` does not take effect immediately (#1315)

## Documentation

* Document behavior change around rounded corners and fullscreen windows (#1323)

# v12-rc2 (2024-Aug-17)

## Bug fixes

* Setting corner-radius to 0 cause all windows to not render (#1311)
* Setting corner-radius causes windows to have a 1-pixel transparent border
* Window shaders no longer work (#1312)

# v12-rc1 (2024-Aug-12)

## New features

* Animations! Yes, now picom officially supports animations. For examples, and information on how to configure it, please go to our [documentation site](https://picom.app/#_animations). There are some video clips in #1253 as well. (#1220 #1253 #1303 #1305 #1308 #1310)
* Universal window rules (#1284). One option to rule them all! Added new configuration option `rules` to replace all existing rule options, and to provide more flexibility on top of that. See [picom(1)](https://picom.app/#_window_rules) for more details. This can be used to configure per-window animations.
* `@include` directives in config file now also search in `$XDG_CONFIG_HOME/picom/include` and `$XDG_CONFIG_DIRS/picom/include`, in addition to relative to the config file's parent directory.
* Allow `corner-radius-rules` to override `corner-radius = 0`. Previously setting corner radius to 0 globally disables rounded corners. (#1170)
* New `picom-inspect` tool, which lets you test out your picom rules. `man picom-inspect(1)` for more details. Sample output:

  ```
  ...
  Checking rounded-corners-exclude:
      window_type = "dock" ... not matched
      window_type = "desktop" ... not matched
      window_type *= "menu" ... not matched
      fullscreen = 1 ... not matched
  Checking opacity-rule:
      _NET_WM_STATE@[0] *= "_NET_WM_STATE_HIDDEN" ... not matched
  Checking corner-radius-rule:
      class_g = "Alacritty" ... matched/10

  Here are some rule(s) that match this window:
      name = '[0.2.1] ./picom-inspect: ~/p/picom(./picom-inspect: ~/p/picom)*'
      class_i = 'Alacritty'
      class_g = 'Alacritty'
      window_type = 'normal'
      ! fullscreen
      border_width = 0
  ```
* picom now has a rudimentary plugin system. At the moment, the only thing you can do with it is loading custom backends.

## Notable changes

* `override_redirect` in rules now only matches top-level windows that doesn't have a client window. Some window managers (e.g. awesome) set override_redirect for all window manager frame windows, causing this rule to match against everything (#625).
* Marginally improve performance when resizing/opening/closing windows. (#1190)
* Type and format specifiers are no longer used in rules. These specifiers are what you put after the colon (':') in rules, e.g. the `:32c` in `"_GTK_FRAME_EXTENTS@:32c"`. Now this information is ignored and the property is matched regardless of format or type.
* `backend` is now a required option. picom will not start if one is not specified explicitly.
* New predefined target for conditions: `group_focused`. This target indicate whether the focused window is in the same window group as the window being matched.
* Meaning of `window_type` in conditions changed slightly, now it supports windows with multiple types. (However the behavior of `wintypes` remains unchanged.)

## Deprecated features

* Setting `--shadow-exclude-reg` is now a hard error. It was deprecated almost since the start of `picom`. `--clip-shadow-above` is the better alternative. (#1254)
* Remove command line options `-n`, `-a`, and `-s`. They were removed more than 10 years ago, it's time to finally get rid of them entirely. (#1254)
* Remove error message for `--glx-swap-method`, it was deprecated in v6.
* Remove error message for passing argument to `--vsync` arguments, it was deprecated in v5.
* Option `--opengl` is now deprecated, use `--backend=glx` instead.

## Bug fixes

* Fix ghosting artifacts that sometimes occur when window manager is restarted (#1081)
* Fix a bug where rounded corner is disabled after making a window fullscreen and back (#1216)
* Fix ugly looking rounded corners (#1261)

## Build changes

* picom now uses some OpenGL 4.3 features.
* picom now optionally depends on `rtkit` at runtime to give itself realtime scheduling priority.
* `libconfig` is now a mandatory dependency, with a minimal supported version of 1.7.
* `xcb-dpms` is not needed anymore.
* `libXext` is not needed anymore.
* man pages are now built with asciidoctor, instead of asciidoc.

## Behind the scene changes

* The X critical section is removed, picom no longer grabs the server to fetch updates. Hopefully, if everything works, this change is unnoticeable. Minor responsiveness improvements could be possible, but I won't bet on it. The main point of this change is this makes debugging much less painful. Previously if you breaks inside the X critical section, the whole X server will lock up, and you would have to connect to the computer remotely to recover. Now there is no longer such worries. This also avoids a bug inside Xorg that makes server grabbing unreliable.

# v11.2 (2024-Feb-13)

## Build changes

* `picom` now depends on `libepoxy` for OpenGL symbol management.

## Bug fixes

* Workaround a NVIDIA problem that causes high CPU usage after suspend/resume (#1172, #1168)
* Fix building on OpenBSD (#1189, #1188)
* Fix occasional freezes (#1040, #1145, #1166)
* Fix `corner-radius-rules` not applying sometimes (#1177)
* Fix window shader not having an effect when frame opacity is enabled (#1174)
* Fix binding root pixmap in case of depth mismatch (#984)

# v11.1 (2024-Jan-28)

## Bug fixes

* Fix missing fading on window close for some window managers. (#704)

# v11 (2024-Jan-20)

## Build changes

* Due to some caveats discovered related to setting the `CAP_SYS_NICE` capability, it is now recommended to **NOT** set this capability for picom.

## Deprecations

* Uses of `--sw-opti`, and `--respect-prop-shadow` are now hard errors.
* `-F` has been removed completely. It was deprecated before the picom fork.

# v11-rc1 (2024-Jan-14)

## Notable features

* picom now uses dithering to prevent banding. Banding is most notable when using a strong background blur. (#952)
* Frame pacing. picom uses present feedback information to schedule new frames when it makes sense to do so. This improves latency, and replaces the `glFlush` and `GL_MaxFramesAllowed=1` hacks we used to do for NVIDIA. (#968 #1156)
* Some missing features have been implemented for the EGL backend (#1004 #1007)

## Bug fixes

* Many memory/resource leak fixes thanks to @absolutelynothelix . (#977 #978 #979 #980 #982 #985 #992 #1009 #1022)
* Fix tiling of wallpaper. (#1002)
* Fix some blur artifacts (#1095)
* Fix shadow color for transparent shadows (#1124)
* Don't spam logs when another compositor is running (#1104)
* Fix rounded corners showing as black with the xrender backend (#1003)
* Fix blur with rounded windows (#950)

## Build changes

* Dependency `pcre` has been replaced by `pcre2`.
* New dependency `xcb-util`.
* `xinerama` is no longer used.
* `picom` now tries to give itself a real-time scheduling priority. ~~Please consider giving `picom` the `CAP_SYS_NICE` capability when packaging it.~~

## Deprecations

* The `kawase` blur method is removed. Note this is just an alias to the `dual_kawase` method, which is still available. (#1102)

# Earlier versions

Please see the GitHub releases page.
