project(
    'cow',
    'c',
    license: 'ISC',
    default_options: ['c_std=c99', 'warning_level=2', 'werror=false'],
    version: run_command('./tools/cow-version-str.sh',
        check: false
    ).stdout().strip(),
    meson_version: '>= 1.3.0',
)

hostsys = host_machine.system()

# ----------------------------------------------------------------------
# Portability probes
#
# Rather than branching on host_machine.system(), probe for what we
# actually need (headers + functions) and record the results in a
# generated include/cow/build_config.h that is consumed by
# cow/compat.h.  This lets Linux-without-libbsd, FreeBSD, and any
# other BSD-like system find their own natural set of headers
# without the source hard-coding the distinction.
# ----------------------------------------------------------------------

conf = configuration_data()

srcs = [
    'src/alias.c',
    'src/app_icon.c',
    'src/arguments.c',
    'src/border.c',
    'src/cmd-alias.c',
    'src/cmd-args.c',
    'src/cmd-bind.c',
    'src/cmd-bind-decoration.c',
    'src/cmd-container.c',
    'src/cmd-ctx.c',
    'src/cmd-decor.c',
    'src/cmd-desk.c',
    'src/cmd-desk-name.c',
    'src/cmd-draw-exec.c',
    'src/cmd-exec.c',
    'src/cmd-filter.c',
    'src/cmd-focus.c',
    'src/cmd-icon.c',
    'src/cmd-icon-placement.c',
    'src/cmd-include.c',
    'src/cmd-keyboard-layout.c',
    'src/cmd-list.c',
    'src/cmd-menu.c',
    'src/cmd-mouse.c',
    'src/menu.c',
    'src/menu_choice.c',
    'src/cmd-move.c',
    'src/cmd-nop.c',
    'src/cmd-pick-window.c',
    'src/cmd-queue.c',
    'src/cmd-quit.c',
    'src/cmd-resize.c',
    'src/cmd-rule.c',
    'src/cmd-script.c',
    'src/cmd-scan.c',
    'src/cmd-scroll.c',
    'src/cmd-send-key.c',
    'src/cmd-selection.c',
    'src/cmd-set.c',
    'src/cmd-show.c',
    'src/cmd-string.c',
    'src/cmd-style.c',
    'src/cmd-window.c',
    'src/cmd-window-list.c',
    'src/cmd.c',
    'src/dsl-ast.c',
    'src/dsl-parse.c',
    'src/dsl-eval.c',
    'src/state-snapshot.c',
    'src/state-value.c',
    'src/colour.c',
    'src/command.c',
    'src/config-serialize.c',
    'src/config.c',
    'src/config_types.c',
    'src/cow_box.c',
    'src/decoration.c',
    'src/decoration-tiles.c',
    'src/decoration_hit.c',
    'src/desk.c',
    'src/desk_ops.c',
    'src/diagnostics.c',
    'src/format.c',
    'src/draw.c',
    'src/draw_exec.c',
    'src/event.c',
    'src/font.c',
    'src/geometry_window.c',
    'src/gradient.c',
    'src/image.c',
    'src/icon_placement.c',
    'src/ipc.c',
    'src/json_util.c',
    'src/keyboard_layout.c',
    'src/keybinding.c',
    'src/layer_shell.c',
    'src/main.c',
    'src/memory.c',
    'src/module_protocol.c',
    'src/output.c',
    'src/output_selector.c',
    'src/options-table.c',
    'src/pan_frames.c',
    'src/placement.c',
    'src/pointer_input.c',
    'src/pointer_op.c',
    'src/policy.c',
    'src/rule.c',
    'src/selection.c',
    'src/seat.c',
    'src/shm.c',
    'src/shm_util.c',
    'src/stack.c',
    'src/state.c',
    'src/status.c',
    'src/style.c',
    'src/text.c',
    'src/titlebar.c',
    'src/update.c',
    'src/util.c',
    'src/vector.c',
    'src/view.c',
    'src/container.c',
    'src/container_state.c',
    'src/container_empty.c',
    'src/container_layout.c',
    'src/virtual_keyboard.c',
    'src/wm.c',
    'src/xpm.c',
    'vendor/cow_log.c',
    'vendor/cJSON.c',
]

prefix     = get_option('prefix')
datadir    = get_option('datadir')
etc_prefix = get_option('etcprefix')
if etc_prefix == ''
    etc_prefix = prefix
endif

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)

