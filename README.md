picom
=====

__picom__ is a compositor for X, and a [fork of Compton](History.md).

**This is a development branch, bugs to be expected**

You can leave your feedback or thoughts in the [discussion tab](https://github.com/yshui/picom/discussions).

## Change Log

See [Releases](https://github.com/yshui/picom/releases)

## Build

### Dependencies

Assuming you already have all the usual building tools installed (e.g. gcc, python, meson, ninja, etc.), you still need:

* libx11
* libx11-xcb
* libXext
* xproto
* xcb
* xcb-damage
* xcb-dpms
* xcb-xfixes
* xcb-shape
* xcb-renderutil
* xcb-render
* xcb-randr
* xcb-composite
* xcb-image
* xcb-present
* xcb-glx
* pixman
* libdbus (optional, disable with the `-Ddbus=false` meson configure flag)
* libconfig (optional, disable with the `-Dconfig_file=false` meson configure flag)
* libGL, libEGL (optional, disable with the `-Dopengl=false` meson configure flag)
* libpcre2 (optional, disable with the `-Dregex=false` meson configure flag)
* libev
* uthash

On Debian based distributions (e.g. Ubuntu), the needed packages are

```
libxext-dev libxcb1-dev libxcb-damage0-dev libxcb-dpms0-dev libxcb-xfixes0-dev libxcb-shape0-dev libxcb-render-util0-dev libxcb-render0-dev libxcb-randr0-dev libxcb-composite0-dev libxcb-image0-dev libxcb-present-dev libxcb-glx0-dev libpixman-1-dev libdbus-1-dev libconfig-dev libgl-dev libegl-dev libpcre2-dev libevdev-dev uthash-dev libev-dev libx11-xcb-dev meson
```

On Fedora, the needed packages are

```
dbus-devel gcc git libconfig-devel libdrm-devel libev-devel libX11-devel libX11-xcb libXext-devel libxcb-devel libGL-devel libEGL-devel meson pcre2-devel pixman-devel uthash-devel xcb-util-image-devel xcb-util-renderutil-devel xorg-x11-proto-devel
```

To build the documents, you need `asciidoc`

### To build

```bash
$ git submodule update --init --recursive
$ meson setup --buildtype=release . build
$ ninja -C build
```

Built binary can be found in `build/src`

If you have libraries and/or headers installed at non-default location (e.g. under `/usr/local/`), you might need to tell meson about them, since meson doesn't look for dependencies there by default.

You can do that by setting the `CPPFLAGS` and `LDFLAGS` environment variables when running `meson`. Like this:

```bash
$ LDFLAGS="-L/path/to/libraries" CPPFLAGS="-I/path/to/headers" meson setup --buildtype=release . build
```

As an example, on FreeBSD, you might have to run meson with:
```bash
$ LDFLAGS="-L/usr/local/lib" CPPFLAGS="-I/usr/local/include" meson setup --buildtype=release . build
$ ninja -C build
```

### To install

``` bash
$ ninja -C build install
```

Default install prefix is `/usr/local`, you can change it with `meson configure -Dprefix=<path> build`

## How to Contribute

All contributions are welcome!

New features you think should be included in picom, a fix for a bug you found - please open a PR!

You can take a look at the [Issues](https://github.com/yshui/picom/issues).

Contributions to the documents and wiki are also appreciated.

Even if you don't want to add anything to picom, you are still helping by compiling and running this branch, and report any issue you can find.

### Become a Collaborator

Becoming a collaborator of picom requires significant time commitment. You are expected to reply to issue reports, reviewing PRs, and sometimes fix bugs or implement new feature. You won't be able to push to the main branch directly, and all you code still has to go through code review.

If this sounds good to you, feel free to contact me.

## Contributors

See [CONTRIBUTORS](CONTRIBUTORS)

The README for the [original Compton project](https://github.com/chjj/compton/) can be found [here](History.md#Compton).

## Licensing

picom is free software, made available under the [MIT](LICENSES/MIT) and [MPL-2.0](LICENSES/MPL-2.0) software
licenses. See the individual source files for details.
