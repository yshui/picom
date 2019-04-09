Compton
=======

**This is a development branch, bug to be expected**

This is forked from the original Compton because that seems to have become unmaintained.

The current battle plan of this fork is to refactor it to make the code _possible_ to maintain, so potential contributors won't be scared away when they take a look at the code.

We also try to fix bugs.

The original README can be found [here](README_orig.md)

## Changelog

See [Releases](https://github.com/yshui/compton/releases)

## Build

### Dependencies

Assuming you already have all the usual building tools installed (e.g. gcc, meson, ninja, etc.), you still need:

* libx11
* libx11-xcb
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
* xcb-xinerama (optional, disable with the `-Dxinerama=false` meson configure flag)
* pixman
* libdbus (optional, disable with the `-Ddbus=false` meson configure flag)
* libconfig (optional, disable with the `-Dconfig_file=false` meson configure flag)
* libxdg-basedir (optional, disable with the `-Dconfig_file=false` meson configure flag)
* libGL (optional, disable with the `-Dopengl=false` meson configure flag)
* libpcre (optional, disable with the `-Dregex=false` meson configure flag)
* libev
* uthash

To build the documents, you need `asciidoc`

### To build

```bash
$ meson --buildtype=release . build
$ ninja -C build
```

##### FreeBSD
To help meson find libraries and headers under FreeBSD:
```bash
$ CPPFLAGS=-I/usr/local/include LDFLAGS=-L/usr/local/lib meson --buildtype=release . build
$ ninja -C build
```

Built binary can be found in `build/src`

### To install

``` bash
$ ninja -C build install
```

Default install prefix is `/usr/local`, you can change it with `meson configure -Dprefix=<path> build`

## How to Contribute

### Code

You can look at the [Projects](https://github.com/yshui/compton/projects) page, and see if there is anything interests you. Or you can take a look at the [Issues](https://github.com/yshui/compton/issues).

### Non-code

Even if you don't want to contribute code, you can still contribute by compiling and running this branch, and report any issue you can find.

## Contributors

See [CONTRIBUTORS](CONTRIBUTORS)
