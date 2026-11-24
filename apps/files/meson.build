project('andromeda-files', ['cpp'],
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# Files (docs/apps.md §3, .agent/DESIGN.md "Files app P2"): a libdesk-app client.
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

_desk_app_sp = subproject('libdesk-app', default_options: ['shared_app_lib=true'])
# The terminal pane hosts the same TerminalView as Andromeda Terminal.
_desk_term_dep = subproject('libdesk-term').get_variable('desk_term_dep')
_desk_app_dep = _desk_app_sp.get_variable('desk_app_dep')
# Gallery video in place: the preview's subsurface player and sandboxed media helper (audio loads lazily).
_preview_media_dep = subproject('andromeda-preview').get_variable('preview_media_dep')
_gio_unix_dep = dependency('gio-unix-2.0') # GUnixMountMonitor, GDesktopAppInfo
add_project_arguments('-DANDROMEDA_TRANSLATIONS_DIR="' + meson.current_source_dir() / 'translations' + '"', language: 'cpp')
_warnings = ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow']

_files_lib = static_library('files_core',
  sources: files('src/trash.cpp', 'src/archive_listing.cpp', 'src/permission_access.cpp', 'src/server_url.cpp',
    'src/tags.cpp', 'src/exif_date.cpp', 'src/state_file.cpp', 'src/admin_session.cpp'),
  dependencies: _desk_app_dep.partial_dependency(compile_args: true, includes: true, sources: true),
  cpp_args: _warnings,
)

_app_dir = get_option('prefix') / get_option('libdir') / 'andromeda'
# The app is a library (desk_app_main) so andromeda-appd can load it in a forked child; the executable only calls it.
_files_app = shared_library('andromeda-files',
  sources: files(
    'src/app_main.cpp',
    'src/files_app.cpp', 'src/offline_map.cpp', 'src/gallery_video.cpp',
    'src/batch_rename_sheet.cpp',
    'src/tag_manager_sheet.cpp',
    'src/terminal_pane.cpp',
    'src/select_pattern_sheet.cpp',
    'src/folder_compare.cpp',
    'src/smart_folders.cpp',
    'src/smart_folder_sheet.cpp',
    'src/compress_sheet.cpp',
    'src/extract_sheet.cpp',
    'src/get_info_sheet.cpp',
    'src/admin_process.cpp',
    'src/admin_view.cpp',
    'src/starred.cpp',
    'src/terminal.cpp',
    'src/index_search.cpp',
    'src/file_manager1.cpp',
    'src/phone_page.cpp',
    'src/storage_page.cpp',
    'src/vaults.cpp',
    'src/vault_sheet.cpp',
    'src/drives.cpp',
    'src/network_page.cpp',
    'src/conflict_sheet.cpp',
    'src/connect_sheet.cpp',
    'src/details_pane.cpp',
    'src/job_pill.cpp',
    'src/files_window.cpp',
    'src/view_store.cpp',
    'src/service_menus.cpp',
    'src/files_window_picker.cpp',
    'src/app_chooser_portal.cpp',
    'src/email_portal.cpp',
    'src/print_jobs.cpp',
    'src/print_portal.cpp',
    'src/print_sheet.cpp',
    'src/app_chooser_sheet.cpp',
    'src/picker_portal.cpp',
    'src/portal_sheets.cpp',
    'src/system_portals.cpp',
  ),
  link_with: _files_lib,
  dependencies: [_desk_app_sp.get_variable('desk_app_shared_dep'), _gio_unix_dep, _desk_term_dep,
    _preview_media_dep],
  cpp_args: _warnings,
  install: true,
  install_dir: _app_dir,
  install_rpath: _app_dir,
)
executable('andromeda-files', 'src/main.cpp',
  link_with: _files_app,
  dependencies: _desk_app_sp.get_variable('desk_app_shared_dep').partial_dependency(includes: true),
  install: true,
  install_rpath: _app_dir,
)

test('permission_access', executable('permission_access_test', 'tests/permission_access_test.cpp',
  link_with: _files_lib, dependencies: _desk_app_dep, include_directories: include_directories('src'), cpp_args: _warnings))
test('archive_listing', executable('archive_listing_test', 'tests/archive_listing_test.cpp', link_with: _files_lib,
  dependencies: _desk_app_dep, include_directories: include_directories('src'), cpp_args: _warnings))
test('server_url', executable('server_url_test', 'tests/server_url_test.cpp', link_with: _files_lib,
  dependencies: _desk_app_dep, include_directories: include_directories('src'), cpp_args: _warnings))
test('exif_date', executable('exif_date_test', 'tests/exif_date_test.cpp', link_with: _files_lib,
  dependencies: _desk_app_dep, include_directories: include_directories('src'), cpp_args: _warnings))
test('print_settings', executable('print_settings_test', 'tests/print_settings_test.cpp', link_with: _files_lib,
  dependencies: _desk_app_dep, include_directories: include_directories('src'), cpp_args: _warnings))
test('safe_regex', executable('safe_regex_test', 'tests/safe_regex_test.cpp', dependencies: _desk_app_dep,
  cpp_args: _warnings))
test('picker_match', executable('picker_match_test', 'tests/picker_match_test.cpp', 'src/picker_portal.cpp',
  dependencies: [_desk_app_dep, _gio_unix_dep], include_directories: include_directories('src'), cpp_args: _warnings))
_dbus_run_session = find_program('dbus-run-session', required: false)
if _dbus_run_session.found()
  _picker_portal_dbus_exe = executable('picker_portal_dbus_test', 'tests/picker_portal_dbus_test.cpp',
    'src/picker_portal.cpp', dependencies: [_desk_app_dep, _gio_unix_dep], include_directories: include_directories('src'),
    cpp_args: _warnings)
  test('picker_portal_dbus', _dbus_run_session, args: ['--', _picker_portal_dbus_exe.full_path()],
    depends: _picker_portal_dbus_exe)
  _print_portal_dbus_exe = executable('print_portal_dbus_test', 'tests/print_portal_dbus_test.cpp',
    'src/print_portal.cpp', 'src/print_jobs.cpp', link_with: _files_lib,
    dependencies: _desk_app_dep, include_directories: include_directories('src'), cpp_args: _warnings)
  test('print_portal_dbus', _dbus_run_session, args: ['--', _print_portal_dbus_exe.full_path()],
    depends: _print_portal_dbus_exe)
endif
test('admin_session', executable('admin_session_test', 'tests/admin_session_test.cpp', link_with: _files_lib,
  dependencies: _desk_app_dep, include_directories: include_directories('src'), cpp_args: _warnings))
test('trash', executable('trash_test', 'tests/trash_test.cpp', link_with: _files_lib, dependencies: _desk_app_dep,
  include_directories: include_directories('src'), cpp_args: _warnings))
test('vaults', executable('vaults_test', 'tests/vaults_test.cpp', 'src/vaults.cpp', link_with: _files_lib,
  dependencies: [_desk_app_dep, _gio_unix_dep], include_directories: include_directories('src'), cpp_args: _warnings))
test('service_menus', executable('service_menus_test', 'tests/service_menus_test.cpp', 'src/service_menus.cpp',
  link_with: _files_lib, dependencies: [_desk_app_dep, _gio_unix_dep], include_directories: include_directories('src'),
  cpp_args: _warnings))

install_data('data/dev.andromeda.Files.desktop', 'data/dev.andromeda.Photos.desktop',
  install_dir: get_option('datadir') / 'applications')
# DBusActivatable=true in the .desktop file: the bus starts the app by its id, then calls Activate or Open.
configure_file(
  input: 'data/dev.andromeda.Files.service.in',
  output: 'dev.andromeda.Files.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)
install_data('data/andromeda-files.portal', install_dir: get_option('datadir') / 'xdg-desktop-portal' / 'portals')
configure_file(
  input: 'data/org.freedesktop.impl.portal.desktop.andromeda.Files.service.in',
  output: 'org.freedesktop.impl.portal.desktop.andromeda.Files.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)
configure_file(
  input: 'data/org.freedesktop.FileManager1.service.in',
  # Own file name, as Dolphin does: nautilus owns org.freedesktop.FileManager1.service.
  output: 'dev.andromeda.Files.FileManager1.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)
configure_file(
  input: 'data/andromeda-files-reconnect.service.in',
  output: 'andromeda-files-reconnect.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user',
)
install_data('data/icons/dev.andromeda.Files.svg', 'data/icons/dev.andromeda.Photos.svg', install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
install_data('translations/en.json', install_dir: get_option('datadir') / 'andromeda' / 'translations' / 'files')
