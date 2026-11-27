project('desk-app', ['c', 'cpp'],
  version: '0.1.0',
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# The app frame for first-party apps (docs/apps.md §2): run loop, window frame, commands, state, single instance.
# Same standard-library settings as libdesk-ui: they link one archive, so the flags must match.
cpp = meson.get_compiler('cpp')
is_libcxx = cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
if is_libcxx
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif
if get_option('buildtype') != 'release'
  if is_libcxx
    add_project_arguments('-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE', language: 'cpp')
  else
    add_project_arguments('-D_GLIBCXX_ASSERTIONS', language: 'cpp')
  endif
endif
if get_option('buildtype') == 'release'
  add_project_arguments('-ffunction-sections', '-fdata-sections', language: 'cpp')
endif

is_wayland = get_option('platform') == 'wayland'
_desk_ui_dep = subproject('libdesk-ui', default_options: ['platform=' + get_option('platform')]).get_variable('desk_ui_dep')

_deps = [
  _desk_ui_dep,
  dependency('nlohmann_json'),
  dependency('xkbcommon'),
  dependency('gio-2.0'),
  dependency('threads'),
]
# The Linux session services (D-Bus, portals, AT-SPI, zygote) and the accessors for apps that still call Wayland.
if is_wayland
  _deps += [
    subproject('libdesk-system').get_variable('desk_system_dep'),
    dependency('sdbus-c++'),
    dependency('wayland-client'),
  ]
  _platform_sources = files(
    'src/app/app_notifier_dbus.cpp',
    'src/app/app_services_linux.cpp',
    'src/app/appearance.cpp',
    'src/app/file_chooser.cpp',
    'src/app/single_instance.cpp',
    'src/app/wayland_accessors.cpp',
    'src/files/file_ops_client.cpp',
    'src/share/link_share.cpp',
    'src/share/share_menu.cpp',
  )
else
  add_languages('objcpp', native: false)
  _platform_sources = files('src/app/app_notifier_macos.mm', 'src/app/app_notifier_macos_app.cpp', 'src/app/app_services_none.cpp', 'src/app/file_chooser_macos.mm',
    'src/app/show_in_finder_macos.mm')
  _deps += dependency('appleframeworks', modules: ['AppKit', 'UserNotifications'])
endif

_inc = include_directories('src')
_warnings = ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow']

desk_app_lib = static_library('desk_app',
  sources: _platform_sources + files(
    'src/app/app_loop.cpp',
    'src/app/glib_source.cpp',
    'src/app/app_popover.cpp',
    'src/app/app_window.cpp',
    'src/app/split_view.cpp',
    'src/app/command_table.cpp',
    'src/app/app_clipboard.cpp',
    'src/app/desk_app.cpp',
    'src/app/state_store.cpp',
    'src/app/shown_timer.cpp',
  ),
  dependencies: _deps,
  include_directories: _inc,
  cpp_args: _warnings,
  objcpp_args: _warnings + ['-fobjc-arc'],
)

desk_app_dep = declare_dependency(
  link_with: desk_app_lib,
  dependencies: _deps,
  include_directories: _inc,
)

if get_option('devel') and not meson.is_subproject()
  # An installed, static libdesk for apps outside this tree (Link's window): pkg-config --static andromeda-desk.
  _ui_sp_dev = subproject('libdesk-ui')
  _dev_whole = [desk_app_lib, _ui_sp_dev.get_variable('desk_ui_lib')]
  _dev_src = [meson.current_source_dir() / 'src', meson.current_source_dir() / 'subprojects/libdesk-ui/src']
  _dev_protocols = [meson.current_source_dir() / 'subprojects/libdesk-ui/protocols']
  if is_wayland
    _dev_whole += subproject('libdesk-system').get_variable('desk_system_lib')
    _dev_src += meson.current_source_dir() / 'subprojects/libdesk-system/src'
    _dev_protocols += meson.current_source_dir() / 'subprojects/libdesk-system/protocols'
    _dev_private = ['-ldl', '-pthread']
  else
    _dev_private = [] # meson adds the frameworks of the dependencies
  endif
  _dev_lib = static_library('andromeda-desk',
    link_whole: _dev_whole,
    install: true,
    install_tag: 'devel',
  )
  _inc_dir = get_option('includedir') / 'andromeda-desk'
  _assets_dir = get_option('datadir') / 'andromeda-desk' / 'assets'
  import('pkgconfig').generate(
    name: 'andromeda-desk',
    description: 'Andromeda libdesk app frame, UI toolkit and session services (static)',
    version: meson.project_version(),
    libraries: _dev_lib,
    libraries_private: _dev_private,
    subdirs: 'andromeda-desk',
    extra_cflags: _ui_sp_dev.get_variable('_tokens_runtime'),
    variables: {'assetsdir': '${prefix}' / _assets_dir},
  )
  meson.add_install_script('tools/install-headers.sh', _inc_dir, _dev_src, '--protocols', _dev_protocols,
    '--generated', _ui_sp_dev.get_variable('_protocol_headers'),
    install_tag: 'devel',
  )
  install_subdir(meson.current_source_dir() / '../shell/assets', install_dir: _assets_dir, strip_directory: true,
    exclude_files: ['andromeda-wallpaper.png', 'noctalia-wallpaper.png'], install_tag: 'devel')
endif

if not is_wayland or (meson.is_subproject() and not get_option('shared_app_lib'))
  subdir_done()
endif

# Apps link libdesk as one shared library, so every app and andromeda-appd map the same code pages; shell, greeter,
# preview and portal keep the static libraries. Consumers get headers and external libraries, not the archives.
_ui_sp = subproject('libdesk-ui')
_system_sp = subproject('libdesk-system')
desk_shared_lib = shared_library('andromeda-desk',
  link_whole: [desk_app_lib, _system_sp.get_variable('desk_system_lib'), _ui_sp.get_variable('desk_ui_lib')],
  dependencies: _deps,
  link_args: '-Wl,-Bsymbolic-functions',
  install: true,
  install_dir: get_option('libdir') / 'andromeda',
  install_tag: 'libdesk',
)
desk_app_shared_dep = declare_dependency(
  link_with: desk_shared_lib,
  dependencies: [
    _desk_ui_dep.partial_dependency(compile_args: true, includes: true, sources: true),
    _system_sp.get_variable('desk_system_dep').partial_dependency(compile_args: true, includes: true),
    dependency('nlohmann_json'), dependency('xkbcommon'), dependency('gio-2.0'), dependency('threads'),
    dependency('sdbus-c++'), dependency('libsystemd'), dependency('wayland-client'),
  ] + _ui_sp.get_variable('_system_deps'),
  include_directories: _inc,
)

_apps_dir = get_option('prefix') / get_option('libdir') / 'andromeda'
executable('andromeda-appd', 'src/appd/appd_main.cpp',
  dependencies: desk_app_shared_dep,
  cpp_args: _warnings + ['-DANDROMEDA_APPS_DIR="' + _apps_dir + '"'],
  install: true,
  install_dir: get_option('libexecdir'),
  install_rpath: _apps_dir,
  install_tag: 'libdesk',
)
configure_file(
  input: 'data/andromeda-appd.service.in',
  output: 'andromeda-appd.service',
  configuration: {'appd': get_option('prefix') / get_option('libexecdir') / 'andromeda-appd'},
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user',
  install_tag: 'libdesk',
)

if meson.is_subproject()
  subdir_done()
endif

executable('desk-app-sample', 'examples/sample_app.cpp',
  dependencies: desk_app_dep,
  cpp_args: _warnings,
)

_dbus_run_session = find_program('dbus-run-session')
foreach _t : ['app_lifecycle', 'shown_timer', 'file_watcher', 'notice_keys', 'command_table', 'state_store', 'file_model', 'type_ahead', 'thumbnail_pipeline', 'batch_rename', 'file_model_bench', 'thumbnail_service', 'glib_source']
  test(_t, executable(_t + '_test', 'tests/' + _t + '_test.cpp', dependencies: desk_app_dep, cpp_args: _warnings))
endforeach
# The zygote's g_thread_new interposer restarts Pango's worker in each child; the executable must export it.
test('app_zygote', executable('app_zygote_test', 'tests/app_zygote_test.cpp', dependencies: desk_app_dep,
  cpp_args: _warnings, link_args: '-Wl,--export-dynamic-symbol=g_thread_new'), timeout: 30)
test('single_instance', _dbus_run_session,
  args: ['--', executable('single_instance_test', 'tests/single_instance_test.cpp', dependencies: desk_app_dep,
    cpp_args: _warnings)],
  timeout: 30,
)
test('i18n_domains', executable('i18n_domains_test', 'tests/i18n_domains_test.cpp', dependencies: desk_app_dep,
  cpp_args: _warnings))
test('appearance_portal', _dbus_run_session,
  args: ['--config-file=' + meson.current_source_dir() / 'tests/isolated-bus.conf', '--', executable('appearance_portal_test', 'tests/appearance_portal_test.cpp', dependencies: desk_app_dep,
    cpp_args: _warnings)],
  timeout: 30,
)
test('file_ops_client', _dbus_run_session,
  args: ['--', executable('file_ops_client_test', 'tests/file_ops_client_test.cpp', dependencies: desk_app_dep,
    cpp_args: _warnings)],
  timeout: 30,
)
