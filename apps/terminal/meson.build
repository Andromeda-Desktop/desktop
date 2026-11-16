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

_desk_ui_dep = subproject('libdesk-ui').get_variable('desk_ui_dep')
_core = static_library('terminal_core', 'src/pty_session.cpp', 'src/osc66.cpp', 'src/osc1337.cpp', 'src/sixel.cpp', 'src/osc99.cpp', 'src/osc_frame.cpp', 'src/holder_client.cpp', 'src/link_finder.cpp', 'src/session_list.cpp', 'src/session_layout.cpp',
  'src/shell_integration.cpp', 'src/command_index.cpp', 'src/terminal_find.cpp', 'src/palette_model.cpp',
  dependencies: [_ghostty, dependency('threads'), _desk_ui_dep, dependency('nlohmann_json')], link_args: ['-lutil'],
  cpp_args: _warnings + ['-DANDROMEDA_TERMINAL_VERSION="' + meson.project_version() + '"', '-DANDROMEDA_TERMINAL_SOURCE_SHELL_DIR="' + meson.current_source_dir() / 'data' / 'shell-integration' + '"'])
_core_dep = declare_dependency(link_with: _core, link_args: ['-lutil'],
  include_directories: include_directories('src'), dependencies: [_ghostty, dependency('threads'), _desk_ui_dep, dependency('nlohmann_json')])

# The pty holder (Rust) builds with cargo into this build dir, as andromeda-preview's helper.
cargo = find_program('cargo')
_cargo_profile = get_option('buildtype') == 'debug' ? 'debug' : 'release'
_ptyd = custom_target('andromeda-ptyd',
  output: 'andromeda-ptyd',
  build_always_stale: true,
  console: true,
  command: [
    'sh', '-c', '"$1" build --quiet --locked $2 --manifest-path "$3" --target-dir "$4" && cp "$4/$5/andromeda-ptyd" "$6"',
    '_', cargo, _cargo_profile == 'release' ? '--release' : '--profile=dev',
    meson.current_source_dir() / 'ptyd' / 'Cargo.toml', meson.current_build_dir() / 'cargo-target', _cargo_profile,
    '@OUTPUT@',
  ],
  install: true,
  install_dir: get_option('libexecdir') / 'andromeda',
)
# Agent sessions daemon (Rust, ACP over stdio; fork of kkestell/claude-code-acp, MIT): the app starts one per instance.
_agentd = custom_target('andromeda-agentd',
  output: 'andromeda-agentd',
  build_always_stale: true,
  console: true,
  command: [
    'sh', '-c', '"$1" build --quiet --locked $2 --manifest-path "$3" --target-dir "$4" && cp "$4/$5/andromeda-agentd" "$6"',
    '_', cargo, _cargo_profile == 'release' ? '--release' : '--profile=dev',
    meson.current_source_dir() / 'agentd' / 'Cargo.toml', meson.current_build_dir() / 'cargo-target', _cargo_profile,
    '@OUTPUT@',
  ],
  install: true,
  install_dir: get_option('libexecdir') / 'andromeda',
)
test('pty_session', executable('pty_session_test', 'tests/pty_session_test.cpp',
  dependencies: _core_dep, cpp_args: _warnings), args: [_ptyd.full_path()], depends: _ptyd, timeout: 30)

# Agent sessions (step 9): the ACP session model; no IO, so the model test drives it directly.
_agent = static_library('terminal_agent', 'src/agent_session.cpp', 'src/agent_markup.cpp',
  dependencies: dependency('nlohmann_json'), cpp_args: _warnings)
_agent_dep = declare_dependency(link_with: _agent, include_directories: include_directories('src'),
  dependencies: dependency('nlohmann_json'))
test('agent_session', executable('agent_session_test', 'tests/agent_session_test.cpp',
  dependencies: _agent_dep, cpp_args: _warnings))

# Preferences file model: TOML parse and write, no IO.
_settings = static_library('terminal_settings', 'src/settings.cpp', 'src/keybindings.cpp',
  dependencies: [dependency('tomlplusplus').partial_dependency(includes: true), dependency('xkbcommon')], cpp_args: _warnings)
_settings_dep = declare_dependency(link_with: _settings, include_directories: include_directories('src'),
  dependencies: dependency('xkbcommon'))
test('settings', executable('settings_test', 'tests/settings_test.cpp',
  dependencies: _settings_dep, cpp_args: _warnings))

test('osc66', executable('osc66_test', 'tests/osc66_test.cpp',
  dependencies: _core_dep, cpp_args: _warnings))

