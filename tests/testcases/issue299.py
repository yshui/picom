#!/usr/bin/env python3

import xcffib.xproto as xproto
import xcffib
import time
import os
import subprocess
import asyncio
from dbus_next.aio import MessageBus
from dbus_next.message import Message, MessageType
from common import *

display = os.environ["DISPLAY"].replace(":", "_")
conn = xcffib.connect()
setup = conn.get_setup()
root = setup.roots[0].root
visual = setup.roots[0].root_visual
depth = setup.roots[0].root_depth
x = xproto.xprotoExtension(conn)
visual32 = find_32bit_visual(conn)

async def get_client_win_async(wid):
    message = await bus.call(Message(destination='com.github.chjj.compton.'+display,
        path='/com/github/chjj/compton',
        interface='com.github.chjj.compton',
        member='win_get',
        signature='us',
        body=[wid, 'client_win']))
    return message.body[0]

def get_client_win(wid):
    return loop.run_until_complete(get_client_win_async(wid))

def wait():
    time.sleep(0.5)

def create_client_window(name):
    client_win = conn.generate_id()
    print("Window : ", hex(client_win))
    conn.core.CreateWindowChecked(depth, client_win, root, 0, 0, 100, 100, 0,
        xproto.WindowClass.InputOutput, visual, 0, []).check()
    set_window_name(conn, client_win, "Test window "+name)
    set_window_class(conn, client_win, "Test windows")
    set_window_state(conn, client_win, 1)
    conn.core.MapWindowChecked(client_win).check()
    return client_win

loop = asyncio.get_event_loop()
bus = loop.run_until_complete(MessageBus().connect())

cmid = conn.generate_id()
colormap = conn.core.CreateColormapChecked(xproto.ColormapAlloc._None, cmid, root, visual32).check()

# Create window
client_wins = []
for i in range(0,2):
    client_wins.append(create_client_window(str(i)))

# Create frame window
frame_win = conn.generate_id()
print("Window : ", hex(frame_win))
conn.core.CreateWindowChecked(depth, frame_win, root, 0, 0, 200, 200, 0,
    xproto.WindowClass.InputOutput, visual, 0, []).check()
set_window_name(conn, frame_win, "Frame")
conn.core.MapWindowChecked(frame_win).check()

# Scenario 1.1
# 1. reparent placeholder to frame
conn.core.ReparentWindowChecked(client_wins[0], frame_win, 0, 0).check()
wait()
# 2. reparent real client to frame
conn.core.ReparentWindowChecked(client_wins[1], frame_win, 0, 0).check()
wait()
# 3. detach the placeholder
conn.core.ReparentWindowChecked(client_wins[0], root, 0, 0).check()
wait()
assert get_client_win(frame_win) == client_wins[1]

# Scenario 1.2
# 1. reparent placeholder to frame
conn.core.ReparentWindowChecked(client_wins[0], frame_win, 0, 0).check()
wait()
# 2. reparent real client to frame
conn.core.ReparentWindowChecked(client_wins[1], frame_win, 0, 0).check()
wait()
# 3. destroy the placeholder
conn.core.DestroyWindowChecked(client_wins[0]).check()
wait()
assert get_client_win(frame_win) == client_wins[1]

client_wins[0] = create_client_window("0")

# Scenario 2
# 1. frame is unmapped
conn.core.UnmapWindowChecked(frame_win).check()
wait()
# 2. reparent placeholder to frame
conn.core.ReparentWindowChecked(client_wins[0], frame_win, 0, 0).check()
wait()
# 3. destroy placeholder, map frame and reparent real client to frame
conn.core.DestroyWindowChecked(client_wins[0]).check()
conn.core.MapWindowChecked(frame_win).check()
conn.core.ReparentWindowChecked(client_wins[1], frame_win, 0, 0).check()
wait()
assert get_client_win(frame_win) == client_wins[1]

client_wins[0] = create_client_window("0")

# Destroy the windows
for wid in client_wins:
    conn.core.DestroyWindowChecked(wid).check()
conn.core.DestroyWindowChecked(frame_win).check()
