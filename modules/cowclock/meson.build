# cowclock -- xarclock-inspired analogue clock for cow.

clock_generated = wl_mod.scan_xml(
    wl_proto_dir / 'stable/xdg-shell/xdg-shell.xml',
)

clock_deps = [
    dependency('wayland-client'),
    dependency('cairo'),
    dependency('pangocairo'),
    dependency('libevent'),
    m_dep,
]

executable('cowclock',
    ['src/cowclock.c'] + clock_generated,
    '../../src/colour.c',
    '../../src/memory.c',
    '../../src/module.c',
    '../../src/module_config.c',
    '../../src/module_draw.c',
    '../../src/module_shm.c',
    '../../src/module_theme.c',
    '../../src/shm_util.c',
    '../../src/text.c',
    '../../src/util.c',
    '../../vendor/cJSON.c',
    include_directories: [
        include_directories('../..'),
        include_directories('../../include'),
        include_directories('../../vendor'),
    ],
    dependencies: clock_deps,
    install: true,
)
