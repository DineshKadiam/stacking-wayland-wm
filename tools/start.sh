#!/bin/sh

set +xv

# Keyboard layout can be set by putting this in ~/.profile or similar.  This
# cannot be set here, due to the fact that river needs this in the environment
# before it starts.
#   XKB_DEFAULT_LAYOUT=gb
#   export XKB_DEFAULT_LAYOUT

# Start cow.
cow &
sleep 1

# Configure monitors in the correct order.  The program `wdisplays` is good
# for this, and behaves similar to xrandr.
wlr-randr --output DP-7 --pos 0,0
wlr-randr --output DP-10 --pos 1920,0
wlr-randr --output DP-12 --pos 3840,0
wlr-randr --output DVI-I-1 --pos 3840,1080
wlr-randr --output DP-6 --pos 1920,1080
wlr-randr --output DVI-I-2 --pos 0,1080
wlr-randr --output DVI-I-3 --pos 1920,2160
