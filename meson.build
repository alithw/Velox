project('velox', 'cpp',
  version: '1.1',
  default_options: ['cpp_std=c++17']
)

cpp = meson.get_compiler('cpp')
qt6 = import('qt6')

inc = include_directories('.')

cli_sources = files('main.cpp')
gui_sources = files(
  'velox_player_main.cpp',
  'VeloxQtPlayerWindow.cpp',
  'VeloxQtPlayerEngine.cpp'
)

cli_link_args = []
gui_link_args = []

if get_option('static_mingw') and cpp.get_id() != 'msvc'
  cli_link_args += ['-static']
  gui_link_args += ['-static']
endif

if get_option('build_cli')
  executable('velox',
    cli_sources,
    include_directories: inc,
    link_args: cli_link_args,
    install_dir: meson.current_source_dir() + '/bin',
    install: true
  )
endif

if get_option('build_gui')
  qt_deps = dependency('qt6', modules: ['Widgets', 'Multimedia'])
  qt_sources = qt6.preprocess(
    moc_headers: [
      'VeloxQtPlayerWindow.h',
      'VeloxQtPlayerEngine.h'
    ]
  )

  executable('velox_player',
    gui_sources + qt_sources,
    include_directories: inc,
    dependencies: qt_deps,
    link_args: gui_link_args,
    win_subsystem: 'windows',
    install_dir: meson.current_source_dir() + '/bin',
    install: true
  )
endif
