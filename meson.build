project('velox', 'cpp',
  version: '1.1',
  default_options: ['cpp_std=c++11']
)

cpp = meson.get_compiler('cpp')
is_windows = host_machine.system() == 'windows'

inc = include_directories('.')

cli_sources = files('main.cpp')
gui_sources = files('VeloxPlayerGUI.cpp')

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
    install: true
  )
endif

if get_option('build_gui')
  if not is_windows
    error('velox_player is Windows-only. Use -Dbuild_gui=false on other platforms.')
  endif

  winlibs = [
    cpp.find_library('winmm', required: true),
    cpp.find_library('ole32', required: true),
    cpp.find_library('comdlg32', required: true),
    cpp.find_library('shell32', required: true),
    cpp.find_library('gdi32', required: true),
    cpp.find_library('comctl32', required: true),
    cpp.find_library('gdiplus', required: true),
    cpp.find_library('user32', required: true),
  ]

  executable('velox_player',
    gui_sources,
    include_directories: inc,
    dependencies: winlibs,
    link_args: gui_link_args,
    gui_app: true,
    install: true
  )
endif
