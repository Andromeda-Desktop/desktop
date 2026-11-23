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

_model = static_library('link_model', 'src/link_model.cpp', dependencies: dependency('nlohmann_json'), cpp_args: _warnings)

test('link_model', executable('link_model_test', 'tests/link_model_test.cpp', link_with: _model, dependencies: dependency('nlohmann_json'),
  include_directories: include_directories('src'), cpp_args: _warnings))

_desk_app_dep = subproject('libdesk-app').get_variable('desk_app_dep')

executable('andromeda-link-app',
  'src/link_app.cpp',
  'src/link_client.cpp',
  'src/main.cpp',
  'src/mirror_decoder.cpp',
  link_with: _model,
  dependencies: [_desk_app_dep, dependency('libqrencode'), dependency('libavcodec'), dependency('libavutil'), dependency('libavfilter'),
                 dependency('libswscale'), dependency('tomlplusplus').partial_dependency(includes: true)],
  cpp_args: _warnings,
  install: true,
)

_av_deps = [dependency('libavcodec'), dependency('libavutil'), dependency('libavfilter'), dependency('libswscale')]
test('mirror_decoder', executable('mirror_decoder_test', 'tests/mirror_decoder_test.cpp', 'src/mirror_decoder.cpp',
  dependencies: [_desk_app_dep] + _av_deps, include_directories: include_directories('src'),
  cpp_args: _warnings + ['-fsanitize=address', '-fno-omit-frame-pointer'], link_args: ['-fsanitize=address']),
  env: ['ASAN_OPTIONS=detect_leaks=0'])

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
