# cowrearrange -- FvwmRearrange-style tile / cascade module.

executable('cowrearrange',
    ['src/cowrearrange.c'],
    '../../src/module.c',
    '../../src/util.c',
    '../../vendor/cJSON.c',
    include_directories: [
        include_directories('../..'),
        include_directories('../../include'),
        include_directories('../../vendor'),
    ],
    dependencies: [
        cc.find_library('m'),
    ],
    install: true,
)
