project(
    'cow',
    'c',
    license: 'GPL-3.0-or-later',
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
    'src/arguments.c',
    'src/border.c',
    'src/cmd-args.c',
    'src/cmd-bind.c',
    'src/cmd-bind-decoration.c',
    'src/cmd-container.c',
    'src/cmd-ctx.c',
    'src/cmd-desk.c',
    'src/cmd-desk-name.c',
    'src/cmd-exec.c',
    'src/cmd-focus.c',
    'src/cmd-include.c',
    'src/cmd-list-commands.c',
    'src/cmd-list.c',
    'src/cmd-mouse.c',
    'src/cmd-move.c',
    'src/cmd-pick-window.c',
    'src/cmd-queue.c',
    'src/cmd-quit.c',
    'src/cmd-resize.c',
    'src/cmd-rule.c',
    'src/cmd-script.c',
    'src/cmd-scan.c',
    'src/cmd-scroll.c',
    'src/cmd-set.c',
    'src/cmd-show.c',
    'src/cmd-string.c',
    'src/cmd-window.c',
    'src/cmd.c',
    'src/dsl-ast.c',
    'src/dsl-eval.c',
    'src/dsl-sources.c',
    'src/dsl-value.c',
    'src/colour.c',
    'src/command.c',
    'src/config.c',
    'src/cow_box.c',
    'src/decoration.c',
    'src/desk.c',
    'src/desk_ops.c',
    'src/draw.c',
    'src/event.c',
    'src/font.c',
    'src/ipc.c',
    'src/json_util.c',
    'src/keybinding.c',
    'src/layer_shell.c',
    'src/main.c',
    'src/memory.c',
    'src/output.c',
    'src/pan_frames.c',
    'src/pointer_op.c',
    'src/rule.c',
    'src/seat.c',
    'src/shm.c',
    'src/stack.c',
    'src/state.c',
    'src/status.c',
    'src/titlebar.c',
    'src/util.c',
    'src/vector.c',
    'src/view.c',
    'src/wm.c',
    'vendor/cJSON.c',
]

prefix     = get_option('prefix')
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
flex_prog  = find_program('flex',  version: '>=2.6', required: true)

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
    ],
    language: 'c',
)

if hostsys == 'linux'
    add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')
endif

# ----------------------------------------------------------------------
# Feature probes -> include/cow/build_config.h
# ----------------------------------------------------------------------

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

# Linux kernel input codes vs. FreeBSD evdev port.  We need exactly
# one of these to get BTN_LEFT / KEY_Return / etc.
have_linux_input = cc.has_header('linux/input-event-codes.h')
have_bsd_input   = cc.has_header('dev/evdev/input-event-codes.h')
conf.set10('HAVE_LINUX_INPUT_EVENT_CODES_H', have_linux_input)
conf.set10('HAVE_DEV_EVDEV_INPUT_EVENT_CODES_H', have_bsd_input)
if not (have_linux_input or have_bsd_input)
    error(
        'Neither <linux/input-event-codes.h> nor '
        + '<dev/evdev/input-event-codes.h> was found.  '
        + 'cow needs one of them for BTN_*/KEY_* constants.  '
        + 'On FreeBSD install devel/evdev-proto.'
    )
endif

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
wayland_protos = dependency('wayland-protocols', version: '>=1.41', default_options: ['tests=false'])
xkbcommon      = dependency('xkbcommon')
pangocairo     = dependency('pangocairo')
cairo          = dependency('cairo')
libevent       = dependency('libevent')

all_deps = [
    wayland_client,
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
    'protocol/river-layer-shell-v1.xml',
    wl_proto_dir / 'stable/xdg-shell/xdg-shell.xml',
    'protocol/wlr-layer-shell-unstable-v1.xml',
    'protocol/pointer-constraints-unstable-v1.xml',
    'protocol/tablet-unstable-v2.xml',
    'protocol/cursor-shape-v1.xml',
)

include_dirs = [
    # include_directories('.') covers both the source root and the
    # build root, so the generated build_config.h is found here.
    include_directories('.'),
    include_directories('include'),
    include_directories('src'),
    include_directories('vendor'),
]

executable('cow',
    srcs + generated + [dsl_parser_gen, dsl_lexer_gen],
    include_directories: include_dirs,
    dependencies: all_deps,
    install: true,
)

executable('moocow',
    'src/moocow.c',
    'src/cmd-scan.c',
    include_directories: include_dirs,
    install: true,
)

subdir('cowbar')
subdir('cowident')
subdir('cowpager')
subdir('cowrearrange')

install_data(
    'share/wayland-sessions/cow.desktop',
    install_dir: get_option('datadir') / 'wayland-sessions',
)

install_data(
    'config/cow/cow.conf',
    install_dir: etc_prefix + '/etc/cow',
)

install_data(
    'tools/start.sh',
    install_dir: get_option('bindir'),
    install_mode: 'rwxr-xr-x',
    rename: 'cow-start',
)

man_conf = configuration_data()
man_conf.set('VERSION', meson.project_version())

scdoc = dependency('scdoc', native: true, required: false)
if scdoc.found()
    scdoc_prog = find_program(scdoc.get_variable(pkgconfig: 'scdoc'), native: true)
    foreach page : [['cow', '1'], ['moocow', '1'], ['cowbar', '1'], ['cowident', '1'], ['cowpager', '1'], ['cowrearrange', '1']]
        name    = page[0]
        section = page[1]
        scd = configure_file(
            input:         'man/@0@.@1@.scd.in'.format(name, section),
            output:        '@0@.@1@.scd'.format(name, section),
            configuration: man_conf,
        )
        custom_target(
            '@0@.@1@'.format(name, section),
            input:   scd,
            output:  '@0@.@1@'.format(name, section),
            command: scdoc_prog,
            feed:    true,
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
    'scdoc': scdoc,
}, bool_yn: true, section: 'Dependencies')