# DSL parser / lexer (script { ... } blocks).  Generated at build
# time; the produced .c sources are added to the cow executable
# below, and the .h headers land in build/ so dsl-eval.c and
# dsl-lexer.c can include them.
bison_prog = find_program('bison', version: '>=3.0', required: true)
flex_prog  = find_program('flex', version: '>=2.5.39', required: true)

dsl_parser_gen = custom_target('dsl-parser',
    input:   'src/dsl-parser.y',
    output:  ['dsl-parser.c', 'dsl-parser.h'],
    command: [bison_prog,
        '-d',
        '-p', 'dsl_yy',
        '--defines=@OUTPUT1@',
        '-o', '@OUTPUT0@',
        '@INPUT@',
    ],
)

dsl_lexer_gen = custom_target('dsl-lexer',
    input:   'src/dsl-lexer.l',
    output:  ['dsl-lexer.c', 'dsl-lexer.h'],
    command: [flex_prog,
        '-P', 'dsl_yy',
        '--header-file=@OUTPUT1@',
        '-o', '@OUTPUT0@',
        '@INPUT@',
    ],
)

add_project_arguments(
    [
        '-D_GNU_SOURCE',
        '-DCOW_VERSION="' + meson.project_version() + '"',
        '-DCOW_SYSCONFDIR="' + etc_prefix + '/etc/cow"',
        '-DCOW_DATADIR="' + prefix / datadir / 'cow"',
    ],
    language: 'c',
)

meson.add_dist_script('tools/dist-tarball.sh')

if hostsys == 'linux'
    add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')
endif

# libbsd provides bsd/*.h on Linux (strlcpy, strtonum, fparseln, queue.h,
# tree.h, libutil.h, etc.).  Optional: a BSD host provides these natively.
libbsd = dependency('libbsd', required: false)
conf.set10('HAVE_LIBBSD', libbsd.found())

# Header probes.  For each, prefer the native header; fall back to the
# libbsd overlay header (which includes the -I bsd/ path when libbsd is
# linked).  compat.h picks the right include based on these.
foreach pair : [
    ['sys/queue.h',  'HAVE_SYS_QUEUE_H'],
    ['sys/tree.h',   'HAVE_SYS_TREE_H'],
    ['libutil.h',    'HAVE_LIBUTIL_H'],
    ['util.h',       'HAVE_UTIL_H'],
]
    conf.set10(pair[1], cc.has_header(pair[0]))
endforeach

# glibc's <sys/queue.h> is a skinny subset that lacks macros like
# TAILQ_FOREACH_SAFE which the code relies on.  Probe specifically so
# compat.h can prefer <bsd/sys/queue.h> on those systems.
conf.set10('HAVE_TAILQ_FOREACH_SAFE',
    cc.has_header_symbol('sys/queue.h', 'TAILQ_FOREACH_SAFE'))

# libbsd ships parallel headers under bsd/.  Probe independently so a
# system with partial libbsd (e.g. only bsd/string.h installed) still
# works.
foreach pair : [
    ['bsd/sys/queue.h', 'HAVE_BSD_SYS_QUEUE_H'],
    ['bsd/sys/tree.h',  'HAVE_BSD_SYS_TREE_H'],
    ['bsd/libutil.h',   'HAVE_BSD_LIBUTIL_H'],
    ['bsd/string.h',    'HAVE_BSD_STRING_H'],
    ['bsd/stdlib.h',    'HAVE_BSD_STDLIB_H'],
]
    conf.set10(pair[1], cc.has_header(pair[0], dependencies: libbsd))
endforeach

# Linux kernel input codes vs. FreeBSD evdev port.  OpenBSD has neither,
# so compat.h falls back to the small set of BTN_* values cow uses.
have_linux_input = cc.has_header('linux/input-event-codes.h')
have_bsd_input   = cc.has_header('dev/evdev/input-event-codes.h')
conf.set10('HAVE_LINUX_INPUT_EVENT_CODES_H', have_linux_input)
conf.set10('HAVE_DEV_EVDEV_INPUT_EVENT_CODES_H', have_bsd_input)

# Function probes.  The same function may live in libc (BSDs), in
# libbsd under a bsd/ prefix (Linux with libbsd installed, non-overlay
# variant) or in libutil (fparseln, below).  Probe against the native
# header first, then against the bsd/-prefixed header if libbsd is
# available, and record whichever succeeds.
foreach sym : [
    ['strlcpy',      'HAVE_STRLCPY',      'string.h'],
    ['strlcat',      'HAVE_STRLCAT',      'string.h'],
    ['strtonum',     'HAVE_STRTONUM',     'stdlib.h'],
    ['reallocarray', 'HAVE_REALLOCARRAY', 'stdlib.h'],
]
    found = cc.has_function(sym[0],
        prefix: '#include <' + sym[2] + '>')
    if not found and libbsd.found()
        found = cc.has_function(sym[0],
            prefix: '#include <bsd/' + sym[2] + '>',
            dependencies: libbsd)
    endif
    conf.set10(sym[1], found)
