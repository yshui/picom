#!/bin/sh

# == Declare stderr function ===

stderr() {
  printf "\033[1;31m%s\n\033[0m" "$@" >&2
}

# === Verify `picom --dbus` status ===

if [ -z "$(dbus-send --session --dest=org.freedesktop.DBus --type=method_call --print-reply /org/freedesktop/DBus org.freedesktop.DBus.ListNames | grep compton)" ]; then
  stderr "picom DBus interface unavailable"
  if [ -n "$(pgrep picom)" ]; then
    stderr "picom running without dbus interface"
    #killall picom & # Causes all windows to flicker away and come back ugly.
    #picom --dbus & # Causes all windows to flicker away and come back beautiful
  else
    stderr "picom not running"
  fi
  exit 1
fi

# === Setup sed ===

SED="${SED:-$(command -v gsed || printf 'sed')}"

# === Get connection parameters ===

dpy=$(printf "$DISPLAY" | tr -c '[:alnum:]' _)

if [ -z "$dpy" ]; then
  stderr "Cannot find display."
  exit 1
fi

service="com.github.chjj.compton.${dpy}"
interface="com.github.chjj.compton"
picom_dbus="dbus-send --print-reply --dest="${service}" / "${interface}"."
type_win='uint32'
type_enum='uint32'

# === Color Inversion ===

# Get window ID of window to invert
if [ -z "$1" -o "$1" = "selected" ]; then
  window=$(xwininfo -frame | sed -n 's/^xwininfo: Window id: \(0x[[:xdigit:]][[:xdigit:]]*\).*/\1/p') # Select window by mouse
elif [ "$1" = "focused" ]; then
  # Ensure we are tracking focus
  window=$(${picom_dbus}find_win string:focused | $SED -n 's/^[[:space:]]*'${type_win}'[[:space:]]*\([[:digit:]]*\).*/\1/p') # Query picom for the active window
elif echo "$1" | grep -Eiq '^([[:digit:]][[:digit:]]*|0x[[:xdigit:]][[:xdigit:]]*)$'; then
  window="$1" # Accept user-specified window-id if the format is correct
else
  echo "$0" "[ selected | focused | window-id ]"
fi

# Color invert the selected or focused window
if [ -n "$window" ]; then
  invert_status="$(${picom_dbus}win_get "${type_win}:${window}" string:invert_color | $SED -n 's/^[[:space:]]*boolean[[:space:]]*\([[:alpha:]]*\).*/\1/p')"
  if [ "$invert_status" = true ]; then
    invert=0 # Set the window to have normal color
  else
    invert=1 # Set the window to have inverted color
  fi
  ${picom_dbus}win_set "${type_win}:${window}" string:invert_color_force "${type_enum}:${invert}" &
else
  stderr "Cannot find $1 window."
  exit 1
fi
exit 0
