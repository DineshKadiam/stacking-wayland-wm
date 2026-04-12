# cowpager -- FvwmPager-style pager for the cow window manager.

if meson.version().version_compare('>= 1.8.0')
    wl_mod_p = import('wayland')
else
    wl_mod_p = import('unstable-wayland')
endif

# cowpager uses xdg-shell for a regular toplevel window so cow applies SSDs.
pager_generated = wl_mod_p.scan_xml(
    '/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml',
)

pager_deps = [
    dependency('wayland-client'),
    dependency('cairo'),
    dependency('libbsd'),
    dependency('threads'),
]

executable('cowpager',
    ['src/cowpager.c'] + pager_generated,
    '../vendor/cJSON.c',
    include_directories: [
        include_directories('..'),
        include_directories('../vendor'),
    ],
    dependencies: pager_deps,
    install: true,
)
