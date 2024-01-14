# Unreleased

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
* `xinerama` is no longer used.

## Deprecations

* The `kawase` blur method is removed. Note this is just an alias to the `dual_kawase` method, which is still available. (#1102)

# Earlier versions

Please see the GitHub releases page.
