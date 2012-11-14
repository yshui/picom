# Compton

__Compton__ is a compositor for X, and a fork of __xcompmgr-dana__.

I was frustrated by the low amount of standalone lightweight compositors.
Compton was forked from Dana Jansens' fork of xcompmgr and refactored.  I fixed
whatever bug I found, and added features I wanted. Things seem stable, but don't
quote me on it. I will most likely be actively working on this until I get the
features I want. This is also a learning experience for me. That is, I'm
partially doing this out of a desire to learn Xlib.

## Changes from xcompmgr:

* __inactive window transparency__ (specified with `-i`)
* __titlebar/frame transparency__ (specified with `-e`)
* menu transparency (thanks to Dana)
* shadows are now enabled for argb windows, e.g. terminals with transparency
* removed serverside shadows (and simple compositing) to clean the code,
  the only option that remains is clientside shadows
* configuration files (specified with `--config`)
* colored shadows (with `--shadow-[red/green/blue] value`)
* a new fade system
* vsync (still under development)
* several more options

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
* pkg-config (B)
* make (B)
* xproto / x11proto (B)
* bash (R)
* xprop,xwininfo / x11-utils (R)
* libpcre (B,R) (Will probably be made optional soon)
* libconfig (B,R) (Will probably be made optional soon)
* libdrm (B) (Will probably be made optional soon)
* libGL (B,R) (Will probably be made optional soon)
* asciidoc (B) (if you wish to run `make docs`)

### How to build

To build, make sure you have the dependencies above:

``` bash
# Make the main program
$ make
# Make the newer man pages
$ make docs
# Install
$ make install
```

(Compton does include a `_CMakeLists.txt` in the tree, but we haven't decided whether we should switch to CMake yet. The `Makefile` is fully usable right now.)

## Example Usage

``` bash
$ compton -cC -i 0.6 -e 0.6 -f
$ compton --config ~/compton.conf
```

### Options and Configuration

```
compton [-d display] [-r radius] [-o opacity]
        [-l left-offset] [-t top-offset]
        [-i opacity] [-e opacity] [-cCfFSdG]
        [--config path] [--shadow-red value]
        [--shadow-green value] [--shadow-blue value]
        [--inactive-opacity-override] [--inactive-dim value]
        [--mark-wmwin-focused] [--shadow-exclude condition]
        [--mark-ovredir-focused] [--no-fading-openclose]
        [--shadow-ignore-shaped] [--detect-round-corners]
```

* `-d` __display__:
  Which display should be managed.
* `-r` __radius__:
  The blur radius for shadows. (default 12)
* `-o` __opacity__:
  The translucency for shadows. (default .75)
* `-l` __left-offset__:
  The left offset for shadows. (default -15)
* `-t` __top-offset__:
  The top offset for shadows. (default -15)
* `-I` __fade-in-step__:
  Opacity change between steps while fading in. (default 0.028)
* `-O` __fade-out-step__:
  Opacity change between steps while fading out. (default 0.03)
* `-D` __fade-delta-time__:
  The time between steps in a fade in milliseconds. (default 10)
* `-m` __opacity__:
  The opacity for menus. (default 1.0)
* `-c`:
  Enabled client-side shadows on windows.
* `-C`:
  Avoid drawing shadows on dock/panel windows.
* `-z`:
  Zero the part of the shadow's mask behind the window (experimental).
* `-f`:
  Fade windows in/out when opening/closing and when opacity
  changes, unless --no-fading-openclose is used.
* `-F`:
  Equals -f. Deprecated.
* `-i` __opacity__:
  Opacity of inactive windows. (0.1 - 1.0)
* `-e` __opacity__:
  Opacity of window titlebars and borders. (0.1 - 1.0)
* `-G`:
  Don't draw shadows on DND windows
* `-b`:
  Daemonize/background process.
* `-S`:
  Enable synchronous operation (for debugging).
* `--config` __path__:
  Look for configuration file at the path.
* `--shadow-red` __value__:
  Red color value of shadow (0.0 - 1.0, defaults to 0).
* `--shadow-green` __value__:
  Green color value of shadow (0.0 - 1.0, defaults to 0).
* `--shadow-blue` __value__:
  Blue color value of shadow (0.0 - 1.0, defaults to 0).
* `--inactive-opacity-override`:
  Inactive opacity set by -i overrides value of _NET_WM_OPACITY.
* `--inactive-dim` __value__:
  Dim inactive windows. (0.0 - 1.0, defaults to 0)
* `--mark-wmwin-focused`:
  Try to detect WM windows and mark them as active.
* `--shadow-exclude` __condition__:
  Exclude conditions for shadows.
* `--mark-ovredir-focused`:
  Mark over-redirect windows as active.
* `--no-fading-openclose`:
  Do not fade on window open/close.
* `--shadow-ignore-shaped`:
  Do not paint shadows on shaped windows.
* `--detect-rounded-corners`:
  Try to detect windows with rounded corners and don't consider
  them shaped windows.

### Format of a condition:

`condition = <target>:<type>[<flags>]:<pattern>`

`<target>` is one of `"n"` (window name), `"i"` (window class
instance), and `"g"` (window general class)

`<type>` is one of `"e"` (exact match), `"a"` (match anywhere),
`"s"` (match from start), `"w"` (wildcard), and `"p"` (PCRE
regular expressions, if compiled with the support).

`<flags>` could be a series of flags. Currently the only defined
flag is `"i"` (ignore case).

`<pattern>` is the actual pattern string.

### Configuration

A more robust
[sample configuration file](https://raw.github.com/chjj/compton/master/compton.sample.conf)
is available in the repository.

#### Example

~/compton.conf:

```
# Shadows
shadow = true;

# Opacity
inactive-opacity = 0.8;
frame-opacity = 0.7;

# Fades
fading = true;
```

Run with:

``` bash
$ compton --config ~/compton.conf
```

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

See LICENSE for more info.
