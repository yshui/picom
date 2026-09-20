#!/usr/bin/env python3
# Behavioral test for the sync-fence stall rescue.
#
# Run with PICOM_DEBUG=suppress_fence_trigger: picom skips its own fence
# trigger, so `xcb_sync_await_fence` stalls exactly the way it does when the
# NVIDIA driver parks rendering completion while the X server's VT is in the
# background. The rescue must detect the stall, trigger the fence from a
# separate connection, and picom must keep compositing afterwards — before
# the rescue existed, this state hung the event loop forever.

import os
import time

import xcffib
import xcffib.xproto as xproto
from common import set_window_name, set_window_state

LOG_FILE = os.path.join(os.getcwd(), "log")


def log_count(pattern):
    try:
        with open(LOG_FILE) as f:
            return f.read().count(pattern)
    except FileNotFoundError:
        return 0


def wait_for_log_count(pattern, count, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if log_count(pattern) >= count:
            return
        time.sleep(0.1)
    raise AssertionError(
        f"timed out waiting for {count}x {pattern!r} in picom log "
        f"(saw {log_count(pattern)})")


RESCUE = "Triggering the fence from a rescue connection"

conn = xcffib.connect()
setup = conn.get_setup()
root = setup.roots[0].root
visual = setup.roots[0].root_visual
depth = setup.roots[0].root_depth


def make_window(name):
    wid = conn.generate_id()
    conn.core.CreateWindowChecked(
        depth, wid, root, 0, 0, 100, 100, 0,
        xproto.WindowClass.InputOutput, visual, 0, []).check()
    set_window_name(conn, wid, name)
    set_window_state(conn, wid, 1)
    conn.core.MapWindowChecked(wid).check()
    return wid


# First render: the suppressed trigger stalls the await; the rescue must fire
# and complete the frame.
w1 = make_window("fence_stall_rescue_1")
wait_for_log_count("stalled for", 1)
wait_for_log_count(RESCUE, 1)

# The compositor must still be alive and rendering: a second window forces
# more frames, each of which stalls and is rescued again. Requiring a HIGHER
# rescue count proves the event loop survived the first stall rather than
# hanging (the pre-rescue behavior).
w2 = make_window("fence_stall_rescue_2")
first = log_count(RESCUE)
wait_for_log_count(RESCUE, first + 1)

conn.core.UnmapWindowChecked(w2).check()
conn.core.UnmapWindowChecked(w1).check()
conn.core.DestroyWindowChecked(w2).check()
conn.core.DestroyWindowChecked(w1).check()
