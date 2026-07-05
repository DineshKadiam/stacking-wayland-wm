# cowiconman -- FvwmIconMan-style window list for cow.

iconman_generated = wl_mod.scan_xml(
    wl_proto_dir / 'stable/xdg-shell/xdg-shell.xml',
)

iconman_deps = [
    dependency('wayland-client'),
    dependency('cairo'),
    dependency('threads'),
    dependency('libevent'),
]

executable('cowiconman',
    ['src/cowiconman.c'] + iconman_generated,
    '../src/module.c',
    '../vendor/cJSON.c',
    include_directories: [
        include_directories('..'),
        include_directories('../vendor'),
        include_directories('../include'),
    ],
    dependencies: iconman_deps,
    install: true,
)
