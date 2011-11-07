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
* shadows are now enabled for argb windows, e.g. terminals with transparency
* removed serverside shadows (and simple compositing) to clean the code,
  the only option that remains is clientside shadows
* menu transparency (thanks to Dana)

The above features give compton a feature set similar to the xfce compositor.

Compton has only been tested with openbox so far, but frame transparency
should work with any window manager that properly sets `_NET_FRAME_EXTENTS`.

## Fixes from the original xcompmgr:

* fixed a segfault when opening certain window types
* fixed a memory leak caused by not freeing up shadows (from the freedesktop
  repo)

## License

xcompmgr has gotten around. As far as I can tell, the lineage for this
particular tree is something like:

* Keith Packard (original author)
* Matthew Hawn
* ...
* Dana Jansens
* Myself

Not counting the tens of people who forked it in between.

Keith Packard's original license remains in the source.

## Building

The same dependencies and build as xcompmgr.

### Dependencies:

* libx11
* libxcomposite
* libxdamage
* libxfixes
* libxrender
* autoconf

To build, make sure you have the above dependencies:

``` bash
$ make
$ make install
```

## Usage

``` bash
$ compton -cC -t -5 -l -5 -r 5 -o 0.4 \
  -fF -I 0.065 -O 0.065 -D 6 -m 0.8 -i 0.6 -e 0.6 &

$ compton -cC -t -5 -l -5 -r 5 -o 0.4 -i 0.6 -e 0.6 &
```
