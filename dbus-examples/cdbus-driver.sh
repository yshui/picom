#!/bin/sh

if [ -z "$SED" ]; then
  SED="sed"
  command -v gsed > /dev/null && SED="gsed"
fi

# === Get connection parameters ===

dpy=$(echo -n "$DISPLAY" | tr -c '[:alnum:]' _)

if [ -z "$dpy" ]; then
  echo "Cannot find display."
  exit 1
fi

service="com.github.chjj.compton.${dpy}"
interface='com.github.chjj.compton'
object='/com/github/chjj/compton'
type_win='uint32'
type_enum='uint32'

# === DBus methods ===

# List all window ID compton manages (except destroyed ones)
dbus-send --print-reply --dest="$service" "$object" "${interface}.list_win"

# Get window ID of currently focused window
focused=$(dbus-send --print-reply --dest="$service" "$object" "${interface}.find_win" string:focused | $SED -n 's/^[[:space:]]*'${type_win}'[[:space:]]*\([[:digit:]]*\).*/\1/p')

if [ -n "$focused" ]; then
  # Get invert_color_force property of the window
  dbus-send --print-reply --dest="$service" "$object" "${interface}.win_get" "${type_win}:${focused}" string:invert_color_force

  # Set the window to have inverted color
  dbus-send --print-reply --dest="$service" "$object" "${interface}.win_set" "${type_win}:${focused}" string:invert_color_force "${type_enum}:1"
else
  echo "Cannot find focused window."
fi

# Reset compton
sleep 3
dbus-send --print-reply --dest="$service" "$object" "${interface}.reset"

# Undirect window
sleep 3
dbus-send --print-reply --dest="$service" "$object" "${interface}.opts_set" string:redirected_force "${type_enum}:0"

# Revert back to auto
sleep 3
dbus-send --print-reply --dest="$service" "$object" "${interface}.opts_set" string:redirected_force "${type_enum}:2"

# Force repaint
dbus-send --print-reply --dest="$service" "$object" "${interface}.repaint"
