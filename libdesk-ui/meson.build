project('desk-ui', ['c', 'cpp'],
  version: '0.1.0',
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# With dev_tokens, tokens are writable globals so the shell can reload them live (ANDROMEDA_DEV_TOKENS).
_tokens_runtime = get_option('dev_tokens') ? ['-DTOKENS_RUNTIME'] : []
add_project_arguments(_tokens_runtime, language: 'cpp')

cc  = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

# Same standard-library settings as the shell: both link one archive, so the flags must match.
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
  add_project_arguments('-ffunction-sections', '-fdata-sections', language: ['c', 'cpp'])
endif

is_wayland = get_option('platform') == 'wayland'

if is_wayland
  egl_dep = dependency('egl', required: false)
  if not egl_dep.found()
    egl_dep = dependency('wayland-egl', required: false)
  endif
  if not egl_dep.found()
    egl_dep = dependency('epoxy')
  endif
  gles2_dep = dependency('glesv2', required: false)
  if not gles2_dep.found()
    gles2_dep = dependency('epoxy')
  endif
else
  egl_dep = dependency('egl')
  gles2_dep = dependency('glesv2')
endif

foreach _h : ['stb/stb_image_resize2.h', 'stb/stb_image_write.h']
  if not cc.has_header(_h)
    error(_h + ' not found')
  endif
endforeach

_system_deps = [
  dependency('freetype2'),
  dependency('fontconfig'),
  dependency('cairo'),
  dependency('cairo-ft'),
  dependency('pango', version: '>=1.50'),
  dependency('pangocairo'),
  dependency('pangoft2'),
  dependency('harfbuzz'),
  dependency('fribidi'),
  dependency('librsvg-2.0'),
  dependency('xkbcommon'),
  dependency('glib-2.0'),
  dependency('gobject-2.0'),
  dependency('gio-2.0'),
  dependency('gio-unix-2.0'), # GDesktopAppInfo: app icons by desktop id (ui/app_icon.cpp)
  dependency('nlohmann_json'),
  dependency('zlib'),
  cpp.find_library('dl', required: false),
]
# Load on first use on Linux (core/lazy_library.h); other platforms link them. Each entry: dependency, library name,
# LAZY_SONAME_<macro> that carries the soname of the library this build links against.
_lazy_deps = [
  [dependency('md4c'), 'md4c', 'MD4C'],
  [dependency('libwebp'), 'webp', 'WEBP'],
  [dependency('libwebpdemux'), 'webpdemux', 'WEBPDEMUX'],
  [dependency('libjxl'), 'jxl', 'JXL'],
  [dependency('libjxl_threads'), 'jxl_threads', 'JXL_THREADS'],
]
_lazy_sonames = []
foreach _l : _lazy_deps
  if host_machine.system() == 'linux'
    _system_deps += _l[0].partial_dependency(compile_args: true, includes: true)
    _lazy_path = run_command(cpp.cmd_array(), '-print-file-name=lib' + _l[1] + '.so', check: true).stdout().strip()
    _lazy_soname = run_command(find_program('readelf'), '-d', _lazy_path, check: true, env: {'LC_ALL': 'C'}).stdout()
    _lazy_soname = _lazy_soname.split('Library soname: [')[1].split(']')[0]
    _system_deps += declare_dependency(compile_args: '-DLAZY_SONAME_@0@="@1@"'.format(_l[2], _lazy_soname))
    _lazy_sonames += _lazy_soname
  else
    _system_deps += _l[0]
  endif
endforeach
# debian/lazy-depends.sh turns this list into package dependencies: dpkg-shlibdeps sees only linked libraries.
configure_file(output: 'lazy-sonames.txt', command: ['printf', '%s\\n'] + _lazy_sonames, capture: true)
if is_wayland
  _system_deps += [dependency('wayland-client'), dependency('wayland-egl'), dependency('sdbus-c++')]
else
  add_languages('objcpp', native: false)
  _system_deps += dependency('appleframeworks', modules: ['AppKit', 'QuartzCore'])
endif
_system_deps += [egl_dep, gles2_dep]

# ── Vendored: fzy matcher ─────────────────────────────────────────────────────
_fzy_lib = static_library('fzy',
  'third_party/fzy/src/match.c',
  c_args: ['-Wno-pedantic', '-Wno-conversion'],
  override_options: ['warning_level=0'],
)
_fzy_inc = include_directories('third_party/fzy/src', is_system: true)

_protocol_sources = []
_protocol_headers = []
# ── Wayland client protocols the toolkit binds ────────────────────────────────
if is_wayland
wayland_scanner    = find_program('wayland-scanner')
wayland_protos_dir = dependency('wayland-protocols').get_variable('pkgdatadir')

foreach _p : [
  ['xdg-output-unstable-v1', wayland_protos_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml'],
  ['xdg-shell', wayland_protos_dir / 'stable/xdg-shell/xdg-shell.xml'],
  ['xdg-activation-v1', wayland_protos_dir / 'staging/xdg-activation/xdg-activation-v1.xml'],
  ['xdg-system-bell-v1', wayland_protos_dir / 'staging/xdg-system-bell/xdg-system-bell-v1.xml'],
  ['xdg-decoration-unstable-v1', wayland_protos_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'],
  ['ext-idle-notify-v1', wayland_protos_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml'],
  ['ext-background-effect-v1', wayland_protos_dir / 'staging/ext-background-effect/ext-background-effect-v1.xml'],
  ['fractional-scale-v1', wayland_protos_dir / 'staging/fractional-scale/fractional-scale-v1.xml'],
  ['viewporter', wayland_protos_dir / 'stable/viewporter/viewporter.xml'],
  ['pointer-gestures-unstable-v1', wayland_protos_dir / 'unstable/pointer-gestures/pointer-gestures-unstable-v1.xml'],
  ['presentation-time', wayland_protos_dir / 'stable/presentation-time/presentation-time.xml'],
  ['idle-inhibit-unstable-v1', wayland_protos_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml'],
  ['keyboard-shortcuts-inhibit-unstable-v1', wayland_protos_dir / 'unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml'],
  ['text-input-unstable-v3', 'protocols/text-input-unstable-v3.xml'],
  ['cursor-shape-v1', 'protocols/cursor-shape-v1.xml'],
  ['hyprland-focus-grab-v1', 'protocols/hyprland-focus-grab-v1.xml'],
  ['wlr-output-management-unstable-v1', 'protocols/wlr-output-management-unstable-v1.xml'],
  ['dsk-surface-v1', 'protocols/dsk-surface-v1.xml'],
  ['dsk-app-menu-v1', 'protocols/dsk-app-menu-v1.xml'],
  ['xdg-foreign-unstable-v2', wayland_protos_dir / 'unstable/xdg-foreign/xdg-foreign-unstable-v2.xml'],
  ['primary-selection-unstable-v1', wayland_protos_dir / 'unstable/primary-selection/primary-selection-unstable-v1.xml'],
]
  _h = custom_target(_p[0] + '-client-header',
    input: _p[1],
    output: _p[0] + '-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  # public-code: apps that link the shared libdesk bind these interfaces from it.
  _protocol_sources += custom_target(_p[0] + '-client-source',
    input: _p[1],
    output: _p[0] + '-client-protocol.c',
    command: [wayland_scanner, 'public-code', '@INPUT@', '@OUTPUT@'],
    depends: _h,
  )
  _protocol_headers += _h
endforeach

# 'namespace' is a C++ keyword: rename it in the generated header only.
_wlr_ls_xml = 'protocols/wlr-layer-shell-unstable-v1.xml'
_wlr_ls_h = custom_target('wlr-layer-shell-unstable-v1-client-header',
  input: _wlr_ls_xml,
  output: 'wlr-layer-shell-unstable-v1-client-protocol.h',
  command: [
    'sh', '-c', '"$1" client-header "$2" "$3" && sed -i s/namespace/name_space/g "$3"',
    '_', wayland_scanner.full_path(), '@INPUT@', '@OUTPUT@',
  ],
)
_protocol_sources += custom_target('wlr-layer-shell-unstable-v1-client-source',
  input: _wlr_ls_xml,
  output: 'wlr-layer-shell-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'public-code', '@INPUT@', '@OUTPUT@'],
  depends: _wlr_ls_h,
)
_protocol_headers += _wlr_ls_h
endif

_protocol_headers += custom_target('tabler-glyphs-header',
  input: ['protocols/gen-tabler-glyphs.py', 'data/tabler.json'],
  output: 'tabler_glyphs.h',
  command: [find_program('python3'), '@INPUT0@', '@INPUT1@', '@OUTPUT@'],
)

_protocol_headers += custom_target('design-tokens-header',
  input: ['protocols/gen-design-tokens.py', 'protocols/design-tokens.toml'],
  output: 'design_tokens.h',
  command: [find_program('python3'), '@INPUT0@', '@INPUT1@', '@OUTPUT@'],
)

if get_option('install_toolkit_themes')
  subdir('data/themes')
endif

# ── Library ───────────────────────────────────────────────────────────────────
_desk_ui_sources = files(
  'src/core/blocking_pool.cpp',
  'src/core/deferred_call.cpp',
  'src/core/freedesktop_key_file.cpp',
  'src/core/files/directory_scanner.cpp',
  'src/core/files/file_info.cpp',
  'src/core/files/file_watcher.cpp',
  'src/core/files/media_info.cpp',
  'src/core/files/volumes.cpp',
  'src/core/files/file_model.cpp',
  'src/core/files/resource_paths.cpp',
  'src/core/input/keybind_matcher.cpp',
  'src/core/input/key_chord.cpp',
  'src/core/log_platform.cpp',
  'protocols/log.cpp',
  'src/core/memory_pressure_poll_source.cpp',
  'src/core/process/process_fds.cpp',
  'src/core/timer_manager.cpp',
  'src/core/ui_phase.cpp',
  'src/i18n/i18n_service.cpp',
  'src/i18n/language_tag.cpp',
  'src/motion/desk_motion.cpp',
  'src/platform/popup_place.cpp',
  'src/platform/surface.cpp',
  'src/render/animation/animation.cpp',
  'src/render/animation/animation_manager.cpp',
  'src/render/animation/motion_service.cpp',
  'src/render/backend/gles_framebuffer.cpp',
  'src/render/backend/gles_render_backend.cpp',
  'src/render/backend/gles_texture_manager.cpp',
  'src/render/backend/render_backend.cpp',
  'src/render/core/async_texture_cache.cpp',
  'src/render/core/color.cpp',
  'src/render/core/damage_region.cpp',
  'src/render/core/heif_library.cpp',
  'src/render/core/image_decoder.cpp',
  'src/render/core/image_encoder.cpp',
  'src/render/core/icon_color_scheme.cpp',
  'src/render/core/image_file_loader.cpp',
  'src/render/core/image_orientation.cpp',
  'src/render/core/shader_program.cpp',
  'src/render/core/startup_metrics.cpp',
  'src/render/core/shared_texture_cache.cpp',
  'src/render/core/stb_image_resize_impl.cpp',
  'src/render/core/thumbnail_pipeline.cpp',
  'src/render/core/thumbnail_service.cpp',
  'src/render/gl_shared_context.cpp',
  'src/render/programs/audio_spectrum_program.cpp',
  'src/render/programs/blur_program.cpp',
  'src/render/programs/countdown_ring_program.cpp',
  'src/render/programs/effect_program.cpp',
  'src/render/programs/fancy_audio_visualizer_program.cpp',
  'src/render/programs/glyph_program.cpp',
  'src/render/programs/graph_program.cpp',
  'src/render/programs/image_program.cpp',
  'src/render/programs/linear_gradient_program.cpp',
  'src/render/programs/rect_program.cpp',
  'src/render/programs/screen_corner_program.cpp',
  'src/render/programs/spinner_program.cpp',
  'src/render/programs/texture_shader_program.cpp',
  'src/render/programs/shader_scene_program.cpp',
  'src/render/programs/terminal_grid_program.cpp',
  'src/render/programs/wallpaper_mask_program.cpp',
  'src/render/programs/wallpaper_program.cpp',
  'src/render/render_context.cpp',
  'src/render/render_target.cpp',
  'src/render/scene/fancy_audio_visualizer_node.cpp',
  'src/render/scene/graph_node.cpp',
  'src/render/scene/terminal_grid_node.cpp',
  'src/render/scene/terminal_bidi.cpp',
  'src/render/scene/input_area.cpp',
  'src/render/scene/input_dispatcher.cpp',
  'src/render/scene/node.cpp',
  'src/render/scene/accessible.cpp',
  'src/render/text/cairo_glyph_renderer.cpp',
  'src/render/text/text_glyph_atlas.cpp',
  'src/render/text/cairo_text_renderer.cpp',
  'src/render/text/font_registry.cpp',
  'src/render/text/terminal_glyph_atlas.cpp',
  'src/render/text/terminal_sprites.cpp',
  'src/render/text/font_weight_catalog.cpp',
  'src/render/text/glyph_registry.cpp',
  'src/time/time_format.cpp',
  'src/time/time_locale.cpp',
  'src/ui/app_icon_colorization.cpp',
  'src/ui/builders.cpp',
  'src/ui/controls/box.cpp',
  'src/ui/controls/button.cpp',
  'src/ui/controls/checkbox.cpp',
  'src/ui/controls/collapsible.cpp',
  'src/ui/controls/color_picker.cpp',
  'src/ui/controls/color_swatch_preview.cpp',
  'src/ui/controls/context_menu.cpp',
  'src/ui/controls/context_menu_popup.cpp',
  'src/ui/controls/countdown_ring.cpp',
  'src/ui/controls/drag_source.cpp',
  'src/ui/controls/drop_zone.cpp',
  'src/ui/controls/flex.cpp',
  'src/ui/controls/glyph.cpp',
  'src/ui/controls/glyph_picker.cpp',
  'src/ui/controls/graph.cpp',
  'src/ui/controls/grid_tile.cpp',
  'src/ui/controls/grid_view.cpp',
  'src/ui/controls/image.cpp',
  'src/ui/controls/input.cpp',
  'src/ui/controls/label.cpp',
  'src/ui/controls/list_editor.cpp',
  'src/ui/controls/reorder_controller.cpp',
  'src/ui/controls/reorder_drag.cpp',
  'src/ui/controls/markdown_view.cpp',
  'src/ui/controls/progress_bar.cpp',
  'src/ui/controls/radio_button.cpp',
  'src/ui/controls/range_slider.cpp',
  'src/ui/controls/roving_list_nav.cpp',
  'src/ui/controls/screen_corner.cpp',
  'src/ui/controls/scrollbar.cpp',
  'src/ui/controls/scroll_view.cpp',
  'src/ui/controls/search_picker.cpp',
  'src/ui/controls/segmented.cpp',
  'src/ui/controls/select.cpp',
  'src/ui/controls/select_dropdown_popup.cpp',
  'src/ui/tooltip/tooltip_manager.cpp',
  'src/ui/controls/selection_list.cpp',
  'src/ui/controls/separator.cpp',
  'src/ui/controls/slider.cpp',
  'src/ui/controls/spacer.cpp',
  'src/ui/controls/spinner.cpp',
  'src/ui/controls/status_row.cpp',
  'protocols/dwindle_tree.cpp',
  'protocols/glass_material.cpp',
  'protocols/i18n_catalog.cpp',
  'src/ui/controls/stepper.cpp',
  'src/ui/controls/toggle.cpp',
  'src/ui/controls/virtual_grid_view.cpp',
  'src/ui/controls/virtual_list_view.cpp',
  'src/ui/dialogs/color_picker_dialog.cpp',
  'src/ui/dialogs/color_picker_dialog_popup.cpp',
  'src/ui/dialogs/dialog_popup_host.cpp',
  'src/ui/dialogs/file_dialog.cpp',
  'src/ui/dialogs/file_dialog_popup.cpp',
  'src/ui/dialogs/file_dialog_sheet.cpp',
  'src/ui/files/file_browser.cpp',
  'src/ui/files/file_cells.cpp',
  'src/ui/files/file_format.cpp',
  'src/ui/files/batch_rename.cpp',
  'src/ui/files/file_columns.cpp',
  'src/ui/files/file_gallery.cpp',
  'src/ui/files/this_pc.cpp',
  'src/ui/files/file_icon.cpp',
  'src/ui/files/type_ahead.cpp',
  'src/ui/dialogs/glyph_picker_dialog.cpp',
  'src/ui/dialogs/glyph_picker_dialog_popup.cpp',
  'src/ui/dialogs/layer_popup_host.cpp',
  'src/ui/dialogs/search_picker_popup.cpp',
  'src/ui/drag_drop_controller.cpp',
  'src/ui/contrast.cpp',
  'src/ui/theme_palette.cpp',
  'src/ui/palette.cpp',
  'src/ui/popup_chrome.cpp',
  'src/ui/scroll_into_view.cpp',
  'src/ui/split_layout.cpp',
  'src/ui/split_pane_focus.cpp',
  'src/ui/style.cpp',
  'src/ui/surface_shadow.cpp',
  'src/ui/visuals/audio_visualizer.cpp',
  'src/ui/visuals/fancy_audio_visualizer.cpp',
  'src/util/clamp.cpp',
  'src/util/fuzzy_match.cpp',
  'src/util/safe_regex.cpp',
  'src/util/string_utils.cpp',
)

# The OS layer: one implementation per OS behind a shared header.
_sys_sources = host_machine.system() == 'darwin' \
  ? files('src/core/sys/sys_macos.cpp', 'src/core/files/directory_watcher_macos.cpp',
      'src/core/freedesktop_thumbnails_none.cpp', 'src/a11y/a11y_roots_none.cpp') \
  : files('src/core/sys/sys_linux.cpp', 'src/core/files/directory_watcher.cpp', 'src/core/freedesktop_thumbnails.cpp',
      'src/a11y/atspi_bridge.cpp', 'src/ui/app_icon.cpp')
_sys_sources += files('src/core/sys/sys_posix.cpp')

# The Wayland backend, and code that still calls Linux-only APIs: `just macos-check` builds everything else.
_wayland_sources = files(
  'src/wayland/dsk_surface_client.cpp',
  'src/wayland/hyprland/focus_grab_service.cpp',
  'src/wayland/layer_surface.cpp',
  'src/wayland/output_probe.cpp',
  'src/wayland/popup_surface.cpp',
  'src/wayland/subsurface.cpp',
  'src/wayland/drag_and_drop.cpp',
  'src/wayland/pipe_io.cpp',
  'src/wayland/primary_selection.cpp',
  'src/wayland/drag_icon_surface.cpp',
  'src/wayland/frame_stats.cpp',
  'src/wayland/surface.cpp',
  'src/wayland/text_input_service.cpp',
  'src/wayland/toplevel_surface.cpp',
  'src/wayland/wayland_connection.cpp',
  'src/wayland/wayland_egl_window.cpp',
  'src/wayland/wayland_seat.cpp',
)

_cocoa_sources = files(
  'src/cocoa/cocoa_display.mm',
  'src/cocoa/cocoa_keys.cpp',
  'src/cocoa/cocoa_surface.mm',
)

# '.' adds the source root and the build root, where the generated headers land.
_desk_ui_inc = [
  include_directories('src'),
  include_directories('.'),
  include_directories('third_party/wuffs', is_system: true),
]

desk_ui_lib = static_library('desk_ui',
  sources: _desk_ui_sources + _sys_sources + (is_wayland ? _wayland_sources : _cocoa_sources) + _protocol_sources
    + _protocol_headers,
  dependencies: _system_deps,
  include_directories: _desk_ui_inc + [_fzy_inc],
  link_with: _fzy_lib,
  cpp_args: [
    '-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow',
    # paths::assetsRoot() looks for the installed noctalia asset bundle (fonts, translations).
    '-DNOCTALIA_INSTALL_PREFIX="' + get_option('prefix') + '"',
    '-DNOCTALIA_INSTALL_DATADIR="' + get_option('datadir') + '"',
    '-DNOCTALIA_SOURCE_ASSETS_DIR="' + (meson.project_source_root() / '..' / 'shell' / 'assets') + '"',
  ],
  objcpp_args: ['-std=c++23', '-fobjc-arc', '-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wconversion',
    '-Wshadow'],
)

desk_ui_dep = declare_dependency(
  link_with: [desk_ui_lib, _fzy_lib],
  dependencies: _system_deps,
  include_directories: _desk_ui_inc,
  sources: _protocol_headers,
  compile_args: _tokens_runtime,
)

if is_wayland and not meson.is_subproject()
  test('freedesktop_thumbnails', executable('freedesktop_thumbnails_test', 'tests/freedesktop_thumbnails_test.cpp',
    dependencies: desk_ui_dep))
  test('popup_place', executable('popup_place_test', 'tests/popup_place_test.cpp', dependencies: desk_ui_dep))
endif
