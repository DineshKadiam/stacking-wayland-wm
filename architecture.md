# CoW Architecture

CoW is a stacking window manager implemented as a Wayland client. River remains
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

CoW can run without any of the companion modules. The modules are ordinary
clients which consume CoW's command and status sockets.

## Process Model

`cow` is normally started by River, for example through:

```
river -c 'cow-start'
```

At startup CoW:

1. Loads the installed `cow-defaults.conf`, then user commands from `cow.conf`;
2. Connects to the Wayland display;
3. Binds River's window-management protocol globals;
4. Creates command and status sockets under `XDG_RUNTIME_DIR`;
5. Enters a libevent-backed loop around the Wayland fd, IPC sockets, timers,
   and signals.

The configuration file and runtime IPC use the same command language. This is
intentional: a line in `cow.conf` should usually be usable through `moocow` as
well.

The package-owned baseline is installed under `datadir/cow`. Startup and
`quit -r` use the same defaults-then-user loader; `cow -n` disables the
baseline. Menus inherited from defaults are replaced on their first user
`menu-add`. That implicit clear is recorded alongside configuration commands so
exported configs can be replayed without accumulating duplicate entries.
`show -D config` reads the installed baseline without executing it.

CoW does not need a CoW-specific file descriptor from River. It connects like
any other Wayland client by calling `wl_display_connect(NULL)`. libwayland then
uses the standard environment, primarily `WAYLAND_DISPLAY` and
`XDG_RUNTIME_DIR`, to find River's Wayland socket and open the client fd. After
that, CoW obtains the Wayland fd with `wl_display_get_fd()` and registers it
with libevent so River protocol events can drive the main loop.

## River Interface

CoW depends on River-specific protocols generated from `protocol/*.xml`. The
central bindings are:

- `river_window_manager_v1`: manage/render sequencing, window/output/seat
  discovery.
- `river_window_v1`: per-window operations such as show, hide, position,
  sizing, SSD hints, focus, fullscreen/maximize state, and decoration handles.
- `river_output_v1`: output geometry and lifecycle.
- `river_xkb_bindings_v1`: compositor-side keyboard binding support.
- `river_layer_shell_v1`: shell surfaces used by CoW-owned UI such as
  backgrounds, pan frames, drag ghosts, and the geometry overlay.

CoW does not own the Wayland display. It asks River to place nodes, focus
windows, raise/lower windows, show/hide surfaces, and commit compositor-visible
state.

CoW and River communicate by Wayland requests and events. CoW can keep its own
model of windows, desks, pages, decorations, and focus intent, but River is the
authority for compositor objects and for when those objects may be safely
updated.

Chorded keyboard bindings are stored as per-seat binding maps. Pressing a
prefix enables only the map below that prefix and asks
`river_xkb_bindings_seat_v1` to consume the next non-modifier key. A matching
binding either enters another map or runs the final action; River reports an
unmatched consumed key so CoW can restore the normal root bindings.

For example, when a command asks to move a window, CoW records the desired
position on the `cow_view`. It does not directly mutate River's scene graph.
Instead, CoW registers that another management pass is required. At the end of
the current event-loop turn, the update scheduler sends the coalesced request
to River. River later calls back into CoW with `manage_start`; CoW applies all
pending policy decisions and replies with `manage_finish`. River can then
schedule a render pass, during which CoW updates decoration surfaces, node
positions, and stacking before returning `render_finish`.

That gives CoW the following:

```
Command / input / rule
        |
        v
Update CoW state and register required updates
        |
        v
Event-loop drain requests River manage pass
        |
        v
River manage_start -> CoW applies policy -> manage_finish
        |
        v
River render_start -> CoW commits visuals/order -> render_finish
```

## Wayland Round Trips

In Wayland, requests from a client to the compositor are buffered and events
from the compositor arrive asynchronously. A round trip flushes pending
requests, asks the compositor to reply, and dispatches all events needed to get
to that reply. In code this is `wl_display_roundtrip()`.

CoW uses round trips at lifecycle boundaries:

- during startup, to discover globals and then process the initial events from
  the globals it binds;
- during shutdown, to let River process the window-manager stop request before
  the protocol objects are destroyed.