endforeach

# memfd_create is Linux-only; shm_open is POSIX.  shm.c picks the
# memfd path when available and falls back to shm_open+unlink otherwise.
conf.set10('HAVE_MEMFD_CREATE',
    cc.has_function('memfd_create',
        prefix: '#define _GNU_SOURCE\n#include <sys/mman.h>'))

# fparseln is declared in <libutil.h> (BSDs) or <bsd/libutil.h>
# (Linux with libbsd).  On FreeBSD it lives in /usr/lib/libutil.so
# from the base system; on Linux the libbsd build supplies it.
#
# A compile-time cc.has_function() probe is unreliable here: on
# FreeBSD the declaration in <libutil.h> is not always visible in
# the probe TU that meson generates, even though the linker
# resolves fparseln from -lutil without issue at build time.  Stop
# probing for the function and treat the presence of a providing
# library (libutil on BSDs, libbsd on Linux) as sufficient.
libutil = cc.find_library('util', required: false)
have_fparseln = libutil.found() or libbsd.found()
conf.set10('HAVE_FPARSELN', have_fparseln)
if not have_fparseln
    error(
        'fparseln() is required but neither libutil nor libbsd '
        + 'was found.  Install libbsd (Linux) or ensure libutil '
        + '(BSD base system) is available.'
    )
endif

# getopts on BSD needs optreset handling.
conf.set10('HAVE_OPTRESET',
    cc.has_header_symbol('unistd.h', 'optreset'))

build_config_h = configure_file(
    output: 'build_config.h',
    configuration: conf,
    install: false,
)

if get_option('cow_debug')
    add_project_arguments(['-g', '-O0', '-fsanitize=address'], language: 'c')
    add_project_link_arguments(['-fsanitize=address'], language: 'c')
else
    add_project_arguments(['-DNDEBUG'], language: 'c')
endif

# Dependencies
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
wayland_protos = dependency('wayland-protocols', version: '>=1.41', default_options: ['tests=false'])
xkbcommon      = dependency('xkbcommon')
pangocairo     = dependency('pangocairo')
cairo          = dependency('cairo')
libevent       = dependency('libevent')

all_deps = [
    wayland_client,
    wayland_cursor,
    xkbcommon,
    pangocairo,
    cairo,
    libevent,
]
if libbsd.found()
    all_deps += libbsd
endif
if libutil.found()
    all_deps += libutil
endif

if m_dep.found()
    all_deps += m_dep
endif

# Protocol header/code generation using Meson's wayland module.
# The wayland module was stabilized in meson 1.8.0; older
# versions require 'unstable-wayland'.
if meson.version().version_compare('>= 1.8.0')
    wl_mod = import('wayland')
else
    wl_mod = import('unstable-wayland')
endif

wl_proto_dir = wayland_protos.get_variable('pkgdatadir')

generated = wl_mod.scan_xml(
    'protocol/river-window-management-v1.xml',
    'protocol/river-xkb-bindings-v1.xml',
    'protocol/river-xkb-config-v1.xml',
    'protocol/river-virtual-keyboard-v1.xml',
    'protocol/virtual-keyboard-unstable-v1.xml',
    'protocol/river-layer-shell-v1.xml',
    wl_proto_dir / 'stable/xdg-shell/xdg-shell.xml',
    wl_proto_dir / 'stable/viewporter/viewporter.xml',
    'protocol/wlr-layer-shell-unstable-v1.xml',
    'protocol/pointer-constraints-unstable-v1.xml',
    'protocol/tablet-unstable-v2.xml',
    'protocol/cursor-shape-v1.xml',
    wl_proto_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml',
)

include_dirs = [
    # include_directories('.') covers both the source root and the
    # build root, so the generated build_config.h is found here.
    include_directories('.'),
    include_directories('include'),
    include_directories('src'),
    include_directories('vendor'),
]

cow = executable('cow',
    srcs + generated + [dsl_parser_gen, dsl_lexer_gen],
    include_directories: include_dirs,
    dependencies: all_deps,
    install: true,
)

test('command-print', find_program('sh'),
    args: [files('tests/command-print.sh'), cow],
)

