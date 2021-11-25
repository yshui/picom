#!/usr/bin/env python3

import xcffib.xproto as xproto
import xcffib
import time
from common import set_window_name, set_window_size_async

conn = xcffib.connect()
setup = conn.get_setup()
root = setup.roots[0].root
visual = setup.roots[0].root_visual
depth = setup.roots[0].root_depth

# issue 394 is caused by a window getting a size update just before destroying leading to a shadow update on destroyed window.
wid = conn.generate_id()
print("Window id is ", hex(wid))

# Create a window
conn.core.CreateWindowChecked(depth, wid, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()

# Set Window name so it doesn't get a shadow
set_window_name(conn, wid, "Test Window")

# Map the window
print("mapping")
conn.core.MapWindowChecked(wid).check()

time.sleep(0.5)

# Resize the window and destroy
print("resize and destroy")
set_window_size_async(conn, wid, 150, 150)
conn.core.DestroyWindowChecked(wid).check()

time.sleep(0.5)
