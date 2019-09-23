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
print("Window id is ", hex(wid))

# Create a window
conn.core.CreateWindowChecked(depth, wid, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()

# Set Window name so it doesn't get a shadow
name = "_NET_WM_NAME"
name_atom = conn.core.InternAtom(True, len(name), name).reply().atom
str_type = "STRING"
str_type_atom = conn.core.InternAtom(True, len(str_type), str_type).reply().atom

win_name = "NoShadow"
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, name_atom, str_type_atom, 8, len(win_name), win_name).check()

# Map the window
print("mapping")
conn.core.MapWindowChecked(wid).check()

time.sleep(0.5)

# Set the Window name so it gets a shadow
print("set new name")
win_name = "YesShadow"
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, name_atom, str_type_atom, 8, len(win_name), win_name).check()

# Unmap the window
conn.core.UnmapWindowChecked(wid).check()

time.sleep(0.5)

# Destroy the window
conn.core.DestroyWindowChecked(wid).check()
