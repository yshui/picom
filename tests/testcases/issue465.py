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
x = xproto.xprotoExtension(conn)

# issue 465 is triggered when focusing a new window with a shadow-exclude rule for unfocused windows.
wid1 = conn.generate_id()
print("Window 1: ", hex(wid1))
wid2 = conn.generate_id()
print("Window 2: ", hex(wid2))

# Create a window
conn.core.CreateWindowChecked(depth, wid1, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()
conn.core.CreateWindowChecked(depth, wid2, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()

# Set Window name
set_window_name(conn, wid1, "Test window 1")
set_window_name(conn, wid2, "Test window 2")

print("mapping 1")
conn.core.MapWindowChecked(wid1).check()
print("mapping 2")
conn.core.MapWindowChecked(wid2).check()
time.sleep(0.5)

x.SetInputFocusChecked(0, wid1, xproto.Time.CurrentTime).check()
time.sleep(0.5)

# Destroy the windows
conn.core.DestroyWindowChecked(wid1).check()
time.sleep(1)
