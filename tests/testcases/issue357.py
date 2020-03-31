#!/usr/bin/env python

import xcffib.xproto as xproto
import xcffib
import time
from common import set_window_name, trigger_root_configure

conn = xcffib.connect()
setup = conn.get_setup()
root = setup.roots[0].root
visual = setup.roots[0].root_visual
depth = setup.roots[0].root_depth

# issue 239 is caused by a window gaining a shadow during its fade-out transition
wid = conn.generate_id()
print("Window 1: ", hex(wid))

# Create a window
conn.core.CreateWindowChecked(depth, wid, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()

# Set Window name so it doesn't get a shadow
set_window_name(conn, wid, "Test window 1")

# Map the window, causing picom to unredirect
print("mapping 1")
conn.core.MapWindowChecked(wid).check()
time.sleep(0.5)

trigger_root_configure(conn)

# Destroy the windows
conn.core.DestroyWindowChecked(wid).check()

time.sleep(1)
