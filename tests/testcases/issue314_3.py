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

opacity_100 = [0xffffffff, ]
opacity_80 = [int(0xffffffff * 0.8), ]
opacity_single = [int(0xffffffff * 0.002), ]
opacity_0 = [0, ]

# issue 314 is caused by changing a windows target opacity during its fade-in/-out transition
wid1 = conn.generate_id()
print("Window 1: ", hex(wid1))

atom = "_NET_WM_WINDOW_OPACITY"
opacity_atom = conn.core.InternAtom(False, len(atom), atom).reply().atom

# Create windows
conn.core.CreateWindowChecked(depth, wid1, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()

# Set Window names
set_window_name(conn, wid1, "Test window 1")

# Check updating opacity while FADING windows
print("Mapping window")
conn.core.MapWindowChecked(wid1).check()
time.sleep(1.2)

print("Update opacity while fading out")
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid1, opacity_atom, xproto.Atom.CARDINAL, 32, 1, opacity_single).check()
time.sleep(0.2)
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid1, opacity_atom, xproto.Atom.CARDINAL, 32, 1, opacity_0).check()
time.sleep(1)

print("Change from fading in to fading out")
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid1, opacity_atom, xproto.Atom.CARDINAL, 32, 1, opacity_80).check()
time.sleep(0.5)
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid1, opacity_atom, xproto.Atom.CARDINAL, 32, 1, opacity_0).check()
time.sleep(1)

print("Update opacity while fading in")
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid1, opacity_atom, xproto.Atom.CARDINAL, 32, 1, opacity_80).check()
time.sleep(0.2)
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid1, opacity_atom, xproto.Atom.CARDINAL, 32, 1, opacity_100).check()
time.sleep(1)

print("Change from fading out to fading in")
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid1, opacity_atom, xproto.Atom.CARDINAL, 32, 1, opacity_0).check()
time.sleep(0.5)
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid1, opacity_atom, xproto.Atom.CARDINAL, 32, 1, opacity_80).check()
time.sleep(1)

# Destroy the windows
conn.core.DestroyWindowChecked(wid1).check()
