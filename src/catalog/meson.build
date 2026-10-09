gnome = import('gnome')

json_glib_dep = dependency('json-glib-1.0', version: '>= 1.10.0')
gio_dep = dependency('gio-2.0', version: '>= 2.68')

# data/fidelity_catalog.json is compiled into the binary; the app never reads
# it from disk or the network. source_dir lets the XML reference ../../data.
catalog_resources = gnome.compile_resources(
  'stk-catalog-resources',
  'catalog.gresource.xml',
  source_dir: meson.current_source_dir(),
  c_name: 'stk_catalog',
)

catalog_inc = include_directories('.')

# glib-compile-resources emits the JSON as one long string literal, which
# -Wpedantic (warning_level=3) flags; the generated file is not ours to fix.
catalog_resources_lib = static_library(
  'stk-catalog-resources',
  catalog_resources[0],
  c_args: ['-Wno-overlength-strings'],
  dependencies: gio_dep,
)

catalog_lib = static_library(
  'stk-catalog',
  'stk-catalog.c',
  catalog_resources[1],
  include_directories: catalog_inc,
  link_with: catalog_resources_lib,
  dependencies: [core_dep, json_glib_dep, gio_dep],
)

catalog_dep = declare_dependency(
  include_directories: catalog_inc,
  link_with: catalog_lib,
  sources: catalog_resources[1],
  dependencies: [core_dep, json_glib_dep, gio_dep],
)

test_catalog = executable(
  'test-catalog',
  'test-catalog.c',
  dependencies: catalog_dep,
)

test('catalog', test_catalog, suite: 'catalog')
