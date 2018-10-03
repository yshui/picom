Compton
=======

**This is a development branch, bug to be expected**

This is forked from the original Compton because that seems to have become unmaintained. I'll merge pull requests as they appear upstream, as well as trying to fix bugs reported to upstream, or found by myself.

New features are not likely to be added, since I expect compton to become irrelevant in near future.

The original README can be found [here](README_orig.md)

## Build

### Dependencies

Assuming you already have all the usual building tools installed (e.g. gcc, make, etc.), you still need:

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
* xcb-xinerama (optional, disable with `NO_XINERAMA=1` make flag)
* pixman
* libdbus (optional, disable with the `NO_DBUS=1` make flag)
* libconfig (optional, disable with the `NO_LIBCONFIG=1` make flag)
* libGL (optional, disable with the `NO_OPENGL=1` make flag)
* libpcre (optional, disable with the `NO_REGEX_PCRE=1` make flag)
* libev

To build the documents, you need `asciidoc`

### How to build

```bash
$ make
$ make install
```

## How to Contribute

### Code

You can look at the [Projects](https://github.com/yshui/compton/projects) page, and see if there is anything interests you. Or you can take a look at the [Issues](https://github.com/yshui/compton/issues).

### Non-code

Even if you don't want to contribute code, you can still contribute by compiling and running this branch, and report any issue you can find.

## Contributors

See [CONTRIBUTORS](CONTRIBUTORS)