Runtime updates do not perform a round trip before requesting a manage pass.
Wayland preserves request order, so `manage_dirty` follows any earlier requests
already queued by CoW. Avoiding a synchronous round trip keeps the main loop
responsive. CoW flushes the display normally and, if the socket would block,
temporarily watches it for writability before continuing the flush.

A round trip is not a frame render and it is not an acknowledgement that every
CoW policy decision has been applied to the screen. It is a protocol
synchronization point. The visible result still flows through River's
manage/render callbacks.

## Runtime Loop

The main loop is driven by libevent. The Wayland display fd is watched
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
              |      |
              |      +-- restart, shutdown, logging, output debounce
              |
              +-- end of event-loop turn
                     |
                     +-- emit status snapshot / request manage pass
```

Mutating commands generally mark state dirty rather than directly forcing all
Wayland updates immediately. Once the triggering callback has completed, CoW
drains the accumulated update mask. The actual compositor synchronization
happens at River's manage/render sequence boundaries.

## Update Scheduling

State mutations register one or both update classes through the shared update
scheduler:

- `COW_UPDATE_MANAGE` means pending policy must be applied in a River manage
  sequence;
- `COW_UPDATE_STATUS` means canonical state represented by the module status
  snapshot has changed.

Requests are bitwise flags and therefore coalesce naturally during an
event-loop turn. At the end of that turn, CoW emits at most one status snapshot
and sends at most one `manage_dirty` request. This keeps modules from observing
intermediate state when a command or River callback changes several related
fields.

Status updates are registered where authoritative fields actually change, not
where an operation merely records an intention to change them. Manage updates
are registered when an operation creates work which River must apply. Keeping
that distinction at mutation sites makes propagation part of the state change
rather than an easily missed responsibility of its caller.

## State Queries

The `show` command is the single command-socket interface for inspecting CoW
state. This includes quering windows, showing menu settings, bindings, etc.

Commands which change reusable configuration are marked as persistent in their
command-table entry. After successful execution their canonical command text is
recorded in `cow_config`. The command `show config` outputs this.

The query implementation and in-process script iterators share the canonical
record builders in `state-snapshot.c`. This keeps fields such as window
geometry, output page state, and rule metadata identical between JSON queries
and expressions such as `for w in (windows)`. `state-value.c` supplies the
neutral typed representation used by both the expression evaluator and JSON
serialization; it is not tied to either interface.

Container and pane collections use these same adapters. They contain IDs and
group-level state rather than copies of window records: windows link through
`container_id` and `pane_id`, and each group or pane names its active window.
Pane snapshots distinguish requested title hiding from effective visibility.
Unlike window iteration, container and pane loops do not change the implicit
command target; callers select a member window explicitly.

Optional container names are globally unique; pane names are unique within
their container. Names are runtime identity, separate from displayed titles,
and are included in restart state and query records. An explicitly named
container may retain one member for mapping rules to populate later. Explicit
layouts (`window-container -E`) also retain empty panes and an empty group.
Pane enumeration walks the layout tree, not the membership list. Application
counts exclude the internal views used for vacant panes.

`container_empty.c` creates native xdg toplevels for the permanent outer frame
and vacant panes. These reuse the normal decoration, focus and pointer paths.
The outer frame is not a member of its own layout: its buttons control the
group. Its client rectangle contains the pane layout. Vacant pane views are
temporary members, replaced when an application joins and recreated when it
leaves. Internal windows are recreated from the container record on restart,
rather than saved as application members. Drops and on-map rules use the same
pane insertion path.

## Manage And Render Sequences

River drives two key phases through the `river_window_manager_v1` API. These
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

The manage phase is where CoW decides what should happen. It updates its model
of windows, outputs, desks, pages, focus, pointer operations, and deferred
actions. It may propose dimensions, show or hide windows, request focus, mark
views dirty, or arrange for later rendering work. When that policy work is
done, CoW calls `river_window_manager_v1_manage_finish()`.

The render phase is where CoW commits visual state and ordering to River. It
renders output backgrounds, window decorations, titlebars, icons, geometry
overlays, drag feedback, and applies stack order. When the visual work is done,
CoW calls `river_window_manager_v1_render_finish()`.

The split is easiest to read as a small state machine:

```
              command, input, rule, timer
                         |
                         v
              register COW_UPDATE_MANAGE
                         |
                         v
                  event-loop drain
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
time. CoW can request a new manage pass, but River decides when that pass
starts and tells CoW when it is safe to apply the next batch of management or
rendering work. CoW therefore avoids applying protocol changes in arbitrary
command contexts when possible; it records intent and lets the next
manage/render cycle make it visible.

