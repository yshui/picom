#!/bin/sh

# === Verify `compton --dbus` status ===

if [ -z "`dbus-send --session --dest=org.freedesktop.DBus --type=method_call --print-reply /org/freedesktop/DBus org.freedesktop.DBus.ListNames | grep compton`" ]; then
  echo "compton DBus interface unavailable"
  if [ -n "`pgrep compton`" ]; then
    echo "compton running without dbus interface"
    #killall compton & # Causes all windows to flicker away and come back ugly.
    #compton --dbus & # Causes all windows to flicker away and come back beautiful
  else
    echo "compton not running"
  fi
  exit 1;
fi

# === Setup sed ===

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
compton_dbus="dbus-send --print-reply --dest="${service}" / "${interface}"."
type_win='uint32'
type_enum='uint32'

# === Color Inversion ===

# Get window ID of window to invert
if [ -z "$1" -o "$1" = "selected" ]; then
  window=$(xwininfo -frame | sed -n 's/^xwininfo: Window id: \(0x[[:xdigit:]][[:xdigit:]]*\).*/\1/p') # Select window by mouse
elif [ "$1" = "focused" ]; then
  # Ensure we are tracking focus
  window=$(${compton_dbus}find_win string:focused | $SED -n 's/^[[:space:]]*'${type_win}'[[:space:]]*\([[:digit:]]*\).*/\1/p') # Query compton for the active window
elif echo "$1" | grep -Eiq '^([[:digit:]][[:digit:]]*|0x[[:xdigit:]][[:xdigit:]]*)$'; then
  window="$1" # Accept user-specified window-id if the format is correct
else
  echo "$0" "[ selected | focused | window-id ]"
fi

# Color invert the selected or focused window
if [ -n "$window" ]; then
  invert_status="$(${compton_dbus}win_get "${type_win}:${window}" string:invert_color | $SED -n 's/^[[:space:]]*boolean[[:space:]]*\([[:alpha:]]*\).*/\1/p')"
  if [ "$invert_status" = true ]; then
    invert=0 # Set the window to have normal color
  else
    invert=1 # Set the window to have inverted color
  fi
  ${compton_dbus}win_set "${type_win}:${window}" string:invert_color_force "${type_enum}:${invert}" &
else
  echo "Cannot find $1 window."
  exit 1;
fi
exit 0;
