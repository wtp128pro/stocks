core_inc = include_directories('.')

core_lib = static_library(
  'stk-core',
  'stk-types.c',
  'stk-parse.c',
  'stk-market.c',
  include_directories: core_inc,
  dependencies: [glib_dep, m_dep],
)

core_dep = declare_dependency(
  include_directories: core_inc,
  link_with: core_lib,
  dependencies: [glib_dep, m_dep],
)

test_core = executable(
  'test-core',
  'test-core.c',
  dependencies: core_dep,
)

test('core', test_core, suite: 'core')
