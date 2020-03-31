import xcffib.xproto as xproto
import xcffib.randr as randr
import time
import random
import string
def set_window_name(conn, wid, name):
    prop_name = "_NET_WM_NAME"
    prop_name = conn.core.InternAtom(True, len(prop_name), prop_name).reply().atom
    str_type = "STRING"
    str_type = conn.core.InternAtom(True, len(str_type), str_type).reply().atom
    conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, prop_name, str_type, 8, len(name), name).check()

def find_picom_window(conn):
    prop_name = "WM_NAME"
    prop_name = conn.core.InternAtom(True, len(prop_name), prop_name).reply().atom
    setup = conn.get_setup()
    root = setup.roots[0].root
    windows = conn.core.QueryTree(root).reply()

    ext = xproto.xprotoExtension(conn)
    for w in windows.children:
        name = ext.GetProperty(False, w, prop_name, xproto.GetPropertyType.Any, 0, (2 ** 32) - 1).reply()
        if name.value.buf() == b"picom":
            return w

def trigger_root_configure(conn):
    setup = conn.get_setup()
    root = setup.roots[0].root
    # Xorg sends root ConfigureNotify when we add a new mode to an output
    rr = conn(randr.key)
    name = ''.join([random.choice(string.ascii_letters + string.digits) for n in range(0, 32)])
    mode_info = randr.ModeInfo.synthetic(id = 0, width = 1000, height = 1000, dot_clock = 0,
            hsync_start = 0, hsync_end = 0, htotal = 0, hskew = 0, vsync_start = 0, vsync_end = 0,
            vtotal = 0, name_len = len(name), mode_flags = 0)

    reply = rr.CreateMode(root, mode_info, len(name), name).reply()
    mode = reply.mode
    reply = rr.GetScreenResourcesCurrent(root).reply()
    # our xvfb is setup to only have 1 output
    output = reply.outputs[0]
    rr.AddOutputModeChecked(output, mode).check()
    rr.SetCrtcConfig(reply.crtcs[0], reply.timestamp, reply.config_timestamp, 0, 0, mode, randr.Rotation.Rotate_0, 1, [output]).reply()

