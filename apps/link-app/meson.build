project('andromeda-link', ['c', 'cpp'],
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# Link desktop app (.agent/TASK-link-design.md step 5): a libdesk-app client of org.andromeda.Link1.
cpp = meson.get_compiler('cpp')
if cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

_warnings = ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow']

_desk_app_sp = subproject('libdesk-app', default_options: ['shared_app_lib=true'])
_desk_app_dep = _desk_app_sp.get_variable('desk_app_dep')
_translations = '-DANDROMEDA_TRANSLATIONS_DIR="' + meson.current_source_dir() / 'translations' + '"'

_model = static_library('link_model', 'src/link_model.cpp', dependencies: [dependency('nlohmann_json'), _desk_app_dep], cpp_args: _warnings)

# Thousands of random steps per model: at -O0 the checks alone neared meson's 30 s timeout on a loaded machine.
test('link_model', executable('link_model_test', 'tests/link_model_test.cpp', link_with: _model, dependencies: [dependency('nlohmann_json'), _desk_app_dep],
  include_directories: include_directories('src'), cpp_args: _warnings + [_translations], override_options: ['optimization=2']))

_app_dir = get_option('prefix') / get_option('libdir') / 'andromeda'
# The app is a library (desk_app_main) so andromeda-appd can load it in a forked child; the executable only calls it.
_link_app = shared_library('andromeda-link-app',
  'src/app_main.cpp',
  'src/link_app.cpp',
  'src/link_client.cpp',
  'src/mirror_decoder.cpp',
  link_with: _model,
  dependencies: [_desk_app_sp.get_variable('desk_app_shared_dep'), dependency('libqrencode'), dependency('libavcodec'), dependency('libavutil'), dependency('libavfilter'),
                 dependency('libswscale'), dependency('libva'), dependency('tomlplusplus').partial_dependency(includes: true)],
  cpp_args: _warnings + [_translations],
  install: true,
  install_dir: _app_dir,
  install_rpath: _app_dir,
)
executable('andromeda-link-app', 'src/main.cpp',
  link_with: _link_app,
  dependencies: _desk_app_sp.get_variable('desk_app_shared_dep').partial_dependency(includes: true),
  install: true,
  install_rpath: _app_dir,
)

_av_deps = [dependency('libavcodec'), dependency('libavutil'), dependency('libavfilter'), dependency('libswscale'), dependency('libva')]
test('mirror_decoder', executable('mirror_decoder_test', 'tests/mirror_decoder_test.cpp', 'src/mirror_decoder.cpp',
  dependencies: [_desk_app_dep] + _av_deps, include_directories: include_directories('src'),
  cpp_args: _warnings + ['-fsanitize=address', '-fno-omit-frame-pointer'], link_args: ['-fsanitize=address']),
  env: ['ASAN_OPTIONS=detect_leaks=0'])

install_data('translations/en.json', install_dir: get_option('datadir') / 'andromeda' / 'translations' / 'link')
install_data('data/dev.andromeda.Link.desktop', install_dir: get_option('datadir') / 'applications')
foreach size : ['16', '24', '32', '48', '64', '128', '256', '512']
  install_data('data/icons/hicolor' / size + 'x' + size / 'apps/dev.andromeda.Link.png',
               install_dir: get_option('datadir') / 'icons/hicolor' / size + 'x' + size / 'apps')
endforeach
# DBusActivatable=true in the .desktop file: the bus starts the app by its id, then calls Activate or Open.
configure_file(
  input: 'data/dev.andromeda.Link.service.in',
  output: 'dev.andromeda.Link.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)