This split is why many commands set flags such as `needs_raise`,
`pending_focus`, `pending_lower`, or `dec.dirty`, then register a manage
update. It keeps command execution, River event handling, and compositor
commits in a predictable order.

## Diagnostics

CoW records low-overhead counters at the update scheduler and at the
manage/render protocol boundaries. `show diagnostics` exposes those counters
without generating periodic status snapshots.

The recorded intervals deliberately describe only what CoW can observe:

- first pending manage request to River's `manage_start` event;
- time spent executing CoW's manage callback;
- CoW's `manage_finish` request to River's next `render_start` event; and
- time spent executing CoW's render callback.

They do not represent River's scene rendering, GPU work, or presentation
latency. `cowdiag` supplements these internal measurements with process CPU and
memory statistics where the host operating system exposes them.

## Internal Model

The global runtime state is `struct cow_wm`. It owns lists of:

- `cow_view`: one managed River window.
- `cow_output`: one River output.
- `cow_seat`: one River seat.
- `cow_container`: a floating split tree of tabbed panes.

Container ownership, tab activation and member geometry synchronisation live in
`src/container.c`, with declarations in `include/cow/container.h`. Individual
decoration resources remain owned by the view.

Containers use a binary tree to represent columns and rows: horizontal nodes
divide columns, and vertical nodes within each column divide rows. Horizontal
nodes may occur below vertical nodes after rotation. Leaves are
`cow_container_pane`s. A
pane owns tab order and selection; the container retains its full membership
list for group operations and restart state. Visibility and titlebar rendering
use the pane's selected tab, not the container's last activated member.
Fullscreen hides the other panes temporarily; snapshots use the same visibility
check as rendering.

The internal split API can move a tab into a new row or column and prune empty
leaves. Pane rectangles are converted to content rectangles through the
existing titlebar geometry helpers. Explicit operations arrange the group;
client dimension events report only a pane's accepted dimensions and must not
resize the group. `window-container` exposes splits, pane navigation, collapse
and equalisation. Shared-edge pointer resizes update split proportions through
the existing pointer-operation loop; outer edges resize the group frame.

Row collapse operates across the entire column, leaving at least one row
expanded. Whole-container shading renders its titlebars as one horizontal strip
without replacing the saved layout tree or row-collapse state.

Whole pane layouts can be tabs in an ordinary outer tab group. The layout's
native frame owns the panes and separately holds membership in the tab group.
Group traversal includes both levels for stacking, desk/output changes,
iconification and sticky flags. A hidden outer tab hides all its panes;
activating a pane selects that tab. Nesting a layout inside another layout's
pane is rejected. Restart restores these outer tab groups after recreating the
native frames, resolving their old identifiers through saved layout state.

`src/container_state.c` stores each container's complete tree, split proportions, tab
order, selected tabs and pre-container floating geometries. The decoder builds
and validates an unpublished tree before attaching any live views. Missing
clients remove tabs or empty panes; no particular anchor window must survive.
Legacy flat membership records remain readable. The private
`src/container_internal.h` shares runtime helpers with the decoder.

`src/container_layout.c` provides the geometry calculation for split
containers. It operates on a binary tree of horizontal and vertical splits,
with minimum frame sizes at the leaves. A bottom-up measurement pass feeds a
top-down arrangement pass, visiting each node once per pass.
It allocates no Wayland objects and
sends no requests. An invalid or undersized layout is rejected without changing
the previous geometry; temporary minimum-size constraints do not overwrite the
requested split proportions. This layout engine is tested independently and
used by the container API. Integration proposes client dimensions and
tiled-edge hints, and clips content to its pane. Moving, stacking, desk
changes, maximization and iconification operate on the group; closing and
detaching operate on the targeted member.

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

A view's logical position is CoW state. River remains responsible for the
actual surface tree and presentation. A view also carries a decoration
configuration snapshot so global decor changes and per-view overrides can be
managed separately.

