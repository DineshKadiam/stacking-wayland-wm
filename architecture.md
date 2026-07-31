# CoW Architecture

CoW is a stacking window manager implemented as a Wayland client.  River remains
the compositor and display server; CoW supplies the window-management policy,
state, commands, decorations, rules, and companion module interfaces.

The important split is:

```
        applications
             |
             v
   +-------------------+
   |       river       |  Wayland compositor, output/input owner,
   |                   |  surface tree, protocol dispatch
   +---------+---------+
             ^
             | river_* management protocols
             v
   +-------------------+
   |        cow        |  policy, layout, stacking, decoration,
   |                   |  desks/pages, commands, rules, IPC
   +----+----------+---+
        ^          ^
        |          |
        |          +-- status stream: cowbar, cowpager, cowiconman, ...
        |
        +------------- command socket: moocow, scripts, modules
```

CoW can run without any of the companion modules.  The modules are ordinary
clients which consume CoW's command and status sockets.

## Process Model

`cow` is normally started by River, for example through:

```
river -c 'cow-start'
```

At startup CoW:

1. Loads configuration commands from `cow.conf`;
2. Connects to the Wayland display;
3. Binds River's window-management protocol globals;
4. Creates command and status sockets under `XDG_RUNTIME_DIR`;
5. Enters a libevent-backed loop around the Wayland fd, IPC sockets, timers,
   and signals.

The configuration file and runtime IPC use the same command language.  This is
intentional: a line in `cow.conf` should usually be usable through `moocow` as
well.

CoW does not need a CoW-specific file descriptor from River.  It connects like
any other Wayland client by calling `wl_display_connect(NULL)`.  libwayland then
uses the standard environment, primarily `WAYLAND_DISPLAY` and
`XDG_RUNTIME_DIR`, to find River's Wayland socket and open the client fd.  After
that, CoW obtains the Wayland fd with `wl_display_get_fd()` and registers it
with libevent so River protocol events can drive the main loop.

## River Interface

CoW depends on River-specific protocols generated from `protocol/*.xml`.
The central bindings are:

- `river_window_manager_v1`: manage/render sequencing, window/output/seat
  discovery.
- `river_window_v1`: per-window operations such as show, hide, position,
  sizing, SSD hints, focus, fullscreen/maximize state, and decoration handles.
- `river_output_v1`: output geometry and lifecycle.
- `river_xkb_bindings_v1`: compositor-side keyboard binding support.
- `river_layer_shell_v1`: shell surfaces used by CoW-owned UI such as
  backgrounds, pan frames, drag ghosts, and the geometry overlay.

CoW does not own the Wayland display.  It asks River to place nodes, focus
windows, raise/lower windows, show/hide surfaces, and commit compositor-visible
state.

CoW and River communicate by Wayland requests and events.  CoW can keep its own
model of windows, desks, pages, decorations, and focus intent, but River is the
authority for compositor objects and for when those objects may be safely
updated.

For example, when a command asks to move a window, CoW records the desired
position on the `cow_view`.  It does not directly mutate River's scene graph.
Instead, CoW requests another management pass.  River later calls back into CoW
with `manage_start`; CoW applies all pending policy decisions and replies with
`manage_finish`.  River can then schedule a render pass, during which CoW
updates decoration surfaces, node positions, and stacking before returning
`render_finish`.

That gives CoW a transactional shape:

```
Command / input / rule
        |
        v
Update CoW state and mark dirty
        |
        v
Request River manage pass
        |
        v
River manage_start -> CoW applies policy -> manage_finish
        |
        v
River render_start -> CoW commits visuals/order -> render_finish
```

## Wayland Round Trips

In Wayland, requests from a client to the compositor are buffered and events
from the compositor arrive asynchronously.  A round trip flushes pending
requests, asks the compositor to reply, and dispatches all events needed to get
to that reply.  In code this is `wl_display_roundtrip()`.

CoW uses round trips in two main places:

