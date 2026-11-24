project('andromeda-terminal', ['c', 'cpp'],
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# Terminal (.agent/TASK-terminal.md): the app around libdesk-term (PTY, libghostty-vt, TerminalView).
_warnings = ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow']

cpp = meson.get_compiler('cpp')
if cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

is_macos = get_option('platform') == 'macos'
_platform_opt = ['platform=' + get_option('platform')]
# libdesk-ui comes only through libdesk-app (apps/AGENTS.md).
_desk_app_dep = subproject('libdesk-app', default_options: _platform_opt + ['shared_app_lib=true']).get_variable('desk_app_dep')
# PTY session, VT engine and the TerminalView grid; the Files pane hosts the same view.
_desk_term_sp = subproject('libdesk-term', default_options: _platform_opt)
_desk_term_dep = _desk_term_sp.get_variable('desk_term_dep')
# The .app bundle has no socket activation or libexec: ptyd and agentd sit beside the executable.
_helpers_arg = is_macos ? ['-DANDROMEDA_HELPERS_BESIDE_EXE'] : []
_core = static_library('terminal_core', 'src/workspace_watcher.cpp', 'src/workspace_bar.cpp', 'src/window_workspaces.cpp', 'src/session_layout.cpp',
  'src/palette_model.cpp', 'src/browse_model.cpp',
  dependencies: [_desk_term_dep, dependency('threads'), _desk_app_dep.partial_dependency(compile_args: true, includes: true, sources: true), dependency('nlohmann_json')],
  cpp_args: _warnings)
_core_dep = declare_dependency(link_with: _core,
  include_directories: include_directories('src'), dependencies: [_desk_term_dep, dependency('threads'), _desk_app_dep, dependency('nlohmann_json')])

# ptyd and agentd cross-build for macOS in step 8.
if not is_macos
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
endif

# Agent sessions (step 9): the ACP session model; no IO, so the model test drives it directly.
_agent = static_library('terminal_agent', 'src/agent_session.cpp', 'src/agent_markup.cpp',
  dependencies: dependency('nlohmann_json'), cpp_args: _warnings)
_agent_dep = declare_dependency(link_with: _agent, include_directories: include_directories('src'),
  dependencies: dependency('nlohmann_json'))
test('agent_session', executable('agent_session_test', 'tests/agent_session_test.cpp',
  dependencies: _agent_dep, cpp_args: _warnings))

# Preferences file model (libdesk-term): TOML parse and write, no IO.
test('settings', executable('settings_test', 'tests/settings_test.cpp',
  dependencies: _desk_term_dep, cpp_args: _warnings))

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

test('workspace_model', executable('workspace_model_test', 'tests/workspace_model_test.cpp',
  dependencies: _core_dep, cpp_args: _warnings))

test('terminal_find', executable('terminal_find_test', 'tests/terminal_find_test.cpp',
  dependencies: _core_dep, cpp_args: _warnings))
test('palette_model', executable('palette_model_test', 'tests/palette_model_test.cpp',
  dependencies: _core_dep, cpp_args: _warnings))
test('browse_model', executable('browse_model_test', 'tests/browse_model_test.cpp',
  dependencies: _core_dep, cpp_args: _warnings))
test('terminal_bidi', executable('terminal_bidi_test', 'tests/terminal_bidi_test.cpp',
  dependencies: _desk_app_dep, cpp_args: _warnings))

_agentd_path = ['-DANDROMEDA_AGENTD_PATH="' + get_option('prefix') / get_option('libexecdir') / 'andromeda' / 'andromeda-agentd' + '"']
if not is_macos
test('agent_host', executable('agent_host_test', 'tests/agent_host_test.cpp', 'src/agent_host.cpp', 'src/agent_kind.cpp',
  dependencies: [_agent_dep, _desk_app_dep], cpp_args: _warnings + _agentd_path),
  args: [_agentd.full_path()], depends: _agentd, timeout: 60)
endif
test('split_trees', executable('split_trees_test', 'tests/split_trees_test.cpp',
  dependencies: [_core_dep, _desk_app_dep], cpp_args: _warnings))
test('theme_palette', executable('theme_palette_test', 'tests/theme_palette_test.cpp',
  dependencies: _desk_app_dep, cpp_args: _warnings))
test('theme_store', executable('theme_store_test', 'tests/theme_store_test.cpp',
  dependencies: [_desk_term_dep, _desk_app_dep], cpp_args: _warnings))

# Session services per platform: D-Bus on Linux; Cocoa frameworks on macOS (.agent/TASK-terminal-macos.md step 7).
if is_macos
  add_languages('objcpp', native: false)
  _platform_src = files('src/control_socket.cpp', 'src/file_actions_macos.mm')
  _platform_deps = [dependency('appleframeworks', modules: ['Foundation', 'AppKit'])]
else
  _platform_src = files('src/control_dbus.cpp', 'src/file_actions_dbus.cpp')
  _platform_deps = []
endif

_terminfo_dir = get_option('prefix') / get_option('datadir') / 'terminfo'
_app_src = files('src/app_main.cpp',
  'src/terminal_app.cpp',
  'src/terminal_window.cpp', 'src/command_palette.cpp', 'src/browse_view.cpp',
  'src/settings_store.cpp',
  'src/theme_cli.cpp', 'src/terminal_control.cpp', 'src/control_cli.cpp',
  'src/prefs_window.cpp',
  'src/agent_host.cpp', 'src/agent_kind.cpp',
  'src/agent_view.cpp',
) + _platform_src
_app_args = _warnings + _agentd_path + _helpers_arg
if is_macos
executable('andromeda-terminal', _app_src, 'src/main.cpp',
  dependencies: [_core_dep, _agent_dep, _desk_app_dep, dependency('fontconfig')] + _platform_deps,
  cpp_args: _app_args,
  objcpp_args: ['-fobjc-arc'],
  link_args: ['-Wl,-rpath,@executable_path/../Frameworks'],
  install: true,
)
else
# The app is a library (desk_app_main) so andromeda-appd can load it in a forked child; the executable only calls it.
_desk_app_sp = subproject('libdesk-app')
_desk_shared_dep = _desk_app_sp.get_variable('desk_app_shared_dep')
_app_dir = get_option('prefix') / get_option('libdir') / 'andromeda'
_terminal_app = shared_library('andromeda-terminal', _app_src,
  link_with: _core,
  dependencies: [_desk_term_dep, _agent_dep, _desk_shared_dep, dependency('fontconfig')],
  include_directories: include_directories('src'),
  cpp_args: _app_args,
  install: true,
  install_dir: _app_dir,
  install_rpath: _app_dir,
)
executable('andromeda-terminal', 'src/main.cpp',
  link_with: _terminal_app,
  dependencies: _desk_shared_dep.partial_dependency(includes: true),
  install: true,
  install_rpath: _app_dir,
)
endif
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

install_subdir(_desk_term_sp.get_variable('shell_integration_dir'), install_dir: get_option('datadir') / 'andromeda' / 'terminal')
install_subdir('themes', install_dir: get_option('datadir') / 'andromeda' / 'terminal')
install_data('data/dev.andromeda.Terminal.desktop', install_dir: get_option('datadir') / 'applications')
# DBusActivatable=true in the .desktop file: the bus starts the app by its id, then calls Activate or Open.
configure_file(
  input: 'data/dev.andromeda.Terminal.service.in',
  output: 'dev.andromeda.Terminal.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)
install_data('data/icons/dev.andromeda.Terminal.svg', install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
