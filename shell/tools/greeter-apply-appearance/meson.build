# The root helper that copies the user's appearance into the greeter's state (pkexec, polkit action
# org.andromeda.greeter.sync-appearance). The policy pins it to bindir/andromeda-greeter-apply-appearance.
# Not installed here: the greeter package ships the helper, policy and tmpfiles beside the greeter user's sysusers
# entry (pkg/PKGBUILD package_andromeda-greeter-git), so shell and greeter release on their own.
_aa_args = ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow']
_aa_inc = include_directories('src')
_aa_policy_path = get_option('prefix') / get_option('datadir') / 'polkit-1' / 'actions' / 'org.andromeda.greeter.apply-appearance.policy'
_aa_support = files(
  'src/greeter/appearance_sync.cpp',
  'src/greeter/greetd_user.cpp',
  'src/greeter/privileged_state_paths.cpp',
  'src/greeter/greeter_config_io.cpp',
  'src/greeter/lockscreen_sync.cpp',
  'src/greeter/greeter_preferences.cpp',
  'src/tools/secure_appearance_sync.cpp',
)
_aa_passwordless = files('src/tools/passwordless_sync_policy.cpp', 'src/tools/passwordless_sync_policy_xml.cpp')

apply_appearance_exe = executable('andromeda-greeter-apply-appearance',
  sources: _aa_support + _aa_passwordless + files('src/tools/apply_appearance_main.cpp'),
  dependencies: [desk_ui_dep, nlohmann_dep, tomlplusplus_dep, libxml2_dep],
  include_directories: _aa_inc,
  cpp_args: _aa_args + ['-DNOCTALIA_GREETER_INSTALLED_POLICY_PATH="' + _aa_policy_path + '"'],
)

apply_appearance_policy = configure_file(
  input: 'data/org.andromeda.greeter.apply-appearance.policy.in',
  output: 'org.andromeda.greeter.apply-appearance.policy',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
)

if build_tests
  test('apply-appearance-passwordless-policy-xml',
    executable('passwordless-sync-policy-xml-test',
      sources: files('tests/passwordless_sync_policy_xml_test.cpp') + _aa_passwordless,
      dependencies: [libxml2_dep, nlohmann_dep],
      include_directories: _aa_inc,
      cpp_args: ['-DNOCTALIA_GREETER_INSTALLED_POLICY_PATH="' + _aa_policy_path + '"'],
    ),
    args: [apply_appearance_policy, get_option('prefix') / get_option('bindir') / 'andromeda-greeter-apply-appearance'],
  )
  test('apply-appearance-compatibility', find_program('sh'),
    args: [meson.current_source_dir() / 'tests/apply_appearance_compatibility_test.sh', apply_appearance_exe],
  )
  test('apply-appearance-passwordless-sync-help', apply_appearance_exe, args: ['passwordless-sync', '--help'])
  test('apply-appearance-legacy-staging',
    executable('legacy-staging-compatibility-test',
      sources: _aa_support + files('src/greeter/appearance_config.cpp', 'tests/legacy_staging_compatibility_test.cpp'),
      dependencies: [desk_ui_dep, nlohmann_dep, tomlplusplus_dep],
      include_directories: _aa_inc,
      cpp_args: _aa_args + ['-DNOCTALIA_GREETER_TESTING'],
    ),
  )
endif
