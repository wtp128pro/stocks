gnome = import('gnome')

gtk_dep = dependency('gtk4', version: '>= 4.14', include_type: 'system')
gio_dep = dependency('gio-2.0', version: '>= 2.68')

ui_inc = include_directories('.')

# The refresh policy is pure logic (GLib + core only): it has its own library
# so that its unit test needs neither GTK nor a display.
scheduler_lib = static_library(
  'stk-scheduler',
  'stk-scheduler.c',
  include_directories: ui_inc,
  dependencies: core_dep,
)

scheduler_dep = declare_dependency(
  include_directories: ui_inc,
  link_with: scheduler_lib,
  dependencies: core_dep,
)

# style.css is compiled into the binary; the app reads no resource from disk.
ui_resources = gnome.compile_resources(
  'stk-ui-resources',
  'stocks.gresource.xml',
  source_dir: meson.current_source_dir(),
  c_name: 'stk_ui',
)

# Generated resource code is not ours to keep warning-free under -Wpedantic.
ui_resources_lib = static_library(
  'stk-ui-resources',
  ui_resources[0],
  c_args: ['-Wno-overlength-strings'],
  dependencies: gio_dep,
)

ui_deps = [core_dep, provider_dep, indices_dep, storage_dep, catalog_dep, chart_dep, picker_dep, scheduler_dep, gtk_dep]

ui_lib = static_library(
  'stk-ui',
  'stk-row.c',
  'stk-detail.c',
  'stk-window.c',
  'stk-app.c',
  ui_resources[1],
  include_directories: ui_inc,
  link_with: ui_resources_lib,
  dependencies: ui_deps,
)

ui_dep = declare_dependency(
  include_directories: ui_inc,
  link_with: ui_lib,
  sources: ui_resources[1],
  dependencies: ui_deps,
)

executable(
  'stocks',
  'main.c',
  dependencies: ui_dep,
  install: true,
)

# Offline, no display: the scheduler is driven with a fake clock.
test_scheduler = executable(
  'test-scheduler',
  'test-scheduler.c',
  dependencies: scheduler_dep,
)

test('scheduler', test_scheduler, suite: 'ui')
