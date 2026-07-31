# cowbuttons -- FvwmButtons-style button panel for cow.

buttons_generated = wl_mod.scan_xml(
    wl_proto_dir / 'stable/xdg-shell/xdg-shell.xml',
    wl_proto_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
    wl_proto_dir / 'unstable/tablet/tablet-unstable-v2.xml',
)

buttons_deps = [
    dependency('wayland-client'),
    dependency('cairo'),
    dependency('threads'),
    dependency('libevent'),
    m_dep,
]

executable('cowbuttons',
    ['src/cowbuttons.c'] + buttons_generated,
    '../../src/colour.c',
    '../../src/image.c',
    '../../src/memory.c',
    '../../src/module.c',
    '../../src/module_cursor.c',
    '../../src/module_config.c',
    '../../src/util.c',
    '../../src/vector.c',
    '../../vendor/cJSON.c',
    include_directories: [
        include_directories('../..'),
        include_directories('../../vendor'),
        include_directories('../../include'),
    ],
    dependencies: buttons_deps,
    install: true,
)
