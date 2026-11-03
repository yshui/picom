#!/usr/bin/env python3

# Regression test: a damage region made of disjoint rectangles must not repaint
# the undamaged area between them.
#
# xrender_copy_area used to clip only the *source* picture and composite over the
# bounding box of the region. The X server's software (fb) Render path, used by
# Xvfb and by Xorg without glamor, ignores a source clip and writes every
# destination pixel of that box, so an undamaged window lying between two damaged
# rectangles was overwritten with the root background. With vsync disabled the
# xrender backend copies straight into the target window, so the damage is
# visible on screen.

import time
import xcffib
import xcffib.xproto as xproto

conn = xcffib.connect()
setup = conn.get_setup()
screen = setup.roots[0]
root = screen.root
visual = screen.root_visual
depth = screen.root_depth
W, H = screen.width_in_pixels, screen.height_in_pixels


def make_window(x, y, w, h, pixel):
    wid = conn.generate_id()
    conn.core.CreateWindowChecked(depth, wid, root, x, y, w, h, 0,
                                  xproto.WindowClass.InputOutput, visual,
                                  xproto.CW.BackPixel, [pixel]).check()
    conn.core.MapWindowChecked(wid).check()
    return wid


def screen_pixel(x, y):
    reply = conn.core.GetImage(xproto.ImageFormat.ZPixmap, root, x, y, 1, 1,
                               0xffffffff).reply()
    return bytes(reply.data.buf())


white = screen.white_pixel
black = screen.black_pixel

# Give the root a background pixmap of its own colour. picom then repaints damaged
# areas by copying this root image into its render buffer, which is the copy that
# used to spill over the bounding box of a disjoint region.
gray = conn.core.AllocColor(screen.default_colormap, 0x8000, 0x8000, 0x8000).reply().pixel
root_pixmap = conn.generate_id()
conn.core.CreatePixmapChecked(depth, root_pixmap, root, W, H).check()
gc = conn.generate_id()
conn.core.CreateGCChecked(gc, root_pixmap, xproto.GC.Foreground, [gray]).check()
conn.core.PolyFillRectangleChecked(root_pixmap, gc, 1,
                                   [xproto.RECTANGLE.synthetic(0, 0, W, H)]).check()
xrootpmap = conn.core.InternAtom(False, len("_XROOTPMAP_ID"), "_XROOTPMAP_ID").reply().atom
conn.core.ChangePropertyChecked(xproto.PropMode.Replace, root, xrootpmap,
                                xproto.Atom.PIXMAP, 32, 1, [root_pixmap]).check()

# Undamaged window in the middle of the screen, and two small windows in
# opposite corners whose repaints form a disjoint damage region whose bounding
# box covers the middle window.
cx, cy = W // 2, H // 2
middle = make_window(cx - 50, cy - 50, 100, 100, white)
time.sleep(1)

before = screen_pixel(cx, cy)
background = screen_pixel(W // 4, H // 2)
print("middle window pixel", before.hex(), "background pixel", background.hex())
if before == background:
    raise SystemExit("FAIL: middle window is not composited, test is inconclusive")


def check(step):
    after = screen_pixel(cx, cy)
    if after != before:
        raise SystemExit("FAIL: undamaged window between disjoint damage rectangles "
                         f"was overwritten after {step}: {before.hex()} -> {after.hex()}")


# Semi-transparent corners: picom must repaint the root background beneath them,
# so every corner repaint copies the root image over a disjoint region.
opacity = conn.core.InternAtom(False, len("_NET_WM_WINDOW_OPACITY"),
                               "_NET_WM_WINDOW_OPACITY").reply().atom
corners = []
for x, y in ((0, 0), (W - 20, H - 20)):
    wid = conn.generate_id()
    conn.core.CreateWindowChecked(depth, wid, root, x, y, 20, 20, 0,
                                  xproto.WindowClass.InputOutput, visual,
                                  xproto.CW.BackPixel, [black]).check()
    conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, opacity,
                                    xproto.Atom.CARDINAL, 32, 1, [0x80000000]).check()
    corners.append(wid)
for wid in corners:
    conn.core.MapWindow(wid)
conn.flush()
time.sleep(0.5)
check("mapping the corner windows")

for i in range(10):
    pixel = white if i % 2 == 0 else black
    for wid in corners:
        conn.core.ChangeWindowAttributes(wid, xproto.CW.BackPixel, [pixel])
        conn.core.ClearArea(False, wid, 0, 0, 0, 0)
    conn.flush()
    time.sleep(0.1)
    check(f"corner repaint {i}")

print("PASS: undamaged window preserved")
