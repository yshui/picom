#!/bin/sh

#TODO: Check compton running; Check dbus interface available

if [ -z "$SED" ]; then
  SED="sed"
  command -v gsed > /dev/null && SED="gsed"
fi

# === Get connection parameters ===

dpy=$(echo -n "$DISPLAY" | tr -c '[:alnum:]' _)

if [ -z "$dpy" ]; then
  echo "Cannot find display."
  exit 1;
fi

service="com.github.chjj.compton.${dpy}"
interface="com.github.chjj.compton"
object='/com/github/chjj/compton'
type_win='uint32'
type_enum='uint16'

# === Color Inversion ===

# Ensure we are tracking focus
dbus-send --print-reply --dest="$service" "$object" "${interface}.opts_set" string:track_focus boolean:true &

# Get window ID of currently focused window
focused=$(dbus-send --print-reply --dest="$service" "$object" "${interface}.find_win" string:focused | $SED -n 's/^[[:space:]]*'${type_win}'[[:space:]]*\([[:digit:]]*\).*/\1/p')

if [ -n "$focused" ]; then
  # Get invert_color_force property of the focused window
  color=$(dbus-send --print-reply --dest="$service" "$object" "${interface}.win_get" "${type_win}:${focused}" string:invert_color_force | $SED -n 's/^[[:space:]]*'${type_enum}'[[:space:]]*\([[:digit:]]*\).*/\1/p')
  # Set the window to have inverted color
  case "${color}" in
    0)
      # Set the window to have inverted color
      dbus-send --print-reply --dest="$service" "$object" "${interface}.win_set" "${type_win}:${focused}" string:invert_color_force "${type_enum}:1" &
    ;;
    *) # Some windows have invert_color_force == 2
      # Set the window to have normal color
      dbus-send --print-reply --dest="$service" "$object" "${interface}.win_set" "${type_win}:${focused}" string:invert_color_force "${type_enum}:0" &
    ;;
  esac
else
  echo "Cannot find focused window."
  exit 1;
fi
exit 0;
