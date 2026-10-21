# cow-appmenu -- standalone XDG application discovery and menu generation.

appmenu_xml = dependency('libxml-2.0', required: get_option('cow_appmenu'))
if appmenu_xml.found()
    cow_appmenu = executable('cow-appmenu',
        'src/cow-appmenu.c',
        'src/appmenu-entry.c',
        'src/appmenu-menu.c',
        'src/appmenu-layout.c',
        'src/appmenu-launch.c',
        'src/appmenu-output.c',
        'src/appmenu-xml.c',
        '../src/icon_lookup.c',
        '../src/memory.c',
        '../src/util.c',
        '../vendor/cJSON.c',
        include_directories: include_dirs,
        dependencies: [libbsd, appmenu_xml],
        install: true,
    )
    test('application-catalogue', find_program('../tests/application-catalogue.sh'),
        args: [cow_appmenu])
    test('application-launch', find_program('../tests/application-launch.sh'),
        args: [cow_appmenu])
    test('application-menu-output', find_program('../tests/application-menu-output.sh'),
        args: [cow_appmenu, cow])
    install_data('../examples/applications.menu', install_dir: datadir / 'cow')
endif
