#!/usr/bin/env python

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


# Map the window
conn.core.MapWindowChecked(wid).check()

time.sleep(0.5)

conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, name_atom, atom_atom, 32, 1, [fs_atom]).check()

time.sleep(0.5)

conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, name_atom, atom_atom, 32, 0, []).check()

conn.core.GetInputFocus().reply()

# Unmap the window
conn.core.UnmapWindowChecked(wid).check()

time.sleep(0.5)

# Destroy the window
conn.core.DestroyWindowChecked(wid).check()
