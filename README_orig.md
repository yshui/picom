# Compton

[![Join the chat at https://gitter.im/chjj/compton](https://badges.gitter.im/Join%20Chat.svg)](https://gitter.im/chjj/compton?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)

__Compton__ is a compositor for X, and a fork of __xcompmgr-dana__.

I was frustrated by the low amount of standalone lightweight compositors.
Compton was forked from Dana Jansens' fork of xcompmgr and refactored.  I fixed
whatever bug I found, and added features I wanted. Things seem stable, but don't
quote me on it. I will most likely be actively working on this until I get the
features I want. This is also a learning experience for me. That is, I'm
partially doing this out of a desire to learn Xlib.

## Changes from xcompmgr:

* OpenGL backend (`--backend glx`), in addition to the old X Render backend.
* Inactive window transparency (`-i`) / dimming (`--inactive-dim`).
* Titlebar/frame transparency (`-e`).
* Menu transparency (`-m`, thanks to Dana).
* shadows are now enabled for argb windows, e.g. terminals with transparency
* removed serverside shadows (and simple compositing) to clean the code,
  the only option that remains is clientside shadows
* configuration files (see the man page for more details)
* colored shadows (`--shadow-[red/green/blue]`)
* a new fade system
* VSync support (not always working)
* Blur of background of transparent windows, window color inversion (bad in performance)
* Some more options...

## Fixes from the original xcompmgr:

* fixed a segfault when opening certain window types
* fixed a memory leak caused by not freeing up shadows (from the freedesktop
  repo)
* fixed the conflict with chromium and similar windows
* [many more](https://github.com/chjj/compton/issues)

## Building

### Dependencies:

__B__ for build-time

__R__ for runtime

* libx11 (B,R)
* libxcomposite (B,R)
* libxdamage (B,R)
* libxfixes (B,R)
* libXext (B,R)
* libxrender (B,R)
* libXrandr (B,R)
* libXinerama (B,R) (Can be disabled with `NO_XINERAMA` at compile time)
* pkg-config (B)
* make (B)
* xproto / x11proto (B)
* sh (R)
* xprop,xwininfo / x11-utils (R)
* libpcre (B,R) (Can be disabled with `NO_REGEX_PCRE` at compile time)
* libconfig (B,R) (Can be disabled with `NO_LIBCONFIG` at compile time)
* libdrm (B) (Can be disabled with `NO_VSYNC_DRM` at compile time)
* libGL (B,R) (Can be disabled with `NO_VSYNC_OPENGL` at compile time)
* libdbus (B,R) (Can be disabled with `NO_DBUS` at compile time)
* asciidoc (B) (and docbook-xml-dtd-4.5, libxml-utils, libxslt, xsltproc, xmlto, etc. if your distro doesn't pull them in)

### How to build

To build, make sure you have the dependencies above:

```bash
# Make the main program
$ make
# Make the man pages
$ make docs
# Install
$ make install
```

(Compton does include a `_CMakeLists.txt` in the tree, but we haven't decided whether we should switch to CMake yet. The `Makefile` is fully usable right now.)

## Known issues

* Our [FAQ](https://github.com/chjj/compton/wiki/faq) covers some known issues.

* VSync does not work too well. You may check the [VSync Guide](https://github.com/chjj/compton/wiki/vsync-guide) for how to get (possibly) better effects.

* If `--unredir-if-possible` is enabled, when compton redirects/unredirects windows, the screen may flicker. Using `--paint-on-overlay` minimizes the problem from my observation, yet I do not know if there's a cure.

* compton may not track focus correctly in all situations. The focus tracking code is experimental. `--use-ewmh-active-win` might be helpful.

* The performance of blur under X Render backend might be pretty bad. OpenGL backend could be faster.

* With `--blur-background` you may sometimes see weird lines around damaged area. `--resize-damage YOUR_BLUR_RADIUS` might be helpful in the case.

## Usage

Please refer to the Asciidoc man pages (`man/compton.1.asciidoc` & `man/compton-trans.1.asciidoc`) for more details and examples.

Note a sample configuration file `compton.sample.conf` is included in the repository.

## Support

* Bug reports and feature requests should go to the "Issues" section above.

* Our (semi?) official IRC channel is #compton on FreeNode.

* Some information is available on the wiki, including [FAQ](https://github.com/chjj/compton/wiki/faq), [VSync Guide](https://github.com/chjj/compton/wiki/vsync-guide), and [Performance Guide](https://github.com/chjj/compton/wiki/perf-guide).

## License

Although compton has kind of taken on a life of its own, it was originally
an xcompmgr fork. xcompmgr has gotten around. As far as I can tell, the lineage
for this particular tree is something like:

* Keith Packard (original author)
* Matthew Hawn
* ...
* Dana Jansens
* chjj and richardgv

Not counting the tens of people who forked it in between.

Compton is distributed under MIT license, as far as I (richardgv) know. See LICENSE for more info.
