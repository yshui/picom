import xcffib.xproto as xproto
import xcffib.randr as randr
import xcffib
import time
import random
import string

def to_atom(conn, string):
    return conn.core.InternAtom(False, len(string), string).reply().atom

def set_window_name(conn, wid, name):
    prop_name = to_atom(conn, "_NET_WM_NAME")
    str_type = to_atom(conn, "UTF8_STRING")
    conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, prop_name, str_type, 8, len(name), name).check()
    prop_name = to_atom(conn, "WM_NAME")
    str_type = to_atom(conn, "STRING")
    conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, prop_name, str_type, 8, len(name), name).check()

def set_window_state(conn, wid, state):
    prop_name = to_atom(conn, "WM_STATE")
    conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, prop_name, prop_name, 32, 2, [state, 0]).check()

def set_window_class(conn, wid, name):
    if not isinstance(name, bytearray):
        name = name.encode()
    name = name+b"\0"+name+b"\0"
    prop_name = to_atom(conn, "WM_CLASS")
    str_type = to_atom(conn, "STRING")
    conn.core.ChangePropertyChecked(xproto.PropMode.Replace, wid, prop_name, str_type, 8, len(name), name).check()

def set_window_size_async(conn, wid, width, height):
    value_mask = xproto.ConfigWindow.Width | xproto.ConfigWindow.Height
    value_list = [width, height]
    return conn.core.ConfigureWindowChecked(wid, value_mask, value_list)

def find_picom_window(conn):
    prop_name = to_atom(conn, "WM_NAME")
    setup = conn.get_setup()
    root = setup.roots[0].root
    windows = conn.core.QueryTree(root).reply()

    ext = xproto.xprotoExtension(conn)
    for w in windows.children:
        name = ext.GetProperty(False, w, prop_name, xproto.GetPropertyType.Any, 0, (2 ** 32) - 1).reply()
        if name.value.buf() == b"picom":
            return w

def prepare_root_configure(conn):
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
    return reply, mode, output

def trigger_root_configure(conn, reply, mode, output):
    rr = conn(randr.key)
    return rr.SetCrtcConfig(reply.crtcs[0], reply.timestamp, reply.config_timestamp, 0, 0, mode, randr.Rotation.Rotate_0, 1, [output])

def find_32bit_visual(conn):
    setup = conn.get_setup()
    render = conn(xcffib.render.key)
    r = render.QueryPictFormats().reply()
    pictfmt_ids = set()
    for pictform in r.formats:
        if (pictform.depth == 32 and
            pictform.type == xcffib.render.PictType.Direct and
            pictform.direct.alpha_mask != 0):
            pictfmt_ids.add(pictform.id)
    print(pictfmt_ids)
    for screen in r.screens:
        for depth in screen.depths:
            for pv in depth.visuals:
                if pv.format in pictfmt_ids:
                    return pv.visual
