# Overview of Commands

# Window

## winops

This operates on a window such that:

* `-t` the target window.
* `-I` identifies a window.
* `-c` closes a window.
* `-r` raises a window.
* `-l` lowers a window.
* `-s` makes a window sticky (toggles)
* `-i` iconifies a window (toggles)

## window-expand

This expands a window, such at:

* `-t` the target window.
* `[-d direction]` N, S, E, W.  No direction given assumes all available
  space.
* `-m` Maximizes the window
* `-M` Unmaximizes the window
* `-T` Toggles maximize/unmaximize
* `-k` Maximize the window, but don't change its geometry.

## window-shade

This shades a window in a given direction.

* `-t` the target window.
* `[-d direction]` N, S, E, W.  No direction given assumes `north` by default.

## desk

* `-t` target output
* `-n` next desk
* `-p` prev desk
* `-n` specific desk (0-9)
* `-c` collect desk (0-9)

## window-move

* `-t` taget window
* `-o` output name
* `-c` centre window
* `-n` snap
* `-d` select desktop to move window to
* `-x` X position on screen.
* `-y` Y position on screen.

