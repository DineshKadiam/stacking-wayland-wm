#!/bin/sh

# Commands in this schell script run once cow has started.
#
# Useful to reconfigure monitors, etc.

# Configure monitors in the correct order.  The program `wdisplays` is good
# for this, and behaves similar to xrandr.
wlr-randr --output DP-7 --pos 0,0
wlr-randr --output DP-10 --pos 1920,0
wlr-randr --output DP-12 --pos 3840,0
wlr-randr --output DVI-I-1 --pos 3840,1080
wlr-randr --output DP-6 --pos 1920,1080
wlr-randr --output DVI-I-2 --pos 0,1080
wlr-randr --output DVI-I-3 --pos 1920,2160
