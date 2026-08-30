#!/bin/sh
# cow-window-switch -- switch focus to a window chosen via a dmenu-style picker.
#
# Usage: cow-window-switch [picker]
#   picker defaults to rofi.
#
# Requires: moocow, jq, and a dmenu-compatible picker.

PICKER="${1:-}"
if [ -z "$PICKER" ]; then
    if command -v fuzzel >/dev/null 2>&1; then
        PICKER="rofi -dmenu"
    elif command -v bemenu >/dev/null 2>&1; then
        PICKER="bemenu"
    elif command -v dmenu >/dev/null 2>&1; then
        PICKER="dmenu"
    else
        echo "cow-window-switch: no picker found (install fuzzel or bemenu)" >&2
        exit 1
    fi
fi

# Build a list: "desk  app_id  title<TAB>id"
# Present the human-readable part to the picker, recover the id from the tab.
sel=$(moocow show -a window \
    | jq -r '.data[] | select(.iconified == false) | "\(.desk)  \(.app_id)  \(.title)\t\(.id)"' \
    | $PICKER \
    | cut -f2)

# Focus the window selected, warping the pointer.
[ -n "$sel" ] && moocow focus -i "$sel" -w
