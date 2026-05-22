# cowpager -- FvwmPager-style pager for the cow window manager.

# cowpager uses xdg-shell for a regular toplevel window so cow applies SSDs.
pager_generated = wl_mod.scan_xml(
    wl_proto_dir / 'stable/xdg-shell/xdg-shell.xml',
)

pager_deps = [
    dependency('wayland-client'),
    dependency('cairo'),
    dependency('threads'),
]

executable('cowpager',
    ['src/cowpager.c'] + pager_generated,
    '../vendor/cJSON.c',
    include_directories: [
        include_directories('..'),
        include_directories('../vendor'),
        include_directories('../include'),
    ],
    dependencies: pager_deps,
    install: true,
)
