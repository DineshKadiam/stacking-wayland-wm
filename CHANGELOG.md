# Changelog

## [0.4](https://codeberg.org/cow-wm/cow/releases/tag/0.4) - 2026-10-25

### New contributors

- [@farhaven](https://codeberg.org/farhaven) contributed for the first time in
  [PR #402](https://codeberg.org/cow-wm/cow/pulls/402).
- [@NyctraVex](https://codeberg.org/NyctraVex) contributed for the first time
  in [PR #412](https://codeberg.org/cow-wm/cow/pulls/412).

### Added

- [PR](https://codeberg.org/cow-wm/cow/pulls/408): windows: provide top-level
  containers for creating arbitrary window layouts —
  [@thomasadam](https://codeberg.org/thomasadam)

### Fixed

- [PR](https://codeberg.org/cow-wm/cow/pulls/470): view: handle rapid title
  updates — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/468): container: prevent hidden
  tabs changing geometry — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/460): output: don't redraw
  unchanged backgrounds — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/459): restart: persist
  squeeze/rotation state — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/458): decor: position handle
  highlight correctly — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/456): Squeeze Title Fixes —
  [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/443): resize: keep opposite edges
  fixed when interactive — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/453): DSL: don't treat w.desk as a
  command — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/449): iconify: focus appropriate
  window — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/448): dsl: allow single quotes —
  [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/442): Fix shaded windows when
  rotating titlebars — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/441): Squeeze/Decor fixes —
  [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/436): decor: wrap frames around
  squeezed titles — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/425): decor: preserve border style
  on squeezed titlebars — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/422): focus: fix -f targets to not
  conflict with n/p — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/409): fix: use portable option
  ordering in show examples — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/407): fix: preserve maximized
  window pages on restart — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/406): fix: recognize maximize
  buttons script — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/394): cmd: prevent stack
  corruption — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/390): module: fix snapshot issues
  — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/389): cowpager: inherit font from
  CoW — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/387): decor: preserve per-window
  overrides styles — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/379): menu: handle clicks released
  before selection — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/372): Make script command
  execution sequential — [@thomasadam](https://codeberg.org/thomasadam)

### Changed

- [PR](https://codeberg.org/cow-wm/cow/pulls/471): Text captions for cowpager
  entries — [@hakfoo](https://codeberg.org/hakfoo)
- [PR](https://codeberg.org/cow-wm/cow/pulls/473): cowpager: respect window
  stacking order — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/467): default config: introduce
  better hand-holding — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/462): Outputs: per-page
  backgrounds — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/451): script: MRU: add filter for
  windows — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/450): Add most-recently-used focus
  circulation — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/445): move: snap: allow for ignore
  list — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/439): window-shade: default to
  titlebar direction — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/423): cowbuttons: add example CDE
  panel — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/421): containers: generalise for
  cowbuttons (FvwmButtons) — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/420): panframes: pass input
  through to windows — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/417): rules: add on-focus-change
  event — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/415): focus: add -f to move
  desk/page to window — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/410): cowpager: make page
  separator lines configurable — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/402): icon: make icon text
  configurable — [@farhaven](https://codeberg.org/farhaven)
- [PR](https://codeberg.org/cow-wm/cow/pulls/401): icons: use active colours on
  pointer hover — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/400): resize: add automatic-corner
  option — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/396): alias: support passing
  parameters to script blocks — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/385): events: add on-edgedrop-x
  events when window is dragged into a screen edge, held, and dropped —
  [@hakfoo](https://codeberg.org/hakfoo)
- [PR](https://codeberg.org/cow-wm/cow/pulls/383): code: refactor modules to
  use event loops and not threads —
  [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/380): menu: Allow "disabled" items
  with distinct colour and blocked interactivity —
  [@hakfoo](https://codeberg.org/hakfoo)
- [PR](https://codeberg.org/cow-wm/cow/pulls/376): commands: add 'alias' —
  [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/375): Add named window selections
  and interactive choice menus — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/374): rule: add fullscreen events
  — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/367): Allow nested style {} blocks
  inside script {} blocks — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/365): style/window-move: add
  anchored window placement — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/364): menus: add configurable side
  columns — [@thomasadam](https://codeberg.org/thomasadam)

### Internal improvements

- [PR](https://codeberg.org/cow-wm/cow/pulls/418): decor: share buffers between
  windows — [@thomasadam](https://codeberg.org/thomasadam)

### Other changes

- [PR](https://codeberg.org/cow-wm/cow/pulls/465): bindings: new command notify
  / chord hints — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/464): ta/deprecations —
  [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/461): bindings: few fixes —
  [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/416): Support running on NetBSD —
  [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/427): focus: report failure when
  no window matches — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/419): decor: add a squeeze toggle
  — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/395): input: add synchronized
  send-key command — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/412): Fix broken manuals grid —
  [@NyctraVex](https://codeberg.org/NyctraVex)
- [PR](https://codeberg.org/cow-wm/cow/pulls/411): after-start: remove
  after-start.sh — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/405): config: use global config
  overriden in user's cow.conf — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/404): bindings: allow when no
  background is present — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/397): menus: add -c and -o options
  to show-menu to allow for any menu to launch centre-of-screen —
  [@hakfoo](https://codeberg.org/hakfoo)
- [PR](https://codeberg.org/cow-wm/cow/pulls/384): scdoc: convert to markdown
  (via lowdown) — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/371): build: CI: increase IRC
  timeout — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/369): keyboard: remember layouts
  per window — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/368): website: fix mobile
  horizontal overflow — [@thomasadam](https://codeberg.org/thomasadam)

## [0.3](https://codeberg.org/cow-wm/cow/releases/tag/0.3) - 2026-09-05

### New contributors

- [@gsurkov](https://codeberg.org/gsurkov) contributed for the first time in
  [PR #340](https://codeberg.org/cow-wm/cow/pulls/340).
- [@nwgh](https://codeberg.org/nwgh) contributed for the first time in
  [PR #342](https://codeberg.org/cow-wm/cow/pulls/342).

### Added

- [PR](https://codeberg.org/cow-wm/cow/pulls/343): module: cowdiag ncurses TUI
  (render CoW/River diagnostics) —
  [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/336): Add declarative window
  styles — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/322): module: add cowclock —
  [@thomasadam](https://codeberg.org/thomasadam)

### Fixed

- [PR](https://codeberg.org/cow-wm/cow/pulls/360): menu: fix border and item
  alignment — [@gsurkov](https://codeberg.org/gsurkov)
- [PR](https://codeberg.org/cow-wm/cow/pulls/351): cowpager: keep the active
  desk border unobscured — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/347): decorations: retain handle
  size without titlebars — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/342): pointer op: fix build on
  FreeBSD — [@nwgh](https://codeberg.org/nwgh)
- [PR](https://codeberg.org/cow-wm/cow/pulls/341): cowident: free resources on
  exit — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/340): border: make mwm style look
  closer to original — [@gsurkov](https://codeberg.org/gsurkov)
- [PR](https://codeberg.org/cow-wm/cow/pulls/333): moocow: preserve shell
  arguments — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/320): kbd: report {caps,num}_lock
  status — [@thomasadam](https://codeberg.org/thomasadam)

### Changed

- [PR](https://codeberg.org/cow-wm/cow/pulls/355): resize: add automatic
  direction selection — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/348): cowpager: expose config
  options for window bg, etc. — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/330): bindings: support chorded
  key sequences — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/327): menu: add selection.relief =
  sunken — [@hakfoo](https://codeberg.org/hakfoo)
- [PR](https://codeberg.org/cow-wm/cow/pulls/326): edge_scroll: allow modifiers
  to enable scrolling — [@thomasadam](https://codeberg.org/thomasadam)

### Internal improvements

- [PR](https://codeberg.org/cow-wm/cow/pulls/323): modules: support shared
  output selectors — [@thomasadam](https://codeberg.org/thomasadam)

### Other changes

- [PR](https://codeberg.org/cow-wm/cow/pulls/361): resize: give borders a
  visual indication — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/358): site: preview forthcoming
  release — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/357): release: add contributor
  details to changelog — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/353): border: improve handle marks
  rendering — [@gsurkov](https://codeberg.org/gsurkov)
- [PR](https://codeberg.org/cow-wm/cow/pulls/352): style fixes —
  [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/349): cowpager: expose active desk
  border option — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/346): menus: select first item
  when opened — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/344): theme: add twm decor —
  [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/339): repo: update links for
  Codeberg organisation — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/337): site: highlight changed dev
  man pages — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/335): river init: generalise (via
  OpenBSD) — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/332): show: allow for command
  serialisation and output — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/331): keyboard-layout: report
  scroll lock state — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/328): cowiconman: make padding
  around icon list customizable — [@hakfoo](https://codeberg.org/hakfoo)
- [PR](https://codeberg.org/cow-wm/cow/pulls/325): rule: add pattern matching
  to script expr — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/324): Add cowiconman colour styles
  — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/321): decor: restore maximized
  button state — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/318): internal: state management
  improvements — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/317): commands: accept
  comma-separated page coordinates —
  [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/316): build: exclude more from
  tarball — [@thomasadam](https://codeberg.org/thomasadam)
- [PR](https://codeberg.org/cow-wm/cow/pulls/315): site: derive release
  information from tags — [@thomasadam](https://codeberg.org/thomasadam)

## [0.2](https://codeberg.org/cow-wm/cow/releases/tag/0.2) - 2026-08-26

### Fixed

- [PR](https://codeberg.org/cow-wm/cow/pulls/296): modules: keep status data
  off thread stacks
- [PR](https://codeberg.org/cow-wm/cow/pulls/279): Text: handle UTF-8
  centrally, and make modules use this
- [PR](https://codeberg.org/cow-wm/cow/pulls/272): menu: position correctly on
  screen
- [PR](https://codeberg.org/cow-wm/cow/pulls/268): kbd: emit status on layout
  changes

### Changed

- [PR](https://codeberg.org/cow-wm/cow/pulls/309): key-bindings: add a '-x'
  option to specify a command when the bind is released
- [PR](https://codeberg.org/cow-wm/cow/pulls/308): cowiconman: switch to using
  a config file
- [PR](https://codeberg.org/cow-wm/cow/pulls/293): menu: open submenus after
  keyboard delay
- [PR](https://codeberg.org/cow-wm/cow/pulls/292): menu: allow customisation of
  repeat keys
- [PR](https://codeberg.org/cow-wm/cow/pulls/281): cowbar: make waybar output
  configurable
- [PR](https://codeberg.org/cow-wm/cow/pulls/274): cowbuttons: allow for
  double-clicks, etc
- [PR](https://codeberg.org/cow-wm/cow/pulls/271): keyboard: add 'show
  keyboard-layout'
- [PR](https://codeberg.org/cow-wm/cow/pulls/263): maximize: add option to
  override reserved areas

### Other changes

- [PR](https://codeberg.org/cow-wm/cow/pulls/305): workflow: add release
  mechanics
- [PR](https://codeberg.org/cow-wm/cow/pulls/310): site: publish development
  and release manuals
- [PR](https://codeberg.org/cow-wm/cow/pulls/307): cursor: allow for setting
  theme and size
- [PR](https://codeberg.org/cow-wm/cow/pulls/306): parser: accept adjacent
  script blocks
- [PR](https://codeberg.org/cow-wm/cow/pulls/301): cowiconman: Import colours
  and font from theme
- [PR](https://codeberg.org/cow-wm/cow/pulls/302): titlebar: handle button
  release before pointer op
- [PR](https://codeberg.org/cow-wm/cow/pulls/303): refactor: Centralise common
  code
- [PR](https://codeberg.org/cow-wm/cow/pulls/291): Outputs: handle windows
  across non-linear geometries
- [PR](https://codeberg.org/cow-wm/cow/pulls/299): bindings: restore window
  context after decorations
- [PR](https://codeberg.org/cow-wm/cow/pulls/297): titlebar: finish button
  operation before dispatch
- [PR](https://codeberg.org/cow-wm/cow/pulls/287): move: retain focus after
  keyboard moves
- [PR](https://codeberg.org/cow-wm/cow/pulls/285): website: fix flex wrapping
- [PR](https://codeberg.org/cow-wm/cow/pulls/284): Window placement: empty
- [PR](https://codeberg.org/cow-wm/cow/pulls/280): website: publish HTML manual
  pages
- [PR](https://codeberg.org/cow-wm/cow/pulls/277): website: add release info
- [PR](https://codeberg.org/cow-wm/cow/pulls/276): Add Codeberg Pages website
- [PR](https://codeberg.org/cow-wm/cow/pulls/267): theme: add canoe
- [PR](https://codeberg.org/cow-wm/cow/pulls/265): keyboard: expose the layout
  via CoW IPC mechanism(s)
- [PR](https://codeberg.org/cow-wm/cow/pulls/266): cowrearrange: support
  maximized state
- [PR](https://codeberg.org/cow-wm/cow/pulls/261): colour: support RGBA values
- [PR](https://codeberg.org/cow-wm/cow/pulls/260): state: preserve stacking
  across restart
- [PR](https://codeberg.org/cow-wm/cow/pulls/257): Support titlebar buttons in
  bind-decoration
- [PR](https://codeberg.org/cow-wm/cow/pulls/255): README: clarify use of AI
- [PR](https://codeberg.org/cow-wm/cow/pulls/254): set next version

## [0.1] - 2026-08-10

### Added

- Implement containers
- Add page_cols/page_rows for virtual page grid
- Add page_vx/page_vy viewport to cow_desk
- Add vx/vy virtual position fields to cow_view
- Add page-aware visibility check
- Implement virtual coordinates
- Allow one positional arg for -c col row
- Persist vx/vy per desk on restart
- Add page info to status socket
- Add pager for cow
- Add IRC notifier
- Add specific after-start script
- Add titlebar.squeeze groundwork
- Teach titlebar.squeeze commands
- Accept titlebar.squeeze.*
- Add squeeze-related logic
- Add a means of showing window props
- Add shared utility helpers
- Introduce raise_on_focus config setting
- Add -h / -v for per-axis maximise
- Add 'show rules'
- Add geometry to container siblings
- Support double-click
- Track monitors more intuitively
- Introduce a new layout command
- Add on-layer-change event
- Add -r for repeatable bindings
- Add desk name support in show and status
- Add desk-name command
- Accept names in selectors and rules
- Persist names across restart
- Add 'set desks.count'
- Add minimal desk-name example
- Add on-title-change
- Add on-outputs-changed event
- Remember original output across migration cascades
- Add pick-window command
- Track single/double clicks closer
- Add util.h as option besides libutil.h and bsd/libutil.h
- Add on-start rule
- Add event loop base
- Accept status clients with libevent
- Add script command
- Add script AST
- Add script sources
- Accept bare targets
- Add DSL loop control
- Add moocow persistent mode
- Add moocow continuation prompt
- Make moocow prompt configurable
- Add moocow client banner id
- Add include command
- Add moocow local settings
- Add keep-size maximize
- Add CoW <-> module API
- Add ability to create colour gradients
- Add geometry window
- Allow for up to ten titlebar buttons
- Add decor image support
- Add FVWM-style theme examples
- Add theme preview gallery
- Add default icon image
- Add title_format option
- Support titlebar positions
- Support page-relative jumps
- Add nop command
- Publish theme status
- Add config files
- Publish current output
- Add circulate skip flag
- Expose window page membership
- Add conditional circulation
- Add window filters
- Add membership expressions
- Add popup menu support
- Add style configuration
- Add draw-exec placement command
- Support meta modifier bindings
- Support numbered selectors consistently
- Advertise window dimension bounds
- Support default theme settings
- Support viewport-relative move and resize
- Support per-menu styles
- Support solid configurable borders
- Add transient keyboard switcher
- Add configurable label format
- Add configurable sorting
- Allow disabling typeahead
- Add contextual key and mouse bindings
- Publish pointer-driven window movement
- Add structured logging foundation
- Support icons on menu items
- Detect the defined corner width even if the handles aren't visible
- Add interval script cells
- Make automatic placement configurable
- Add dependency-free XPM support
- Add -i argument to focus command
- Add per-button colour and relief settings
- Add titlebar text justification
- Add configurable border relief
- Add configurable label area
- Support pointer placement and icons
- Add configurable fonts
- Support deleting matching bindings
- Add TOML file

### Fixed

- Fix example
- Restore windows on pages when restarting
- Guard against no pointer
- Fix pointer detection
- Reapply on init
- Fix typos in user-visible strings
- Fix NULL dereferences
- Fix -Wformat-security warning
- Constrain relocated NW/NE corners
- Fix reference to cow.conf
- Reset when dragging windows
- Fix restart rule example
- Fix window resizing problem
- Keep sticky-page views static
- Quit -r: clear rules and run on-map/on-desk hooks
- Fix getops handling
- Fix panning issue
- Correct stop_repeat trigger
- Retain desk when moving windows
- Keep views alive when outputs go away
- Honour -k when combined with -o
- Keep -o on the requested output
- Re-home orphaned views to their original output
- Keep { ... } blocks as a single argument
- Reapply bindings
- Clamp pager to originating desk
- Fix typo
- Restore orphan desk pages
- Suppress panframes after output changes
- Keep brace blocks
- Fix attrs include order
- Fix maximize button relief
- Fix top highlight on titlebar
- Fix commands
- Fix grouped popup selection
- Avoid white relief sentinel
- Keep shaded side borders
- Avoid runtime profile overrides
- Clear focus on iconify
- Preserve targetted profile state
- Fix container drag highlight crash
- Honour rule-time placement
- Fix panel direction
- Keep windows on current page
- Keep focus after centering
- Preserve quoted command arguments
- Restore seats after focus loss
- Keep expanded windows unmaximized
- Fix resize arrow bindings
- Clear stale command errors
- Respect client size changes
- Clear SSD shape on window leave
- Preserve joins for edge gradients
- Reset profiles on config reload
- Keep gradient corners consistent
- Clear output cursor focus on destroy
- Fix left-side button hit tests
- Keep squeezed size when shaded
- Clear stale SSD cursor state
- Reset module surfaces on enter
- Restore previous focus on close
- Avoid raising close focus replacement
- Prevent menu keyboard reopen loop
- Honor river presentation hints
- Handle partially maximized windows
- Anchor windows before making them sticky
- Avoid redundant surface commits
- Fix stale binding context after menus close
- Fix window placement across outputs
- Correct vertical titlebar shading
- Correct per-button colour rendering
- Correct title justification handling
- Preserve inherited titlebar button style
- Keep gradient relief shading continuous
- Restore shaded window clipping
- Fix warnings

### Changed

- Initial commit
- Default to titlebar.inactive_colour
- Don't process desktop-configuration
- Assign to current desk when unstick occurs
- Titleheight
- Get River to notify of surface geometry
- Make dragging more visually obvious
- Make 'window-move -C' dual-purpose
- Make generic
- Update
- Unattach restores previous WxH
- On BSD set optind/optreset
- Sticky: add -S for across pages
- Undermouse: force on-screen
- Update panframes after scroll
- Always set SSDs when unfullscreening
- Config update
- Tidy up
- Clean up linkage and cow_ prefix naming
- Code layout
- Init: fix dereference issues
- Consolidate geometry / walk / clamp / filter helpers
- Move constants to config.h
- Tidy-up/style
- Treat '-0'/'+0' as absolute zero, not relative
- Interrupt top border squeezed titlebar
- Make top strip squeeze-aware
- Redo NE/NW corner logic
- Config: add example
- Call river internal API
- Maintain z-ordering
- Straddle pages if edge_scroll false
- Reduce number of processes
- Use shared helpers
- Split window-manager dispatchers
- Consolidate decoration drawing primitives
- Split view funcs to helpers
- Simplify state serialisation
- Consolidate pan-frame edge handling
- Unify command args parsing and selectors
- Consolidate IPC and binding plumbing
- Rename for naming consistency
- Replace [static N] array params with plain pointers
- Don't trigger panframe switch
- Rewrite as event-driven command rule subsystem
- Per-view config snapshots and event fan-out
- Don't crash on -t with uninitialised wm state
- Reject malformed -t specs instead of silently ignoring
- Cow: update rule examples
- Update rule syntax
- Pan the desk viewport by sub-page increments
- Interactive panning, surfaced in cowpager
- Don't apply page-strict culling
- Subsume the page command; remove cmd-page
- Separate out pre-cmd parsing
- Assign number
- Feature-parity with FvwmPager
- Status: indicate if output is current
- Bind clicks on border edges, etc.
- Register default decor binds
- Ship defaults as active
- Tile / cascade module
- Per-event JSON payloads
- Update logo
- Only the Cow.
- Update README.md
- Removes duplicated Anvil
- Prepare for using a list and not an array
- Document desk-name
- Optional -n to show desk names
- Make space for lowering windows
- Migrate views before destroying output
- Defer on-map/on-desk rules until dims
- Don't recalculate window position
- Fire once all outputs have settled
- Adopt orphans after the output's name is known
- Debounce on-outputs-changed firing
- Pointer-only edge arming during drags
- Show orange on panframe dwell
- Descope braces
- Focus window under cursor
- Visually keep win on output
- Check for the existance of util.h
- Port fallback to shm_open from src/shm.c
- Make desks.count operate on all outputs
- Desks.count: split into helpers
- Skip on-start after restart
- Use libevent for dispatch
- Use libevent in cowpager
- Refactor set handlers
- Split manage phases
- Use titlebar render context
- Share output JSON
- Clean container and layer docs
- Use squeeze paint context
- Aggregate options to a common struct
- Split colour setters
- Share set target helpers
- Fold squeeze setters
- Rename set helpers
- Trim set comments
- Table titlebar buttons
- Split desktop config setter
- Parse script blocks
- Execute script blocks
- Read multiline scripts
- Document scripts
- Share block scanner
- Fill script context
- Target outputs by number
- Man: minor update
- Show moocow prompt banner
- Reduce verbosity
- Use table for overrides
- Define a table to store them
- Move decor-related settings to new cmd
- Slight visual improvement
- Document theme includes in config
- Improve FVWM theme fidelity
- Make first-class objects
- Destroy stale window icons
- Initial implementation
- Group windows
- Filter windows
- Icon disable option
- Target iconified ids
- Improve mwm border style
- Skip fixed-size drags
- New module
- Style cowbuttons
- Cowbuttons; add panels
- Panel toggles
- Panel buttons
- Toggle panels
- External panels
- Drive panels from config
- Refresh launcher example
- Manpage update
- Follow FvwmPager monitor layout
- Share module config parsing
- Manpage tweaks
- Filter displayed desks
- Use layer-shell background
- Compose with pending resize
- Hide off-page fullscreen windows
- Fit windows to usable area
- Update cursor over decorations
- Mark resized layouts tiled
- Clean up views on shutdown
- Update decor example
- Render border gradients in frame space
- Shade gradient border relief
- Show titlebar gradient styling
- Document configuration
- Close unrelated submenus
- Namespace sockets by display
- Prefer transient parents after close
- Share command string body handling
- Cosmetic cleanup
- Defer command maximized hints
- Hide pointer after idle timeout
- Use river layer shell usable area
- Defer client state protocol requests
- Use river xkb modifier updates
- Reflow on usable area changes
- Centralize frame hit testing
- Separate raw pointer handling from pan frames
- Align corner hit regions with rendering
- Centralize percent conversion parsing
- Show location app and title by default
- Defer actions until popup teardown
- Filter entries during typeahead
- Separate viewport and usable geometry
- Restack nodes after live refresh
- Use application icons by default
- Draw application icons in window miniatures
- Improve default label appearance
- Convert window manager diagnostics
- Record window manager lifecycle events
- Trace command and rule execution
- Trace compositor state transitions
- Identify command and status clients
- Toggle output with SIGUSR2
- Reduce routine output verbosity
- Binding; fix root bindings after leaving background
- Centralise path expansion
- Use formatted window titles for tabs
- Separate titlebar and border styles
- Propagate new titlebar settings to views
- Simplify vertical titlebar relief shading
- Centralise relief value handling
- Draw focused windows above all others
- Centralise cached image rendering
- Optionally display application icons
- Centralise command argument quoting
- Use native menus for grouped windows
- Notify when desk count changes
- Initialise script sources before loading
- Tweak default config
- 0.1

### Removed

- Remove use of conky
- Remove action in favour of exec
- Remove debug
- Remove dead code
- Remove inline functions and strip prototype argument names
- Remove generic pointer cleanup
- Deprecate the root-menu command

### Documentation

- Fix typo and add IRC information
- Formatting fixes
- Add container screenshot
- Update
- Link to video
- Add link to wiki
- Fix typo
- Update docs
- Add pager screenshot
- Remove references to action cmd
- Document titlebar.squeeze.* options
- Rewrite manpage
- Document scroll, retire page
- Document bind-decoration
- Remove duplicate
- Add new logos
- Document raise_on_focus
- Cow vs cow vs bull
- Update for decor
- Document CoW/River relationship
- Tweak -F focus example
- Clarify page filter scope
- Explain focus filter scope
- Fix typos
- Update for packages
- Minor tweaks
- Document menu item icons
- Describe configurable icon placement
- Sync to next release

### Build and packaging

- Support BSD
- Exclude .cache/ directory
- Fix build
- Make checks granular
- Tidy up file locations
- Add build files
- Add shell.nix
- Move certain applications to modules/
- Add flake.nix
- Add NixOS support
- Correct licence to ISC
- Use fedora/latest rather than rawhide
- Avoid Fedora multilib dependency conflicts
- Notify IRC of SourceHut failures

[0.1]: https://codeberg.org/cow-wm/cow/src/tag/0.1

<!-- generated by git-cliff -->
