# cowpager -- FvwmPager-style pager for the cow window manager.

# cowpager uses xdg-shell for a regular toplevel window so cow applies SSDs.
pager_generated = wl_mod.scan_xml(
    wl_proto_dir / 'stable/xdg-shell/xdg-shell.xml',
    wl_proto_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
    wl_proto_dir / 'unstable/tablet/tablet-unstable-v2.xml',
)

pager_deps = [
    dependency('wayland-client'),
    dependency('cairo'),
    dependency('libevent'),
    dependency('pangocairo'),
    m_dep,
]

pager_support_sources = files(
    '../../src/colour.c',
    '../../src/image.c',
    '../../src/image_cache.c',
    '../../src/xpm.c',
    '../../src/memory.c',
    '../../src/module.c',
    '../../src/module_status.c',
    '../../src/module_snapshot.c',
    '../../src/json_util.c',
    '../../src/output_selector.c',
    '../../src/module_shm.c',
    '../../src/shm_util.c',
    '../../src/module_cursor.c',
    '../../src/module_config.c',
    '../../src/module_draw.c',
    '../../src/module_theme.c',
    '../../src/text.c',
    '../../src/util.c',
    '../../vendor/cJSON.c',
)
pager_include_dirs = [
        include_directories('../..'),
        include_directories('../../vendor'),
        include_directories('../../include'),
]

cowpager = executable('cowpager',
    ['src/cowpager.c'] + pager_generated + pager_support_sources,
    include_directories: pager_include_dirs,
    dependencies: pager_deps,
    install: true,
)

pager_test = executable('test-cowpager',
    ['../../tests/cowpager.c'] + pager_generated + pager_support_sources,
    include_directories: pager_include_dirs,
    dependencies: pager_deps,
)
test('cowpager', pager_test)
