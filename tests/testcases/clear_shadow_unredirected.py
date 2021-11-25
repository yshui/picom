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

# making sure disabling shadow while screen is unredirected doesn't cause assertion failure
wid = conn.generate_id()
print("Window id is ", hex(wid))

# Create a window
conn.core.CreateWindowChecked(depth, wid, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()

# Set Window name so it does get a shadow
set_window_name(conn, wid, "YesShadow")

# Map the window
print("mapping")
conn.core.MapWindowChecked(wid).check()

time.sleep(0.5)

# Set fullscreen property, causing screen to be unredirected
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, name_atom, atom_atom, 32, 1, [fs_atom]).check()

time.sleep(0.5)

# Set the Window name so it loses its shadow
print("set new name")
set_window_name(conn, wid, "NoShadow")

# Unmap the window
conn.core.UnmapWindowChecked(wid).check()

time.sleep(0.5)

# Destroy the window
conn.core.DestroyWindowChecked(wid).check()
