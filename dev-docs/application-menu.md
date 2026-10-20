# Application discovery

`cow-appmenu` is a standalone C helper for the starter desktop. It writes a
versioned JSON catalogue to standard output without connecting to CoW or
Wayland. It reuses CoW's memory helpers and JSON library, and uses libxml2 for
menu XML. It does not depend on Xfce, Garcon, GLib, or Python. Meson enables
the helper when libxml2 is available; `-Dcow_appmenu=enabled` requires it.

```sh
meson compile -C build cow-appmenu
build/cow-appmenu | jq .
```

The initial implementation discovers `.desktop` files in
`$XDG_DATA_HOME/applications` and `$XDG_DATA_DIRS/applications`, using the
standard fallback directories. Earlier directories take precedence. Hidden
entries mask lower-priority copies. Desktop-entry IDs include subdirectory
names, separated by hyphens.

The catalogue filters `Hidden`, `NoDisplay`, `OnlyShowIn`, `NotShowIn`, and
`TryExec`, and selects translated labels using `LC_ALL`, `LC_MESSAGES`, then
`LANG`. When `XDG_CURRENT_DESKTOP` is unset, it uses `CoW`. Directory symlinks
are not traversed, to avoid cycles. Sorting within each directory makes
discovery repeatable.

Each application includes its ID, source file, translated display fields,
execution metadata, categories, and keywords. `Exec`, `Categories`, and
`Keywords` retain desktop-entry escaping. **Exec is not a shell command**:
consumers must parse desktop-entry quoting and field codes before launching.
Icon values may be theme names, not paths. Discovery never executes entries.

## Menu resolution

