# Overview of Commands

# Window

## winops

This operates on a window such that:

* `-t` the target window.
* `-I` identifies a window.
* `-c` closes a window.
* `-r` raises a window (within its layer).
* `-l` lowers a window (within its layer).
* `-s` makes a window sticky (toggles)
* `-i` iconifies a window (toggles)
* `-L layer` sets the window's layer.  Accepts `N`, `=N`, `+N`,
  `-N`, `top`, `bottom`, `=top`, `=bottom`.  `top`/`bottom` toggle
  back to `default_layer` when already at the endpoint; `=top` /
  `=bottom` always set (useful inside rules).

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

## focus

* `-n` / `-p` -- next / previous focusable window.
* `-d dir` -- nearest focusable in direction `up|down|left|right`.
* `-o output` -- warp to a named output.
* `-t %app|/title/|#id` -- selector.
* `-L` -- restrict `-n|-p|-d` to the focused view's layer.
* `-l N` -- target layer `N`.  Standalone: focuses the topmost
  focusable in `N`, or cycles within `N` when already there.
  Combined with `-n|-p`: traverses only within `N`.

## set

Layer-related keys:

* `default_layer` (uint, default 4) -- layer assigned to newly
  mapped windows.
* `max_layer` (uint, default 16) -- cap for `winops -L`.  Lowering
  the cap clamps every existing view's layer.

## script

Evaluate an embedded `script { ... }` block.  The block may use
`let` bindings, `for` loops over the named sources (`windows`,
`desks`, `outputs`, `rules`) with an optional `where` filter, and
`if` / `else` conditionals; any cow command inside the block is
invoked with the rest of its argv (with `#{expr}` interpolations
expanded).  See the SCRIPTING section in cow(1) for full grammar
and examples.
