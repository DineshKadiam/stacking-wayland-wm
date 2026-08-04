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

## window-list

Open the transient keyboard window switcher.  `-F expr` filters candidate
windows, `-w` warps to the selection, and `-f format` controls each menu label.
The format conversions expose the entry number (`%n`), displayed/raw title
(`%t`/`%T`), app ID (`%a`), identifier (`%i`), output (`%o`/`%O`), desk
(`%d`/`%D`), geometry (`%x`, `%y`, `%w`, `%h`), and layer (`%l`).
`-S none|title|app|id|output|desk|location` controls stable list ordering.

## set

Layer-related keys:

* `default_layer` (uint, default 4) -- layer assigned to newly
  mapped windows.
* `max_layer` (uint, default 16) -- cap for `winops -L`.  Lowering
  the cap clamps every existing view's layer.

## script

Evaluate an embedded `script { ... }` block.  The block may use
`let` bindings, `for` loops over the named sources (`windows`,
`desks`, `outputs`/`screens`, `rules`) with an optional `where` filter, and
`if` / `else` conditionals; any cow command inside the block is
invoked with the rest of its argv (with `#{expr}` interpolations
expanded).  See the SCRIPTING section in cow(1) for full grammar
and examples.

## menu-add / menu-clear / menu-destroy / menu-show

Define and operate popup menus.

* `menu-add NAME LABEL ACTION` -- append an item to menu `NAME`
  (creating it on first reference).  `ACTION` of `title` makes a
  non-selectable header; `-` (with `LABEL` also `-`) adds a
  separator; `popup OTHER` links to a sub-menu; `popup-pick OTHER`
  links to a sub-menu whose child actions pick a window first when
  no window context exists; anything else is the cow command run when
  the item is selected.
* `menu-clear NAME` -- empty a menu without destroying it.
* `menu-destroy NAME` -- remove a menu.
* `menu-show NAME [-x N] [-y N]` -- pop up the menu, defaulting to
  the current pointer.  An explicit command queue target becomes the
  menu context for selected item actions.  Use `popup-pick OTHER`
  for root-menu submenus that should stay visible before selecting a
  window target.

An open menu takes keyboard focus and handles arrow keys, Enter, Escape,
Backspace, and alphanumeric hotkeys directly.

See the MENUS section in `cow(1)` for the full grammar, `menu-style`
keys, and a worked example.

`menu-style KEY VALUE` changes the global style.  `menu-style -m NAME KEY
VALUE` stores a per-menu override for any style property, creating the menu on
first reference.  `menu-style -m NAME -r KEY` removes one override.  Unset
properties resolve from the global style; submenus do not inherit their
parent's overrides.

`selection.relief` accepts `raised` (the traditional two-pixel relief around
the selected item, with the menu background retained inside it) or `none` for
a flat highlight.

## root-mouse

`root-mouse KEY ACTION...` binds an unmodified mouse action on cow's output
background surface only. Use this for root-window style menus; use `mouse`
for normal River pointer bindings.
