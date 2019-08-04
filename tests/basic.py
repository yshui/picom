#!/usr/bin/env python

import xcffib.xproto as xproto
import xcffib

conn = xcffib.connect()
setup = conn.get_setup()
root = setup.roots[0].root
visual = setup.roots[0].root_visual
depth = setup.roots[0].root_depth

wid = conn.generate_id()
conn.core.CreateWindowChecked(depth, wid, root, 0, 0, 100, 100, 0, xproto.WindowClass.InputOutput, visual, 0, []).check()
conn.core.MapWindowChecked(wid).check()
conn.core.UnmapWindowChecked(wid).check()
conn.core.DestroyWindowChecked(wid).check()


