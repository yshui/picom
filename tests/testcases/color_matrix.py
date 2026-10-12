#!/usr/bin/env python3
# The screen color matrix (_PICOM_COLOR_MATRIX on the root window). With
# unredir-if-possible and only a fullscreen window, the screen starts
# unredirected; then
#   1. setting a matrix redirects it, so the matrix reaches every pixel,
#   2. a malformed matrix counts as the identity, and unredirects it again,
#   3. deleting the property goes back to the identity too.

import os
import time

import xcffib
import xcffib.xproto as xproto
from common import set_window_name, to_atom

LOG_FILE = os.path.join(os.getcwd(), "log")


def wait_for_log(pattern, count, timeout=5.0):
    """Wait until picom's debug log has printed `pattern` `count` times."""
    deadline = time.time() + timeout
    seen = 0
    while time.time() < deadline:
        try:
            with open(LOG_FILE) as f:
                seen = f.read().count(pattern)
            if seen >= count:
                return
        except FileNotFoundError:
            pass
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {pattern!r} x{count} in picom log (seen {seen})")


conn = xcffib.connect()
setup = conn.get_setup()
root = setup.roots[0].root
visual = setup.roots[0].root_visual
depth = setup.roots[0].root_depth

matrix_atom = to_atom(conn, "_PICOM_COLOR_MATRIX")
state_atom = to_atom(conn, "_NET_WM_STATE")
fullscreen_atom = to_atom(conn, "_NET_WM_STATE_FULLSCREEN")

# Red luminance at 40%, in 16.16 fixed point.
red = [7838, 15388, 2988, 0, 0, 0, 0, 0, 0]


def set_matrix(values):
    conn.core.ChangePropertyChecked(xproto.PropMode.Replace, root, matrix_atom,
                                    xproto.Atom.INTEGER, 32, len(values), values).check()


wid = conn.generate_id()
# A background, so the server paints it and picom gets damage for it: a window
# that was never damaged can't unredirect the screen.
conn.core.CreateWindowChecked(depth, wid, root, 0, 0, 100, 100, 0,
                              xproto.WindowClass.InputOutput, visual, xproto.CW.BackPixel,
                              [setup.roots[0].black_pixel]).check()
set_window_name(conn, wid, "color_matrix_test")
conn.core.MapWindowChecked(wid).check()
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, state_atom, xproto.Atom.ATOM,
                                32, 1, [fullscreen_atom]).check()
time.sleep(0.5)

# 1. A matrix redirects the screen.
set_matrix(red)
wait_for_log("Color matrix changed: not the identity", 1)
wait_for_log("Screen redirected.", 1)

# 2. Eight items are not a matrix: the identity, so unredirected again.
set_matrix(red[:8])
wait_for_log("Color matrix changed: identity", 1)
wait_for_log("Screen unredirected.", 1)

# 3. Deleting the property is the identity too.
set_matrix(red)
wait_for_log("Color matrix changed: not the identity", 2)
wait_for_log("Screen redirected.", 2)
conn.core.DeletePropertyChecked(root, matrix_atom).check()
wait_for_log("Color matrix changed: identity", 2)
wait_for_log("Screen unredirected.", 2)

conn.core.UnmapWindowChecked(wid).check()
conn.core.DestroyWindowChecked(wid).check()
