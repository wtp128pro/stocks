gtk_dep = dependency('gtk4', version: '>= 4.14', include_type: 'system')

chart_inc = include_directories('.')

chart_lib = static_library(
  'stk-chart',
  'stk-chart-geom.c',
  'stk-chart.c',
  'stk-sparkline.c',
  include_directories: chart_inc,
  dependencies: [core_dep, gtk_dep],
)

chart_dep = declare_dependency(
  include_directories: chart_inc,
  link_with: chart_lib,
  dependencies: [core_dep, gtk_dep],
)

test_chart_geom = executable(
  'test-chart-geom',
  'test-chart-geom.c',
  dependencies: chart_dep,
)
test('chart-geom', test_chart_geom, suite: 'chart')

test_chart_widget = executable(
  'test-chart-widget',
  'test-chart-widget.c',
  dependencies: chart_dep,
)
test('chart-widget', test_chart_widget, suite: 'chart')