# Exercise real WM state without connecting to a compositor. Reuse CoW's
# objects, replacing only its entry point with the test harness.
wm_test_sources = []
foreach source : srcs
    if source != 'src/main.c'
        wm_test_sources += source
    endif
endforeach
wm_test_protocols = static_library('wm-test-protocols',
    generated + [dsl_parser_gen, dsl_lexer_gen],
    include_directories: include_dirs,
    dependencies: all_deps,
)
# Linking the protocol library does not order test compilation after header
# generation. Add headers as sources without compiling protocol code twice.
wm_test_headers = [dsl_parser_gen[1], dsl_lexer_gen[1]]
foreach source : generated
    if source.full_path().endswith('.h')
        wm_test_headers += source
    endif
endforeach
# Private-helper tests include one implementation directly; omit its object.
foreach name, replacement : {
    'pan-frames': '',
    'decoration-tiles': 'src/decoration-tiles.c',
    'alias-args': '',
    'send-key': '',
    'container': '',
    'container-state': '',
    'style-state': '',
    'arguments': 'src/arguments.c',
    'config-layers': '',
    'icon-hover': 'src/view.c',
    'maximize-page': 'src/view.c',
    'resize-direction': 'src/cmd-resize.c',
    'window-list': 'src/cmd-window-list.c',
}
    test_sources = []
    foreach source : wm_test_sources
        if source != replacement
            test_sources += source
        endif
    endforeach
    runner = executable('test-' + name,
        'tests/' + name + '.c',
        wm_test_headers,
        objects: cow.extract_objects(test_sources),
        link_with: wm_test_protocols,
        include_directories: include_dirs,
        dependencies: all_deps,
    )
    test(name, runner,
        args: name == 'config-layers' ? files('config/cow/cow-defaults.conf') : [],
    )
endforeach

xpm_test = executable('test-xpm',
    'tests/test_xpm.c',
    'src/xpm.c',
    'src/memory.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('xpm', xpm_test,
    args: [
        meson.current_source_dir() / 'tests/data/basic.xpm',
        meson.current_source_dir() / 'tests/data/invalid.xpm',
    ],
)

executable('moocow',
    'src/moocow.c',
    'src/cmd-scan.c',
    'src/module.c',
    'src/util.c',
    'vendor/cJSON.c',
    include_directories: include_dirs,
    install: true,
)

