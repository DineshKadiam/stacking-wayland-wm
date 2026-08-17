# cowident -- FvwmIdent-style window identifier for cow.
#
# Two phases: a fullscreen wlr-layer-shell overlay captures the next click,
# and an xdg-toplevel popup displays the resulting window's properties using
# cow's configured titlebar font.

ident_generated = wl_mod.scan_xml(
    wl_proto_dir / 'stable/xdg-shell/xdg-shell.xml',
    wl_proto_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
    wl_proto_dir / 'unstable/tablet/tablet-unstable-v2.xml',
    wl_proto_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml',
    '../../protocol/wlr-layer-shell-unstable-v1.xml',
)

ident_deps = [
    dependency('wayland-client'),
    dependency('wayland-cursor'),
    dependency('cairo'),
    dependency('pangocairo'),
    dependency('xkbcommon'),
]

executable('cowident',
    ['src/cowident.c'] + ident_generated,
    '../../src/memory.c',
    '../../src/json_util.c',
    '../../src/module.c',
    '../../src/module_shm.c',
    '../../src/shm_util.c',
    '../../src/text.c',
    '../../src/util.c',
    '../../src/module_cursor.c',
    '../../vendor/cJSON.c',
    include_directories: [
        include_directories('../..'),
        include_directories('../../include'),
        include_directories('../../vendor'),
    ],
    dependencies: ident_deps,
    install: true,
)
