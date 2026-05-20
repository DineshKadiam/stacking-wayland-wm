# cowrearrange -- FvwmRearrange-style tile / cascade module.

executable('cowrearrange',
    ['src/cowrearrange.c'],
    '../vendor/cJSON.c',
    include_directories: [
        include_directories('..'),
        include_directories('../vendor'),
    ],
    dependencies: [
        cc.find_library('m'),
    ],
    install: true,
)
