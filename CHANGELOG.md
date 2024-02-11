# Unreleased

## New features

* Allow `corner-radius-rules` to override `corner-radius = 0`. Previously setting corner radius to 0 globally disables rounded corners. (#1170)

## Build changes

* `picom` now depends on `libepoxy` for OpenGL symbol management.

## Bug fixes

* Workaround a NVIDIA problem that causes high CPU usage after suspend/resume (#1172, #1168)
* Fix building on OpenBSD (#1189, #1188)

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
