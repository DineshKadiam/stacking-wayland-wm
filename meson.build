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

srcs = [
    'src/arguments.c',
    'src/border.c',
    'src/cmd-bind.c',
    'src/cmd-container.c',
    'src/cmd-ctx.c',
    'src/cmd-desk.c',
    'src/cmd-page.c',
    'src/cmd-exec.c',
    'src/cmd-focus.c',
    'src/cmd-list-commands.c',
    'src/cmd-list.c',
    'src/cmd-mouse.c',
    'src/cmd-move.c',
    'src/cmd-queue.c',
    'src/cmd-quit.c',
    'src/cmd-resize.c',
    'src/cmd-rule.c',
    'src/cmd-set.c',
    'src/cmd-show.c',
    'src/cmd-string.c',
    'src/cmd-window.c',
    'src/cmd.c',
    'src/colour.c',
    'src/command.c',
    'src/config.c',
    'src/cow_box.c',
    'src/decoration.c',
    'src/desk_ops.c',
    'src/font.c',
    'src/ipc.c',
    'src/keybinding.c',
    'src/layer_shell.c',
    'src/main.c',
    'src/memory.c',
    'src/output.c',
    'src/pan_frames.c',
    'src/pointer_op.c',
    'src/seat.c',
    'src/shm.c',
    'src/state.c',
    'src/status.c',
    'src/titlebar.c',
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

# getopts on BSD needs optreset handling.
if cc.has_header_symbol('unistd.h', 'optreset')
    add_project_arguments('-DHAVE_OPTRESET', language: 'c')
endif

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

libbsd = dependency('libbsd', required: false)
if libbsd.found()
    add_project_arguments('-DHAVE_LIBBSD', language: 'c')
endif

all_deps = [
    wayland_client,
    xkbcommon,
    pangocairo,
    cairo,
]
if libbsd.found()
    all_deps += libbsd
endif

if m_dep.found()
    all_deps += m_dep
endif

if hostsys == 'freebsd'
    add_project_link_arguments('-lutil', language: 'c')
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
)

include_dirs = [
    include_directories('.'),
    include_directories('include'),
    include_directories('vendor'),
]

executable('cow',
    srcs + generated,
    include_directories: include_dirs,
    dependencies: all_deps,
    install: true,
)

executable('moocow',
    'src/moocow.c',
    install: true,
)

subdir('cowbar')
subdir('cowpager')

install_data(
    'share/wayland-sessions/cow.desktop',
    install_dir: get_option('datadir') / 'wayland-sessions',
)

install_data(
    'config/cow.conf',
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
    foreach page : [['cow', '1'], ['moocow', '1'], ['cowbar', '1'], ['cowpager', '1']]
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
    'scdoc': scdoc,
}, bool_yn: true, section: 'Dependencies')
