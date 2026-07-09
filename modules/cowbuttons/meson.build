# cowbuttons -- FvwmButtons-style button panel for cow.

buttons_generated = wl_mod.scan_xml(
    wl_proto_dir / 'stable/xdg-shell/xdg-shell.xml',
)

buttons_deps = [
    dependency('wayland-client'),
    dependency('cairo'),
    dependency('threads'),
    dependency('libevent'),
]

executable('cowbuttons',
    ['src/cowbuttons.c'] + buttons_generated,
    '../../src/module.c',
    '../../vendor/cJSON.c',
    include_directories: [
        include_directories('../..'),
        include_directories('../../vendor'),
        include_directories('../../include'),
    ],
    dependencies: buttons_deps,
    install: true,
)
