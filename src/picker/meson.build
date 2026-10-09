gtk_dep = dependency('gtk4', version: '>= 4.14', include_type: 'system')

picker_deps = [core_dep, provider_dep, storage_dep, catalog_dep, gtk_dep]

picker_inc = include_directories('.')

picker_lib = static_library(
  'stk-picker',
  'stk-picker.c',
  include_directories: picker_inc,
  dependencies: picker_deps,
)

picker_dep = declare_dependency(
  include_directories: picker_inc,
  link_with: picker_lib,
  dependencies: picker_deps,
)

# Needs a display: run under xvfb-run (the test exits 77, "skipped", when GTK
# cannot initialise). A fake StkProvider replaces the network.
# The test does not depend on being the active window, but its windows come and
# go on the shared display and change which window is active there, which other
# GUI tests that count repaints (chart-widget) observe: it runs alone.
test_picker = executable(
  'test-picker',
  'test-picker.c',
  dependencies: picker_dep,
)

test(
  'picker',
  test_picker,
  suite: 'picker',
  timeout: 180,
  is_parallel: false,
  env: {'GTK_A11Y': 'none', 'GSETTINGS_BACKEND': 'memory'},
)
