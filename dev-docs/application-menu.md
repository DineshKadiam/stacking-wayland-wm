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

Other elements, attributes, and namespaces are rejected with diagnostics. This
deliberately is not a complete XDG Menu implementation. `Move` and menu
inlining remain to be implemented. `inline="false"` is accepted; other inlining
attributes are rejected rather than silently ignored.

XML parsing does not load external DTDs or substitute entities. Internal DTD
declarations are rejected. The usual external menu DOCTYPE is accepted without
fetching it. Recursive includes, including symlink cycles, are rejected. The
loader accepts regular local files only, with limits of 64 nesting levels, 256
files, and 16 MiB aggregate XML input.

Later consumers will generate searchable CoW menus and a starter cowbuttons
panel from the same catalogue. Existing defaults and personal configuration are
deliberately unchanged at this stage.
