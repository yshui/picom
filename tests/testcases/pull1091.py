#!/usr/bin/env python3

import xcffib.xproto as xproto
import xcffib
from common import *

conn = xcffib.connect()
setup = conn.get_setup()
root = setup.roots[0].root
visual = setup.roots[0].root_visual
depth = setup.roots[0].root_depth

# assertion failure mentioned in 1091 happens when a root change happens right after we
# redirected the screen, before we have even rendered a single frame
wid = conn.generate_id()
print("Window id is ", hex(wid))

# Create a window
conn.core.CreateWindowChecked(depth, wid, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()

# Map the window
print("mapping")
conn.core.MapWindowChecked(wid).check()

time.sleep(0.5)

for i in range(0, 8):
	modes = []
	for s in range(0, 10):
		reply, mode, output = prepare_root_configure(conn, i * 100 + 100 + s)
		modes.append((reply, mode, output))

	set_window_bypass_compositor(conn, wid).check()
	time.sleep(0.1)

	set_window_bypass_compositor(conn, wid, 0)
	conn.flush()
	for reply, mode, output in modes:
		trigger_root_configure(conn, reply, mode, output).reply()

	time.sleep(0.1)