Desks are per-output unless global desktop mode is configured. Pages are stored
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
CoW state mutation / IPC response / update registration
```

Commands can be loaded from:

- `cow.conf`;
- `include` files;
- `moocow`;
- rules;
- titlebar button actions;
- scripts using `script { ... }`.

The small DSL is limited to `script` blocks. It exists to add structured
control flow without replacing the command language used elsewhere. Commands
emitted by a script run through a child command queue before the following
statement is evaluated. A later collection snapshot therefore sees logical
state changes, including pending focus, made by earlier statements even though
River applies the corresponding compositor request in the next manage cycle.

Each command queue also owns its error list. Child queues transfer errors to
their parent explicitly, as happens for `include` and DSL execution. Keeping
errors out of process-global state prevents an IPC command, key binding, or
configuration load from consuming failures produced by another event.

## Rules And Styles

Rules bind command lists to lifecycle events such as map, move, resize, desk
change, page change, startup, restart, title change, and output topology
changes.

Rules are evaluated with an event context. That context can expose the current
view, desk, or output to normal commands and to DSL scripts.

Rules and styles share a policy definition containing the object's name, global
or output/desk scope, optional window selector, and definition order. The
shared matcher therefore gives `%app`, `/title/`, and `#identifier` the same
meaning in both interfaces.

Their payloads deliberately differ. A rule associates an event with a command
list and executes that list when the event occurs. A style contains typed
window policy which can be resolved repeatedly and undone when the style stops
matching. For a window event, CoW resolves matching styles in definition order
before dispatching matching rules. Later styles override individual fields; a
command subsequently issued by a rule or through IPC marks that field as an
explicit per-window override.

Decoration policy uses three layers: the global configuration, sparse named or
inline style overlays, and explicit per-window overrides. Initial placement and
state fields are applied once when the window is first managed. Live fields
such as decoration, layer, stickiness, and circulation are recomputed when
relevant window or scope state changes and after configuration reload.

## IPC And Modules

CoW exposes two Unix sockets:

- `cow-$WAYLAND_DISPLAY-cmd.sock`: request/response command execution.
- `cow-$WAYLAND_DISPLAY-status.sock`: line-oriented JSON status stream.

The default paths live under `XDG_RUNTIME_DIR`; if `WAYLAND_DISPLAY` is unset,
CoW falls back to `cow-cmd.sock` and `cow-status.sock`. `COW_IPC_SOCKET` and
`COW_STATUS_SOCKET` can override the client paths.

`moocow` is the command-line client for the command socket. Companion programs
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
window-management state. They observe CoW's coalesced status snapshots and ask
CoW to act.

Socket output is buffered with libevent. Command clients are closed only after
their response has drained, and a slow status client retains at most one queued
snapshot. This avoids truncating nonblocking writes without allowing an
unbounded backlog of obsolete state.

Modules use `module_status.c` for event-driven connection, reconnection, and
line framing. The helper stores incoming data on the heap, so module threads do
not need large stack buffers; modules remain in their Wayland/libevent main
thread. The canonical status snapshot contains outputs, desks, windows, focus,
and theme data. `module_snapshot.c` parses that state for modules which need a
local model, notably cowpager and cowiconman. Those modules therefore do not
issue a second synchronous `show -a window` request or maintain separate JSON
parsers and fixed-size state arrays.

## Decorations

Server-side decorations are CoW-owned surfaces associated with each managed
view. CoW renders borders, titlebars, buttons, gradients, images, container
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
reconnection scenarios where River preserves windows. Stable River window
identifiers are used to associate restored state with newly reported windows.

State restoration is deferred until the first manage sequence after startup so
that initial windows, outputs, and seats have been reported before CoW applies
saved placement, desk, page, and related state.

## Design Boundaries

CoW deliberately avoids becoming a compositor. River owns:

- the Wayland display;
- monitor configuration;
- input routing at the compositor level;
- presentation and surface lifetime;
- protocol-level window objects.

CoW owns:

- stacking-window-manager policy;
- command/config semantics;
- desks, pages, rules, styles, and decoration policy;
- module-facing IPC and status;
- state persistence.

This boundary is the main design tradeoff. CoW gains a smaller compositor
surface area and can focus on FVWM/MWM-style window-management behaviour, while
River remains the required compositor backend.
