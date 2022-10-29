#!/usr/bin/env python3

import xcffib.xproto as xproto
import xcffib
import time

conn = xcffib.connect()
setup = conn.get_setup()
root = setup.roots[0].root
visual = setup.roots[0].root_visual
depth = setup.roots[0].root_depth

# issue 239 is caused by a window gaining a shadow during its fade-out transition
wid = conn.generate_id()
print("Window ids are ", hex(wid))

# Create a window
mask = xproto.CW.BackPixel
value = [ setup.roots[0].white_pixel ]
conn.core.CreateWindowChecked(depth, wid, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, mask, value).check()

name = "_NET_WM_STATE"
name_atom = conn.core.InternAtom(False, len(name), name).reply().atom
atom = "ATOM"
atom_atom = conn.core.InternAtom(False, len(atom), atom).reply().atom
fs = "_NET_WM_STATE_FULLSCREEN"
fs_atom = conn.core.InternAtom(False, len(fs), fs).reply().atom


# Map the window, causing screen to be redirected
conn.core.MapWindowChecked(wid).check()

time.sleep(0.5)

# Set fullscreen property, causing screen to be unredirected
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, name_atom, atom_atom, 32, 1, [fs_atom]).check()

time.sleep(0.5)

# Clear fullscreen property, causing screen to be redirected
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, name_atom, atom_atom, 32, 0, []).check()

# Do a round trip to X server so the compositor has a chance to start the rerun of _draw_callback
conn.core.GetInputFocus().reply()

# Unmap the window, triggers the bug
conn.core.UnmapWindowChecked(wid).check()

time.sleep(0.5)

# Destroy the window
conn.core.DestroyWindowChecked(wid).check()
