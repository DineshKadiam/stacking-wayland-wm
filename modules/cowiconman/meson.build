# cowiconman -- FvwmIconMan-style window list for cow.

iconman_generated = wl_mod.scan_xml(
    wl_proto_dir / 'stable/xdg-shell/xdg-shell.xml',
    wl_proto_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
    wl_proto_dir / 'unstable/tablet/tablet-unstable-v2.xml',
)

iconman_deps = [
    dependency('wayland-client'),
    dependency('cairo'),
    dependency('pangocairo'),
    dependency('libevent'),
    m_dep,
]

cowiconman = executable('cowiconman',
    ['src/cowiconman.c'] + iconman_generated,
    '../../src/image.c',
    '../../src/image_cache.c',
    '../../src/xpm.c',
    '../../src/colour.c',
    '../../src/memory.c',
    '../../src/json_util.c',
    '../../src/module.c',
    '../../src/module_status.c',
    '../../src/module_snapshot.c',
    '../../src/output_selector.c',
    '../../src/module_config.c',
    '../../src/module_shm.c',
    '../../src/shm_util.c',
    '../../src/text.c',
    '../../src/util.c',
    '../../src/module_cursor.c',
    '../../src/module_draw.c',
    '../../src/module_theme.c',
    '../../vendor/cJSON.c',
    include_directories: [
        include_directories('../..'),
        include_directories('../../vendor'),
        include_directories('../../include'),
    ],
    dependencies: iconman_deps,
    install: true,
)
