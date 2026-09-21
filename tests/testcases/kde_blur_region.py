#!/usr/bin/env python3
# Behavioral test for _KDE_NET_WM_BLUR_BEHIND_REGION support.
#
# Drives the full property lifecycle the way a KDE-protocol client would and
# asserts picom's debug log reflects each transition:
#   1. region set BEFORE mapping (the common one-shot case — this is what
#      breaks if the property is only read before the client window binds),
#   2. region replaced while mapped (PropertyNotify path),
#   3. region deleted (revert to the full bounding shape).

import os
import time

import xcffib
import xcffib.xproto as xproto
from common import set_window_name, set_window_state, to_atom

LOG_FILE = os.path.join(os.getcwd(), "log")


def wait_for_log(pattern, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(LOG_FILE) as f:
                if pattern in f.read():
                    return
        except FileNotFoundError:
            pass
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {pattern!r} in picom log")


conn = xcffib.connect()
setup = conn.get_setup()
root = setup.roots[0].root
visual = setup.roots[0].root_visual
depth = setup.roots[0].root_depth

wid = conn.generate_id()
conn.core.CreateWindowChecked(
    depth, wid, root, 0, 0, 200, 200, 0,
    xproto.WindowClass.InputOutput, visual, 0, []).check()
set_window_name(conn, wid, "kde_blur_region_test")
set_window_state(conn, wid, 1)

blur_atom = to_atom(conn, "_KDE_NET_WM_BLUR_BEHIND_REGION")

# 1. Two rects, set before the window is ever mapped.
rects = [0, 0, 100, 50, 0, 150, 100, 50]
conn.core.ChangePropertyChecked(
    xproto.PropMode.Replace, wid, blur_atom, xproto.Atom.CARDINAL,
    32, len(rects), rects).check()
conn.core.MapWindowChecked(wid).check()
wait_for_log("Blur region of window")
wait_for_log("2 rects")

# 2. Replace with a single rect while mapped.
rects = [10, 10, 50, 50]
conn.core.ChangePropertyChecked(
    xproto.PropMode.Replace, wid, blur_atom, xproto.Atom.CARDINAL,
    32, len(rects), rects).check()
wait_for_log("1 rects")

# 3. Delete the property: blur reverts to the full bounding shape.
conn.core.DeletePropertyChecked(wid, blur_atom).check()
wait_for_log("cleared")

conn.core.UnmapWindowChecked(wid).check()
conn.core.DestroyWindowChecked(wid).check()