- during startup, to discover globals and then process the initial events from
  the globals it binds;
- before issuing `river_window_manager_v1_manage_dirty()` from the event loop,
  so earlier requests/events have settled before River starts a new management
  sequence.

This matters because CoW often acts on objects River has just announced or
changed.  Without a synchronization point, CoW could ask for a manage pass while
it still has unread output, seat, or window events queued locally.  The round
trip lets CoW catch up with River's current protocol state first, then request a
new manage cycle.

A round trip is not a frame render and it is not an acknowledgement that every
CoW policy decision has been applied to the screen.  It is a protocol
synchronization point.  The visible result still flows through River's
manage/render callbacks.

## Runtime Loop

The main loop is driven by libevent.  The Wayland display fd is watched
alongside command/status sockets, timers, and signals.

```
        event_base_loop()
              |
              +-- Wayland fd readable
              |      |
              |      +-- dispatch River events
              |
              +-- command socket readable
              |      |
              |      +-- parse command -> cmd_q -> mutate CoW state
              |
              +-- timers/signals
                     |
                     +-- manage_dirty, restart, shutdown, output debounce
```

Mutating commands generally mark state dirty rather than directly forcing all
Wayland updates immediately.  The actual compositor synchronization happens at
River's manage/render sequence boundaries.

## Manage And Render Sequences

River drives two key phases through the `river_window_manager_v1` API.  These
are protocol callbacks from River to CoW, not ordinary internal function calls.
CoW is expected to do its work for each phase and explicitly return control to
River with the matching finish request.

```
River                  CoW
-----                  ---
manage_start  ----->   destroy removed objects
                       tick outputs/pages/pan frames
                       configure new views
                       process seats and pointer operations
                       restore state on first manage after startup
                       apply deferred view changes/focus/warp
manage_finish <-----

render_start  ----->   render output backgrounds
                       render window decorations and icons
                       apply stack order
                       render geometry window and drag ghost
render_finish <-----
```

The manage phase is where CoW decides what should happen.  It updates its model
of windows, outputs, desks, pages, focus, pointer operations, and deferred
actions.  It may propose dimensions, show or hide windows, request focus, mark
views dirty, or arrange for later rendering work.  When that policy work is
done, CoW calls `river_window_manager_v1_manage_finish()`.

The render phase is where CoW commits visual state and ordering to River.  It
renders output backgrounds, window decorations, titlebars, icons, geometry
overlays, drag feedback, and applies stack order.  When the visual work is done,
CoW calls `river_window_manager_v1_render_finish()`.

The split is easiest to read as a small state machine:

```
              command, input, rule, timer
                         |
                         v
                    CoW state dirty
                         |
                         v
      river_window_manager_v1_manage_dirty()
                         |
                         v
                  +--------------+
                  | manage_start |
                  +--------------+
                         |
                         v
              policy and model updates
                         |
                         v
                  manage_finish
                         |
                         v
                  +--------------+
                  | render_start |
                  +--------------+
                         |
                         v
              surfaces, nodes, stacking
                         |
                         v
                  render_finish
```

This API boundary is important because River remains in charge of compositor
time.  CoW can request a new manage pass, but River decides when that pass
starts and tells CoW when it is safe to apply the next batch of management or
rendering work.  CoW therefore avoids applying protocol changes in arbitrary
command contexts when possible; it records intent and lets the next
manage/render cycle make it visible.

This split is why many commands set flags such as `needs_raise`,
`pending_focus`, `pending_lower`, `dec.dirty`, or request `manage_dirty`.
It keeps command execution, River event handling, and compositor commits in a
predictable order.

## Internal Model

The global runtime state is `struct cow_wm`.  It owns lists of:

- `cow_view`: one managed River window.
- `cow_output`: one River output.
- `cow_seat`: one River seat.
- `cow_container`: a tabbed group of views.

The core relationships are:

