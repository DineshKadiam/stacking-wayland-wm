# cowbuttons -- FvwmButtons-style button panel for cow.

buttons_generated = wl_mod.scan_xml(
    wl_proto_dir / 'stable/xdg-shell/xdg-shell.xml',
    wl_proto_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
    wl_proto_dir / 'unstable/tablet/tablet-unstable-v2.xml',
)

buttons_deps = [
    dependency('wayland-client'),
    dependency('cairo'),
    dependency('pangocairo'),
    dependency('libevent'),
    m_dep,
]

buttons_common_sources = [
    '../../src/colour.c',
    '../../src/image.c',
    '../../src/icon_lookup.c',
    '../../src/xpm.c',
    '../../src/memory.c',
    '../../src/module.c',
    '../../src/module_status.c',
    '../../src/module_shm.c',
    '../../src/shm_util.c',
    '../../src/module_cursor.c',
    '../../src/module_config.c',
    '../../src/output_selector.c',
    '../../src/module_draw.c',
    '../../src/text.c',
    '../../src/util.c',
    '../../src/vector.c',
    '../../vendor/cJSON.c',
]
buttons_include_dirs = [
        include_directories('../..'),
        include_directories('../../vendor'),
        include_directories('../../include'),
]
cowbuttons = executable('cowbuttons',
    ['src/cowbuttons.c'] + buttons_generated + buttons_common_sources,
    include_directories: buttons_include_dirs,
    dependencies: buttons_deps,
    install: true,
)
buttons_applications_test = executable('test-buttons-applications',
    ['../../tests/buttons-applications.c'] + buttons_generated + buttons_common_sources,
    include_directories: buttons_include_dirs,
    dependencies: buttons_deps,
)