The resolver follows the
[XDG menu-file format](https://specifications.freedesktop.org/menu/latest/menu-file-format.html)
for the supported elements listed below.

```sh
build/cow-appmenu --menu examples/applications.menu | jq .menu
```

Bare `--menu` discovers `menus/${XDG_MENU_PREFIX}applications.menu` in
`XDG_CONFIG_HOME` (default `~/.config`), then `XDG_CONFIG_DIRS` (default
`/etc/xdg`). Only absence selects the installed CoW fallback; an invalid
existing menu fails. To test discovery before installation:

```sh
build/cow-appmenu --menu --fallback examples/applications.menu | jq .menu
```

`menu_file` and `menu_source` identify the chosen file and whether it was
explicit, discovered, or a fallback. No arguments still produce only the
application catalogue.

`--menu` adds a hierarchy to the JSON catalogue. Each menu has a stable
slash-separated `id`, a `name`, an array of application references, and a
`menus` array. In JSON version 2, each reference contains both `id` and `file`,
matching the corresponding catalogue entry. This distinguishes same-ID
applications from different search scopes. Entries refer to the shared
catalogue rather than duplicating it. Empty menus remain in the data; a
presentation layer can hide them.

The initial XDG Menu subset supports `Menu`, `Name`, `AppDir`,
`DefaultAppDirs`, `Include`, `Exclude`, `All`, `Filename`, `Category`, `And`,
`Or`, `Not`, `Deleted`, and `NotDeleted`. Include/exclude rules execute in
document order. Application directories apply to the declaring menu and
descendants, not siblings. Later directories override earlier ones;
`DefaultAppDirs` expands at its position in that order, retaining
user-over-system precedence. Relative paths are resolved against the menu file.
Identical search scopes reuse a cached catalogue rather than rescanning
directories for every menu.

The top-level catalogue contains visible default applications plus custom
applications considered by menus, deduplicated by ID and source file. Menu
membership order follows the document; application membership follows
discovery.

The additional `entries` array supplies presentation order without changing
membership. It contains `type: menu` references by ID, `type: application`
references by ID and file, and `type: separator` records. Consumers should
render this array rather than concatenate the membership arrays.

`Layout` and inherited `DefaultLayout` support explicit `Filename` and
`Menuname` positions, `Separator`, and `Merge` (`menus`, `files`, or `all`).
Unmentioned entries are sorted by translated label using locale collation, with
stable ID tie-breaking. The built-in layout places menus before files. Layout
runs after allocation filtering; empty submenus are hidden unless `show_empty`
requests them. Only the last layout is effective; an empty `Layout` uses the
applicable default. Redundant separators are removed.

`Directory`, `DirectoryDir`, and `DefaultDirectoryDirs` supply translated menu
labels, comments, icons, and visibility from `.directory` files. The reader and
locale fallback are shared with application discovery. Relative directory
search paths are resolved against the menu file, and later search paths take
priority. Search paths are inherited by descendants, not siblings. Missing
directory references fall back to earlier references, then `Name`. The stable
menu ID always uses `Name`, independent of the displayed language.

`OnlyUnallocated` menus contain entries which no ordinary menu has included. An
entry remains allocated if a later rule excludes it. Allocation is collected
before filtering these menus, regardless of their document order.
`NotOnlyUnallocated` restores ordinary matching; the last of the two flags
wins. Deleted or invisible menus do not allocate entries.

`MergeFile` (with no type or `type="path"`) and `MergeDir` include local menu
files. Imported paths are resolved relative to their source file, not the outer
menu. Same-named submenus are combined at the last occurrence, retaining
directive order so later rules can override earlier ones. Missing includes are
ignored; unreadable or malformed files fail the operation. Merge directories
are processed in filename order for repeatable results.

`MergeFile type="parent"` ignores its text and finds the same relative menu
filename in the remaining configuration directories. `DefaultMergeDirs` expands
the corresponding `menus/<name>-merged` directories from system to user, so
user fragments take precedence. The applications menu uses
`applications-merged`, stripping the configured `XDG_MENU_PREFIX`.

`Move` applies relative `Old`/`New` menu paths after file merging. Child moves
run before parent moves; pairs within a menu run in document order. Missing
sources do nothing, missing destinations are created, and existing destinations
retain precedence when merged. Moves into a source's own descendants are
rejected, and the resulting hierarchy must remain within the nesting limit.

Layouts support `inline`, `inline_limit`, `inline_header`, and `inline_alias`
on `DefaultLayout`, with per-child overrides on `Menuname`. Inlining flattens
presentation entries without changing membership or menu IDs. Entries may
reference descendant menus, carry a `name` override, or have type `header` with
a `name`. Single-entry aliases retain the original launch action and icon.

Other elements, attributes, and namespaces are rejected with diagnostics. This
deliberately is not a complete XDG Menu implementation.

XML parsing does not load external DTDs or substitute entities. Internal DTD
declarations are rejected. The usual external menu DOCTYPE is accepted without
fetching it. Recursive includes, including symlink cycles, are rejected. The
loader accepts regular local files only, with limits of 64 nesting levels, 256
files, and 16 MiB aggregate XML input.

The default root menu opens `CoWApplications` as a normal submenu. A deferred
`moocow applications-refresh` resolves the user's alias after configuration
loading, including on restart. Its default invokes `cow-applications --load`
outside CoW rather than blocking config loading. The wrapper generates into a
private temporary file and includes it only on success. This reserved namespace
avoids replacing user-defined menus. No personal configuration is rewritten.

Refresh failures produce a brief notification as well as stderr diagnostics.
A successful refresh dismisses that notification. This belongs to the wrapper;
the catalogue generator remains independent of CoW.

## Starter panel

Panel configuration belongs to cowbuttons. `cowbuttons -S` loads the shipped
`starter-panel.conf`, which users can copy and edit with the normal grammar.
Its `applications` entries consume the external JSON catalogue and select one
available application per category, without duplicate launchers. Explicit
`Category=desktop-id.desktop` preferences override optional `xdg-mime` defaults
for browser, mail, file manager and editor roles. Category matches prefer
graphical applications and sort by name and ID. Unmatched slots use other
available applications, after category selection within each directive. The
catalogue reports executable availability; cowbuttons owns button layout, icon
resolution and presentation. Launches reuse `cow-appmenu --launch`.

The panel opens rightwards and includes a terminal and cowiconman when those
programs are installed. Its `cow {}` block handles placement and reservations.
Neither layout nor configuration generation belongs in cow-appmenu.

The compact launcher buttons use icons, with text as a fallback. The terminal
and task list share the remaining width equally; the terminal respawns if
closed. The Applications arrow toggles the menu above the panel.

A `desks` cell uses the pinned output's live desk list and active desk. Its
ordinary `desk -t` action follows CoW's global or per-output desk policy.
The embedded cowpager is pinned to that same output and shows its current desk's
page grid. Its output-specific title prevents another panel recapturing it.
If cowpager is unavailable, the desk buttons still work. Both controls reuse
the existing status subscriptions rather than polling or generating config.

The default `starter-panels` alias loops over outputs and starts
`cowbuttons -S -m OUTPUT`. The `starter` alias defers its initial invocation
until after configuration loading and installs an `on-outputs-changed` rule;
no separate supervisor is involved. A per-session,
per-panel/output lock prevents duplicate windows. Repeated launches leave the
embedded CoW policy alone within the same session, restoring it when CoW
replaces its command socket on restart. Panels close when their output
disappears.

The terminal and task list have exact, output-specific titles. Cowiconman's
`title-env` setting receives its title from the external command in the panel
configuration, so concurrent panels cannot capture each other's task lists. The
normal standalone cowiconman title remains unchanged.

The panel's rules and style use `=title` exact selectors. Substring selectors
would also match embedded clients and similarly named outputs such as `DP-1`
and `DP-10`.

Only the panel reserves desktop space, not its swallowed clients. Reapplying
the policy restores that reservation before anchoring the panel, so a restart
does not move it above its own reserved space.

Users can disable startup with `alias starter-panels nop`, or replace the alias
to load their own copy of `starter-panel.conf`. Existing panels are left alone;
close and relaunch them to load edited button layouts.

## Welcome message

The `starter` alias invokes `welcome` through deferred IPC, after personal
overrides have loaded. Its default is a persistent, output-centred `notify -c`
popup
explaining the root menu and configuration layers. The root menu's **Starter**
submenu shows or dismisses it. `alias welcome nop` disables it on subsequent
starts without replacing the defaults; `notify -n CoWWelcome -d` dismisses the
current message.

Automatic starter activation is limited to sessions without a selected overlay,
or with `/dev/null` explicitly selected for a defaults-only desktop.
The loader sets `session.has_config` before reading defaults, including for
explicit empty or unreadable files, but excluding `/dev/null`.
Defaults invoke `starter` only when it is false. Personal configs can opt in
with `starter`; manually calling `welcome`
or `starter-panels` does not enable hotplug startup. Includes leave the session
flag unchanged. Reloading recomputes it and rebuilds rules from the new config.

## Startup validation

The application-panel test executes the shipped panel's embedded `cow {}`
script without a running compositor. It starts with no aliases and evaluates
the script twice, checking that reloads replace rather than duplicate rules and
styles. Syntax-only configuration checks do not execute script bodies and
cannot catch these startup failures.

## CoW output

```sh
build/cow-appmenu --menu examples/applications.menu --format cow \
    --name Applications --terminal foot > /tmp/applications.conf
```

Include the generated file and use `menu-show Applications`. Each menu uses
typeahead; a flat “All applications” submenu searches across categories.
Generation does not connect to CoW. Install the helper before selecting
entries, or put the build directory on the session's `PATH`.

The renderer uses the catalogue's executable-availability flag to disable
unavailable launchers, as well as unsupported entries. Empty menus display a
disabled explanation without changing the JSON membership or layout data.

The renderer uses `cow_command_from_argv` for CoW syntax and separately quotes
the filename and terminal for the shell used by CoW's `exec` command.
Desktop-entry `Exec` text never enters that shell command. Popup targets are
literal names, not command arguments.

Generated menus own their name prefix and replace contents on reload.
Previously generated but now unreachable submenus are not deleted.

Application and directory icons reuse `cow_icon_name_resolve`: XDG data
directories, `hicolor` size matching, then `pixmaps`, with user precedence.
`--icon-size` controls lookup size, not the menu's style. The renderer caches
successful and missing lookups for one generation. Absolute PNG/SVG/XPM files
are also supported. Full `index.theme` inheritance remains pending.

The shared lookup has an optional acceptance callback: CoW retains its image
rendering checks, while this helper only locates readable regular files.
Consequently the helper gains no Cairo, GTK, or desktop-environment dependency.

## Launching

`cow-appmenu --launch FILE [--terminal PROGRAM]` rereads an entry using the
shared reader, parses `Exec`, changes to `Path` when specified, and calls
`execvp`. No shell interprets the desktop-entry arguments. Field expansion does
not recursively interpret substituted labels or paths.

Terminal entries use `PROGRAM -e ...`, defaulting to `xterm`. D-Bus activation
and startup notification are not yet supported; D-Bus-enabled entries need an
`Exec` fallback. Discovery never launches applications.

The `cow-applications` wrapper passes `cow-terminal` instead, selecting foot or
xterm when the entry is launched. The default root-menu Terminal entry and
terminal binding use this helper too. An explicit terminal argument to
`cow-applications` still takes precedence.
