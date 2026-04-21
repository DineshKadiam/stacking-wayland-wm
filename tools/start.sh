#!/bin/sh

set -xv

# Keyboard layout can be set by putting this in ~/.profile or similar.  This
# cannot be set here, due to the fact that river needs this in the environment
# before it starts.
#   XKB_DEFAULT_LAYOUT=gb
#   export XKB_DEFAULT_LAYOUT

# Start cow.
cow &
sleep 1

CONFIG_PATH=${XDG_CONFIG_HOME:-$HOME/.config}

echo "CONFIG_PATH: $CONFIG_PATH"

# If the user has created a after-start.sh file, use it.
[ -f "${CONFIG_PATH}/cow/after-start.sh" ] && {
    . "${CONFIG_PATH}/cow/after-start.sh"
}
