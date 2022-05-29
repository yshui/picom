#!/usr/bin/env python3

import xcffib.xproto as xproto
import xcffib
import time
from common import set_window_name, trigger_root_configure, prepare_root_configure

conn = xcffib.connect()
setup = conn.get_setup()
root = setup.roots[0].root
visual = setup.roots[0].root_visual
depth = setup.roots[0].root_depth

# issue 357 is triggered when a window is destroyed right after configure_root
wid = conn.generate_id()
print("Window 1: ", hex(wid))

# Create a window
conn.core.CreateWindowChecked(depth, wid, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()

# Set Window name
set_window_name(conn, wid, "Test window 1")

print("mapping 1")
conn.core.MapWindowChecked(wid).check()
time.sleep(0.5)

reply, mode, output = prepare_root_configure(conn)
trigger_root_configure(conn, reply, mode, output).reply()

# Destroy the windows
conn.core.DestroyWindowChecked(wid).check()

time.sleep(1)
