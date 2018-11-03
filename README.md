Compton
=======

**This is a development branch, bug to be expected**

This is forked from the original Compton because that seems to have become unmaintained. I'll merge pull requests as they appear upstream, as well as trying to fix bugs reported to upstream, or found by myself.

The original README can be found [here](README_orig.md)

## Build

### Dependencies

Assuming you already have all the usual building tools installed (e.g. gcc, meson, ninja, etc.), you still need:

* libx11
* libXext
* xproto
* xcb
* xcb-damage
* xcb-xfixes
* xcb-shape
* xcb-renderutil
* xcb-render
* xcb-randr
* xcb-composite
* xcb-image
* xcb-present
* xcb-xinerama (optional, disable with `-Dxinerama=false` meson configure flag)
* pixman
* libdbus (optional, disable with the `-Ddbus=false` meson configure flag)
* libconfig (optional, disable with the `-Dconfig_file=false` meson configure flag)
* libGL (optional, disable with the `-Dopengl=false` meson configure flag)
* libpcre (optional, disable with the `-Dregex=false` meson configure flag)
* libev

To build the documents, you need `asciidoc`

### How to build

```bash
$ meson --buildtype=release . build
$ ninja -C build
```

Built binary can be found in `build/src`

## How to Contribute

### Code

You can look at the [Projects](https://github.com/yshui/compton/projects) page, and see if there is anything interests you. Or you can take a look at the [Issues](https://github.com/yshui/compton/issues).

### Non-code

Even if you don't want to contribute code, you can still contribute by compiling and running this branch, and report any issue you can find.

## Contributors

See [CONTRIBUTORS](CONTRIBUTORS)
