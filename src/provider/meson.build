gio_dep = dependency('gio-2.0', version: '>= 2.68')
soup_dep = dependency('libsoup-3.0', version: '>= 3.0')
# >= 1.10: json-glib 1.10 added the parser's nesting-depth cap; older versions
# overflow the stack on deeply nested input, and this module parses untrusted
# JSON.
json_dep = dependency('json-glib-1.0', version: '>= 1.10.0')

provider_deps = [core_dep, gio_dep, soup_dep, json_dep]

provider_inc = include_directories('.')

provider_lib = static_library(
  'stk-provider',
  'stk-provider.c',
  'stk-nasdaq.c',
  include_directories: provider_inc,
  dependencies: provider_deps,
)

provider_dep = declare_dependency(
  include_directories: provider_inc,
  link_with: provider_lib,
  dependencies: provider_deps,
)

# Offline: parsers run on captured fixtures; the request layer (retries,
# timeouts, caps, cancellation) runs against a loopback fake server. Nothing
# here touches the Internet.
test_nasdaq = executable(
  'test-nasdaq',
  'test-nasdaq.c',
  dependencies: provider_dep,
)

test(
  'nasdaq',
  test_nasdaq,
  args: [meson.current_source_dir() / 'fixtures'],
  suite: 'provider',
  timeout: 120,
)
