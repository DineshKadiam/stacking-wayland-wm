cowdiag_ncurses = dependency('ncursesw', required: get_option('cowdiag'))

if cowdiag_ncurses.found()
    executable('cowdiag',
        'src/cowdiag.c',
        'src/proc.c',
        '../../src/module.c',
        '../../src/module_protocol.c',
        '../../src/util.c',
        '../../vendor/cJSON.c',
        include_directories: [
            include_directories('../..'),
            include_directories('.'),
            include_directories('../../include'),
            include_directories('../../vendor'),
        ],
        dependencies: [cowdiag_ncurses, dependency('wayland-client')],
        install: true,
    )
else
    message('ncursesw not found: cowdiag will not be built')
endif