icon_placement_test = executable('test-icon-placement',
    'tests/icon-placement.c',
    'src/icon_placement.c',
    'src/memory.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('icon-placement', icon_placement_test)

selection_test = executable('test-selection',
    'tests/selection.c',
    'src/selection.c',
    'src/memory.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('selection', selection_test)

command_quote_test = executable('test-command-quote',
    'tests/command-quote.c',
    'src/util.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('command-quote', command_quote_test)

command_parse_test = executable('test-command-parse',
    'tests/command-parse.c',
    'src/cmd-string.c',
    'src/cmd-scan.c',
    'src/memory.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('command-parse', command_parse_test)

dsl_parse_test = executable('test-dsl-parse',
    'tests/dsl-parse.c',
    'src/dsl-ast.c',
    'src/dsl-parse.c',
    'src/memory.c',
    dsl_parser_gen,
    dsl_lexer_gen,
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('dsl-parse', dsl_parse_test)

colour_test = executable('test-colour',
    'tests/colour.c',
    'src/colour.c',
    'src/memory.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('colour', colour_test)

text_test = executable('test-text',
    'tests/text.c',
    'src/text.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('text', text_test)

placement_test = executable('test-placement',
    'tests/placement.c',
    'src/placement.c',
    'src/memory.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('placement', placement_test)

config_types_test = executable('test-config-types',
    'tests/config-types.c',
    'src/config_types.c',
    'src/memory.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('config-types', config_types_test)

options_table_test = executable('test-options-table',
    'tests/options-table.c',
    'src/options-table.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('options-table', options_table_test)

cow_box_test = executable('test-cow-box',
    'tests/cow-box.c',
    'src/cow_box.c',
    'vendor/cJSON.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('cow-box', cow_box_test)

container_layout_test = executable('test-container-layout',
    'tests/container-layout.c',
    'src/container_layout.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('container-layout', container_layout_test)

update_test = executable('test-update',
    'tests/update.c',
    'src/update.c',
    'src/diagnostics.c',
    'vendor/cJSON.c',
    'vendor/cow_log.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('update', update_test)

diagnostics_test = executable('test-diagnostics',
    'tests/diagnostics.c',
    'src/diagnostics.c',
    'vendor/cJSON.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('diagnostics', diagnostics_test)

cowdiag_proc_test = executable('test-cowdiag-proc',
    'tests/cowdiag-proc.c',
    'modules/cowdiag/src/proc.c',
    include_directories: [include_dirs,
        include_directories('modules/cowdiag/src')],
    dependencies: wayland_client,
)
test('cowdiag-proc', cowdiag_proc_test)

output_selector_test = executable('test-output-selector',
    'tests/output-selector.c',
    'src/module_output.c',
    'src/output_selector.c',
    'vendor/cJSON.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('output-selector', output_selector_test)

module_theme_test = executable('test-module-theme',
    'tests/module-theme.c',
    'src/module_theme.c',
    'src/colour.c',
    'src/memory.c',
    'vendor/cJSON.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('module-theme', module_theme_test)

module_snapshot_test = executable('test-module-snapshot',
    'tests/module-snapshot.c',
    'src/module_snapshot.c',
    'src/module_theme.c',
    'src/json_util.c',
    'src/colour.c',
    'src/memory.c',
    'vendor/cJSON.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('module-snapshot', module_snapshot_test)

module_draw_test = executable('test-module-draw',
    'tests/module-draw.c',
    'src/module_draw.c',
    'src/colour.c',
    'src/memory.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('module-draw', module_draw_test)

shared_utils_test = executable('test-shared-utils',
    'tests/shared-utils.c',
    'src/json_util.c',
    'src/shm_util.c',
    'vendor/cJSON.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('shared-utils', shared_utils_test)

state_value_test = executable('test-state-value',
    'tests/state-value.c',
    'src/state-value.c',
    'src/memory.c',
    'vendor/cJSON.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('state-value', state_value_test)

config_serialize_test = executable('test-config-serialize',
    'tests/config-serialize.c',
    'src/config-serialize.c',
    'src/state-value.c',
    'src/util.c',
    'src/memory.c',
    'vendor/cJSON.c',
    include_directories: include_dirs,
    dependencies: all_deps,
)
test('config-serialize', config_serialize_test)

subdir('cowbar')
subdir('modules/cowclock')
subdir('modules/cowdiag')
subdir('modules/cowbuttons')
subdir('modules/cowident')
subdir('modules/cowiconman')
subdir('modules/cowpager')
subdir('modules/cowrearrange')

install_data(
    'share/wayland-sessions/cow.desktop',
    install_dir: get_option('datadir') / 'wayland-sessions',
)

install_data(
    'config/cow/cow.conf',
    'config/cow/cowbar.conf',
    'config/cow/cowclock.conf',
    install_dir: etc_prefix + '/etc/cow',
)

install_data(
    'config/cow/cow-defaults.conf',
    install_dir: datadir / 'cow',
)

install_data(
    'share/cow/icons/default.png',
    install_dir: datadir / 'cow/icons',
)

man_conf = configuration_data()
man_conf.set('VERSION', meson.project_version())
man_conf.set('DATE', run_command(
    'date', '+%Y-%m-%d',
    check: true,
).stdout().strip())

lowdown = find_program('lowdown', native: true, required: false)
if lowdown.found()
    lowdown_man = find_program('tools/lowdown-man.sh')
    foreach page : [
        ['cow', '1'], ['moocow', '1'], ['cowbar', '1'],
        ['cowbuttons', '1'], ['cowclock', '1'], ['cowdiag', '1'],
        ['cowident', '1'], ['cowiconman', '1'], ['cow-module-cmd', '1'],
        ['cowpager', '1'], ['cowrearrange', '1'],
    ]
        name    = page[0]
        section = page[1]
        markdown = configure_file(
            input:         'man/@0@.@1@.md.in'.format(name, section),
            output:        '@0@.@1@.md'.format(name, section),
            configuration: man_conf,
        )
        custom_target(
            '@0@.@1@'.format(name, section),
            input:   markdown,
            output:  '@0@.@1@'.format(name, section),
            command: [lowdown_man, lowdown, '@INPUT@'],
            capture: true,
            install: true,
            install_dir: get_option('mandir') / 'man@0@'.format(section),
        )
    endforeach
endif

summary({
    'wayland-client': wayland_client,
    'xkbcommon': xkbcommon,
    'pangocairo': pangocairo,
    'cairo': cairo,
    'libbsd': libbsd,
    'libevent': libevent,
    'ncursesw (cowdiag)': cowdiag_ncurses,
    'lowdown': lowdown,
}, bool_yn: true, section: 'Dependencies')
