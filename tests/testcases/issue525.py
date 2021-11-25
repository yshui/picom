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

# issue 525 happens when a window is unmapped with pixmap stale flag set
wid = conn.generate_id()
print("Window id is ", hex(wid))

# Create a window
conn.core.CreateWindowChecked(depth, wid, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()

# Map the window
print("mapping")
conn.core.MapWindowChecked(wid).check()

time.sleep(0.5)

# change window size, invalidate the pixmap
conn.core.ConfigureWindow(wid, xproto.ConfigWindow.X | xproto.ConfigWindow.Width, [100, 200])

# unmap the window immediately after
conn.core.UnmapWindowChecked(wid).check()

time.sleep(0.1)

# Destroy the window
conn.core.DestroyWindowChecked(wid).check()
