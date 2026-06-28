# cowbar -- status relay for the cow window manager.

cowbar_inc = [
    include_directories('.'),
    include_directories('../include'),
    include_directories('../vendor'),
]

executable('cowbar',
    'src/cowbar.c',
    '../src/module.c',
    '../vendor/cJSON.c',
    include_directories: cowbar_inc,
    install: true,
)
