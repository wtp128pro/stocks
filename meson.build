project(
  'stocks', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 1.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')

# GLib is the only dependency shared by every module; modules that need more
# (GTK, libsoup, json-glib, ...) declare those in their own meson.build.
glib_dep = dependency('glib-2.0', version: '>= 2.68')
m_dep = cc.find_library('m', required: false)

add_project_arguments(
  '-DG_LOG_DOMAIN="stocks"',
  '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_68',
  language: 'c',
)

subdir('src')
subdir('data')
