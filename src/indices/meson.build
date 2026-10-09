# Market indices: the Yahoo chart provider and the router that sends '^'
# symbols to it. gio/soup/json come from the provider module (same scope).
indices_deps = [core_dep, provider_dep, gio_dep, soup_dep, json_dep]

indices_inc = include_directories('.')

indices_lib = static_library(
  'stk-indices',
  'stk-yahoo.c',
  'stk-router.c',
  include_directories: indices_inc,
  dependencies: indices_deps,
)

indices_dep = declare_dependency(
  include_directories: indices_inc,
  link_with: indices_lib,
  dependencies: indices_deps,
)

# Offline: parsers run on captured fixtures; the request layer (politeness,
# retries, timeouts, caps, cancellation) and the router run against loopback
# fake servers and fake providers. Nothing here touches the Internet.
test_indices = executable(
  'test-indices',
  'test-indices.c',
  dependencies: indices_dep,
)

test(
  'indices',
  test_indices,
  args: [meson.current_source_dir() / 'fixtures'],
  suite: 'indices',
  timeout: 120,
)