test('sixel', executable('sixel_test', 'tests/sixel_test.cpp',
  dependencies: _core_dep, cpp_args: _warnings))

test('osc1337', executable('osc1337_test', 'tests/osc1337_test.cpp',
  dependencies: _core_dep, cpp_args: _warnings))

test('link_finder', executable('link_finder_test', 'tests/link_finder_test.cpp',
  dependencies: _core_dep, cpp_args: _warnings))

test('command_index', executable('command_index_test', 'tests/command_index_test.cpp',
  dependencies: _core_dep, cpp_args: _warnings))

test('session_list', executable('session_list_test', 'tests/session_list_test.cpp',
  dependencies: _core_dep, cpp_args: _warnings))

test('terminal_find', executable('terminal_find_test', 'tests/terminal_find_test.cpp',
  dependencies: _core_dep, cpp_args: _warnings))
test('palette_model', executable('palette_model_test', 'tests/palette_model_test.cpp',
  dependencies: _core_dep, cpp_args: _warnings))
test('terminal_bidi', executable('terminal_bidi_test', 'tests/terminal_bidi_test.cpp',
  dependencies: _desk_ui_dep, cpp_args: _warnings))

cpp = meson.get_compiler('cpp')
if cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

_desk_app_dep = subproject('libdesk-app').get_variable('desk_app_dep')
_agentd_path = ['-DANDROMEDA_AGENTD_PATH="' + get_option('prefix') / get_option('libexecdir') / 'andromeda' / 'andromeda-agentd' + '"']
test('agent_host', executable('agent_host_test', 'tests/agent_host_test.cpp', 'src/agent_host.cpp', 'src/agent_kind.cpp',
  dependencies: [_agent_dep, _desk_app_dep], cpp_args: _warnings + _agentd_path),
  args: [_agentd.full_path()], depends: _agentd, timeout: 60)
test('split_trees', executable('split_trees_test', 'tests/split_trees_test.cpp',
  dependencies: [_core_dep, _desk_app_dep], cpp_args: _warnings))
test('theme_palette', executable('theme_palette_test', 'tests/theme_palette_test.cpp',
  dependencies: _desk_app_dep, cpp_args: _warnings))
test('theme_store', executable('theme_store_test', 'tests/theme_store_test.cpp', 'src/theme_store.cpp',
  dependencies: [_settings_dep, _desk_app_dep], cpp_args: _warnings))

_terminfo_dir = get_option('prefix') / get_option('datadir') / 'terminfo'
executable('andromeda-terminal',
  'src/key_map.cpp',
  'src/kitty_images.cpp',
  'src/context_row.cpp',
  'src/main.cpp',
  'src/notifier.cpp',
  'src/terminal_app.cpp',
  'src/terminal_view.cpp',
  'src/terminal_window.cpp', 'src/command_palette.cpp',
  'src/settings_store.cpp',
  'src/theme_store.cpp',
  'src/theme_cli.cpp', 'src/terminal_control.cpp', 'src/control_cli.cpp',
  'src/prefs_window.cpp',
  'src/agent_host.cpp', 'src/agent_kind.cpp',
  'src/agent_view.cpp',
  dependencies: [_core_dep, _agent_dep, _settings_dep, _desk_app_dep, dependency('fontconfig')],
  cpp_args: _warnings + _agentd_path + ['-DANDROMEDA_TERMINFO_DIR="' + _terminfo_dir + '"'],
  install: true,
)
find_program('tic')
meson.add_install_script('sh', '-c', 'mkdir -p "$DESTDIR$1" && tic -x -o "$DESTDIR$1" "$2"', 'tic',
  _terminfo_dir, meson.current_source_dir() / 'data' / 'andromeda.terminfo')

_user_unit_dir = get_option('prefix') / 'lib' / 'systemd' / 'user'
configure_file(input: 'data/andromeda-ptyd.service.in', output: 'andromeda-ptyd.service',
  configuration: {
    'libexecdir': get_option('prefix') / get_option('libexecdir'),
    'bindir': get_option('prefix') / get_option('bindir'),
  },
  install_dir: _user_unit_dir)
install_data('data/andromeda-ptyd.socket', install_dir: _user_unit_dir)
install_symlink('andromeda-ptyd.socket', pointing_to: '../andromeda-ptyd.socket',
  install_dir: _user_unit_dir / 'sockets.target.wants')

install_subdir('data/shell-integration', install_dir: get_option('datadir') / 'andromeda' / 'terminal')
install_data('data/dev.andromeda.Terminal.desktop', install_dir: get_option('datadir') / 'applications')
install_data('data/icons/dev.andromeda.Terminal.svg', install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
