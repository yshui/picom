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

The above features give compton a feature set similar to the xfce compositor.

Compton has only been tested with openbox so far, but frame transparency
should work with any window manager that properly sets `_NET_FRAME_EXTENTS`.

## Fixes from the original xcompmgr:

* fixed a segfault when opening certain window types
* fixed a memory leak caused by not freeing up shadows (from the freedesktop
  repo)

## Building

The same dependencies as xcompmgr.

### Dependencies:

* libx11
* libxcomposite
* libxdamage
* libxfixes
* libxrender
* pkg-config
* make

To build, make sure you have the above dependencies:

``` bash
$ make
$ make install
```

## Usage

``` bash
$ compton -cC -i 0.6 -e 0.6
$ compton -cC -fF -I 0.065 -O 0.065 -D 6 -m 0.8 -i 0.6 -e 0.6
```

### Options

    compton [-d display] [-r radius] [-o opacity]
            [-l left-offset] [-t top-offset]
            [-i opacity] [-e opacity] [-cCfFS]

* `-d` display
  Specifies the display to manage.
* `-r` radius
  Specifies the blur radius for client-side shadows.
* `-o` opacity
  Specifies the opacity for client-side shadows.
* `-l` left-offset
  Specifies the left offset for client-side shadows.
* `-t` top-offset
  Specifies the top offset for client-side shadows.
* `-I` fade-in-step
  Specifies the opacity change between steps while fading in.
* `-O` fade-out-step
  Specifies the opacity change between steps while fading out.
* `-D` fade-delta
  Specifies the time (in milliseconds) between steps in a fade.
* `-c`
  Enable client-side shadows on windows.
* `-f`
  When -c is specified, enables a smooth fade effect for transient windows like
  menus, and for all windows on hide and restore events.
* `-C`
  When -c is specified, attempts to avoid painting shadows on panels and docks.
* `-F`
  When -f is specified, also enables the fade effect when windows change their
  opacity, as with transset(1).
* `-i` opacity
  Specifies inactive window transparency. (0.1 - 1.0)
* `-e` opacity
  Specifies window frame transparency. (0.1 - 1.0)
* `-S`
  Enables synchronous operation.  Useful for debugging.

## License

xcompmgr has gotten around. As far as I can tell, the lineage for this
particular tree is something like:

* Keith Packard (original author)
* Matthew Hawn
* ...
* Dana Jansens
* Myself

Not counting the tens of people who forked it in between.

See LICENSE for more info.