```
 cow_wm
   |
   +-- outputs
   |     |
   |     +-- desk_set
   |     |     |
   |     |     +-- desks
   |     |           |
   |     |           +-- page_vx/page_vy
   |     |
   |     +-- visible_views
   |
   +-- views
   |     |
   |     +-- output
   |     +-- desk
   |     +-- node
   |     +-- decoration surface
   |
   +-- seats
         |
         +-- focused view
         +-- pointer state
```

A view's logical position is CoW state.  River remains responsible for the
actual surface tree and presentation.  A view also carries a decoration
configuration snapshot so global decor changes and per-view overrides can be
managed separately.

Desks are per-output unless global desktop mode is configured.  Pages are stored
as viewport offsets on each desk (`page_vx`, `page_vy`) rather than as separate
objects.

## Commands And Configuration

Commands are declared in `cmd_table[]` and implemented by `src/cmd-*.c`.
Parsing is derived from tmux-style command infrastructure:

```
text command
     |
     v
cmd_string_parse()
     |
     v
cmd_list -> cmd_q
     |
     v
command exec function
     |
     v
CoW state mutation / IPC response / manage_dirty
```

Commands can be loaded from:

- `cow.conf`;
- `include` files;
- `moocow`;
- rules;
- titlebar button actions;
- scripts using `script { ... }`.

The small DSL is limited to `script` blocks.  It exists to add structured
control flow without replacing the command language used elsewhere.

## Rules

Rules bind command lists to lifecycle events such as map, move, resize, desk
change, page change, startup, restart, title change, and output topology
changes.

Rules are evaluated with an event context.  That context can expose the current
view, desk, or output to normal commands and to DSL scripts.

## IPC And Modules

CoW exposes two Unix sockets:

- `cow-$WAYLAND_DISPLAY-cmd.sock`: request/response command execution.
- `cow-$WAYLAND_DISPLAY-status.sock`: line-oriented JSON status stream.

The default paths live under `XDG_RUNTIME_DIR`; if `WAYLAND_DISPLAY` is unset,
CoW falls back to `cow-cmd.sock` and `cow-status.sock`.  `COW_IPC_SOCKET` and
`COW_STATUS_SOCKET` can override the client paths.

`moocow` is the command-line client for the command socket.  Companion programs
such as `cowbar`, `cowpager`, `cowident`, `cowrearrange`, and `cowiconman` use
the shared module helpers in `src/module.c` to connect to the sockets.

```
 module
   |
   +-- connect status socket
   |      |
   |      +-- receive JSON snapshots
   |
   +-- connect command socket
          |
          +-- send ordinary CoW commands
```

This keeps modules small: they do not need to bind River protocols or duplicate
window-management state.  They observe CoW's status stream and ask CoW to act.

## Decorations

Server-side decorations are CoW-owned surfaces associated with each managed
view.  CoW renders borders, titlebars, buttons, gradients, images, container
tabs, icons, and geometry overlays, then asks River to position the resulting
nodes.

Decor configuration is split between:

- global defaults;
- named decor definitions;
- per-view snapshots and overrides;
- rules which apply decor dynamically.

This lets a theme define reusable defaults while still allowing rules to alter
specific windows, desks, or applications.

## State Persistence

CoW saves and restores enough state to survive restart and compositor
reconnection scenarios where River preserves windows.  Stable River window
identifiers are used to associate restored state with newly reported windows.

State restoration is deferred until the first manage sequence after startup so
that initial windows, outputs, and seats have been reported before CoW applies
saved placement, desk, page, and related state.

## Design Boundaries

CoW deliberately avoids becoming a compositor.  River owns:

- the Wayland display;
- monitor configuration;
- input routing at the compositor level;
- presentation and surface lifetime;
- protocol-level window objects.

CoW owns:

- stacking-window-manager policy;
- command/config semantics;
- desks, pages, rules, and decoration policy;
- module-facing IPC and status;
- state persistence.

This boundary is the main design tradeoff.  CoW gains a smaller compositor
surface area and can focus on FVWM/MWM-style window-management behaviour, while
River remains the required compositor backend.
