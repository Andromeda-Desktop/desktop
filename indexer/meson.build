project('andromeda-index', 'cpp',
  version: '0.1.0',
  meson_version: '>=1.1',
  default_options: ['cpp_std=c++23', 'warning_level=3', 'werror=true', 'b_ndebug=if-release'],
)

sqlite_dep = dependency('sqlite3', version: '>=3.43') # FTS5 contentless_delete

core = static_library('index-core',
  'src/index_db.cpp',
  'src/name_fold.cpp',
  'src/crawler.cpp',
  'src/skip_rules.cpp',
  'src/folder_reader.cpp',
  'src/live_index.cpp',
  'src/query_server.cpp',
  'src/document_kind.cpp',
  'src/text_extract.cpp',
  'src/content_worker.cpp',
  'protocols/ipc_channel.cpp',
  include_directories: include_directories('.'),
  dependencies: [sqlite_dep, dependency('threads')],
)
core_dep = declare_dependency(link_with: core, include_directories: include_directories('src', '.'), dependencies: [sqlite_dep, dependency('threads')])

index_exe = executable('andromeda-index', 'src/main.cpp', dependencies: core_dep, install: true)
# The Rust extractor (indexer/extract) builds with cargo into this build dir, next to andromeda-index, which starts
# it from its own directory.
cargo = find_program('cargo')
_cargo_profile = get_option('buildtype') == 'debug' ? 'debug' : 'release'
extract_exe = custom_target('andromeda-index-extract',
  output: 'andromeda-index-extract',
  build_always_stale: true,
  console: true,
  command: [
    'sh', '-c', '"$1" build --quiet --locked $2 --manifest-path "$3" --target-dir "$4" && cp "$4/$5/andromeda-index-extract" "$6"',
    '_', cargo, _cargo_profile == 'release' ? '--release' : '--profile=dev',
    meson.current_source_dir() / 'extract' / 'Cargo.toml', meson.current_build_dir() / 'cargo-target', _cargo_profile,
    '@OUTPUT@',
  ],
  install: true,
  install_dir: get_option('bindir'),
)

user_unit_dir = get_option('prefix') / 'lib' / 'systemd' / 'user'
configure_file(
  input: 'data/andromeda-index.service.in',
  output: 'andromeda-index.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: user_unit_dir,
)
install_data('data/andromeda-index.socket', install_dir: user_unit_dir)

test('index_core', executable('index_core_test', 'tests/index_core_test.cpp', dependencies: core_dep))
test('live_index', executable('live_index_test', 'tests/live_index_test.cpp', dependencies: core_dep))
test('serve', find_program('python3'), args: [files('tests/serve_test.py'), index_exe], depends: extract_exe, timeout: 60)
benchmark('query_100k', executable('query_bench', 'tests/query_bench.cpp', dependencies: core_dep), timeout: 300)
