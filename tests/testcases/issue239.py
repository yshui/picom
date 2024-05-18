#!/usr/bin/env python3

import xcffib.xproto as xproto
import xcffib
import time
from common import set_window_name

conn = xcffib.connect()
setup = conn.get_setup()
root = setup.roots[0].root
visual = setup.roots[0].root_visual
depth = setup.roots[0].root_depth

# issue 239 is caused by a window gaining a shadow during its fade-out transition
wid = conn.generate_id()
print("Window id is ", hex(wid))

# Create a window
conn.core.CreateWindowChecked(depth, wid, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()

# Set Window name so it doesn't get a shadow
set_window_name(conn, wid, "NoShadow")

# Map the window
print("mapping")
conn.core.MapWindowChecked(wid).check()

time.sleep(0.5)

# Set the Window name so it gets a shadow
print("set new name")
set_window_name(conn, wid, "YesShadow")

# Unmap the window
conn.core.UnmapWindowChecked(wid).check()

time.sleep(0.5)

# Destroy the window
conn.core.DestroyWindowChecked(wid).check()
