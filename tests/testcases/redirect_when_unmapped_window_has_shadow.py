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

name = "_NET_WM_STATE"
name_atom = conn.core.InternAtom(False, len(name), name).reply().atom
atom = "ATOM"
atom_atom = conn.core.InternAtom(False, len(atom), atom).reply().atom
fs = "_NET_WM_STATE_FULLSCREEN"
fs_atom = conn.core.InternAtom(False, len(fs), fs).reply().atom

wid1 = conn.generate_id()
print("Window 1 id is ", hex(wid1))

# Create a window
conn.core.CreateWindowChecked(depth, wid1, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()

# Map the window
print("mapping 1")
conn.core.MapWindowChecked(wid1).check()

time.sleep(0.5)

print("unmapping 1")
# Unmap the window
conn.core.UnmapWindowChecked(wid1).check()

time.sleep(0.5)

# create and map a second window
wid2 = conn.generate_id()
print("Window 2 id is ", hex(wid2))
conn.core.CreateWindowChecked(depth, wid2, root, 200, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()
print("mapping 2")
conn.core.MapWindowChecked(wid2).check()
time.sleep(0.5)

# Set fullscreen property on the second window, causing screen to be unredirected
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid2, name_atom, atom_atom, 32, 1, [fs_atom]).check()

time.sleep(0.5)

# Unset fullscreen property on the second window, causing screen to be redirected
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid2, name_atom, atom_atom, 32, 0, []).check()

time.sleep(0.5)

# map the first window again
print("mapping 1")
conn.core.MapWindowChecked(wid1).check()

time.sleep(0.5)

# Destroy the windows
conn.core.DestroyWindowChecked(wid1).check()
conn.core.DestroyWindowChecked(wid2).check()
