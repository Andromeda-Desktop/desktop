project('andromeda-terminal', ['c', 'cpp'],
  version: '0.1.0',
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# Terminal (.agent/TASK-terminal.md): libghostty-vt core; the system package first, else the pinned build
# from tools/build-libghostty.sh, linked statically so the binary needs no private library path.
_warnings = ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow']

_ghostty = dependency('libghostty-vt', version: '>=1.0.1', required: false)
if not _ghostty.found()
  _prefix = meson.current_source_dir() / 'subprojects' / 'libghostty-vt' / 'prefix'
  _ghostty = declare_dependency(
    include_directories: include_directories('subprojects/libghostty-vt/prefix/include'),
    dependencies: meson.get_compiler('cpp').find_library('ghostty-vt', dirs: _prefix / 'lib', static: true),
    compile_args: '-DGHOSTTY_STATIC',
  )
endif

_core = static_library('terminal_core', 'src/pty_session.cpp',
  dependencies: [_ghostty, dependency('threads')], link_args: ['-lutil'], cpp_args: _warnings)
_core_dep = declare_dependency(link_with: _core, link_args: ['-lutil'],
  include_directories: include_directories('src'), dependencies: [_ghostty, dependency('threads')])

test('pty_session', executable('pty_session_test', 'tests/pty_session_test.cpp',
  dependencies: _core_dep, cpp_args: _warnings), timeout: 30)

cpp = meson.get_compiler('cpp')
if cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

_desk_app_dep = subproject('libdesk-app').get_variable('desk_app_dep')

executable('andromeda-terminal',
  'src/key_map.cpp',
  'src/main.cpp',
  'src/notifier.cpp',
  'src/terminal_app.cpp',
  'src/terminal_view.cpp',
  'src/terminal_window.cpp',
  dependencies: [_core_dep, _desk_app_dep],
  cpp_args: _warnings,
  install: true,
)

install_data('data/dev.andromeda.Terminal.desktop', install_dir: get_option('datadir') / 'applications')
install_data('data/icons/dev.andromeda.Terminal.svg', install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
