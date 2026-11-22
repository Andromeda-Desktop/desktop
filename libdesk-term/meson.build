project('desk-term', ['c', 'cpp'],
  version: '0.1.0',
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# The terminal core for first-party apps: a PTY session on libghostty-vt drawn as a libdesk-ui cell grid
# (TerminalView). Andromeda Terminal and the Files terminal pane both host it.
cpp = meson.get_compiler('cpp')
if cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif
_warnings = ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow']
is_macos = get_option('platform') == 'macos'

# libghostty-vt: the system package first, else the pinned build from tools/build-libghostty.sh, linked statically so
# a binary needs no private library path. macOS: the aarch64 build from `tools/build-libghostty.sh macos`.
_ghostty = dependency('libghostty-vt', version: '>=1.0.1', required: false)
if is_macos
  _prefix = meson.current_source_dir() / 'libghostty-vt' / 'prefix-macos'
  _ghostty = declare_dependency(
    include_directories: include_directories('libghostty-vt/prefix-macos/include'),
    # The dylib, not the archive: both libghostty-vt.a and libdesk-ui define wuffs, and ld64 keeps both copies.
    dependencies: cpp.find_library('ghostty-vt', dirs: _prefix / 'lib', static: false),
  )
elif not _ghostty.found()
  _prefix = meson.current_source_dir() / 'libghostty-vt' / 'prefix'
  _ghostty = declare_dependency(
    include_directories: include_directories('libghostty-vt/prefix/include'),
    dependencies: cpp.find_library('ghostty-vt', dirs: _prefix / 'lib', static: true),
    compile_args: '-DGHOSTTY_STATIC',
  )
endif

_desk_ui_dep = subproject('libdesk-ui', default_options: ['platform=' + get_option('platform')]).get_variable('desk_ui_dep')
# The release version comes from the terminal unit's tags (XTVERSION answers with it).
_version = run_command(meson.current_source_dir() / '..' / 'tools' / 'unit-version.sh', 'terminal', check: true).stdout().strip()
_args = _warnings + [
  '-DANDROMEDA_TERMINAL_VERSION="' + _version + '"',
  '-DANDROMEDA_TERMINFO_DIR="' + get_option('prefix') / get_option('datadir') / 'terminfo' + '"',
]
# The .app bundle has no socket activation or libexec: ptyd sits beside the executable.
if is_macos
  _args += '-DANDROMEDA_HELPERS_BESIDE_EXE'
endif

_deps = [
  _ghostty,
  dependency('threads'),
  dependency('nlohmann_json'),
  dependency('tomlplusplus').partial_dependency(includes: true), # header-only: no library to link
  dependency('xkbcommon'),
  dependency('gio-2.0'),
]
# Compiles against libdesk-ui but does not link it: an app links libdesk-ui once, static or through libandromeda-desk.
desk_term_lib = static_library('desk_term',
  'src/command_index.cpp', 'src/context_row.cpp', 'src/holder_client.cpp', 'src/key_map.cpp', 'src/keybindings.cpp',
  'src/kitty_images.cpp', 'src/link_finder.cpp', 'src/osc1337.cpp', 'src/osc66.cpp', 'src/osc99.cpp',
  'src/osc_frame.cpp', 'src/pty_session.cpp', 'src/session_list.cpp', 'src/settings.cpp', 'src/sixel.cpp',
  'src/terminal_find.cpp', 'src/terminal_view.cpp', 'src/theme_store.cpp',
  dependencies: _deps + [_desk_ui_dep.partial_dependency(compile_args: true, includes: true, sources: true)],
  cpp_args: _args,
)
desk_term_dep = declare_dependency(
  link_with: desk_term_lib,
  include_directories: include_directories('src'),
  dependencies: _deps,
)
