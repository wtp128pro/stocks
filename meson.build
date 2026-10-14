project(
  'stocks', 'c',
  version: '0.2.3',
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

# Hardening. Every flag is probed so an unsupported compiler/linker just skips
# it; none of these turn warnings into errors. Flags that only work together
# are probed as a group, so one of them cannot be silently dropped on its own.
#
# Stack canaries on functions with local arrays or address-taken locals.
# Shadowed declarations hide bugs in nested scopes.
hardening_cflags = cc.get_supported_arguments('-fstack-protector-strong', '-Wshadow')
# Format-string checks: reject non-literal format strings without arguments.
# -Wformat-security is ignored (with a warning) without -Wformat, which is
# implied by -Wformat=2, so the two are probed together.
if cc.has_multi_arguments('-Wformat=2', '-Wformat-security')
  hardening_cflags += ['-Wformat=2', '-Wformat-security']
endif
# Fortified libc wrappers need optimisation to take effect (glibc warns
# without it, which -Dwerror=true turns into an error), so only enable them
# when the effective compile is optimising. Meson puts its own -O flag (none
# for 'plain') before the user's c_args (CFLAGS/CPPFLAGS), and the last -O
# wins, so __OPTIMIZE__ is probed with both, in that order. A
# _FORTIFY_SOURCE already set there (e.g. by distro build flags) is left
# alone instead of being redefined. -U avoids a "redefined" warning when the
# toolchain defines it by default; it is probed together with -D, since -D
# alone would trip that very warning.
opt = get_option('optimization')
opt_args = (opt == 'plain') ? [] : ['-O' + opt]
effective_cargs = opt_args + get_option('c_args')
optimizing = cc.get_define('__OPTIMIZE__', args: effective_cargs) != ''
if optimizing and cc.get_define('_FORTIFY_SOURCE', args: effective_cargs) == ''
  if cc.has_multi_arguments('-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=2')
    hardening_cflags += ['-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=2']
  endif
endif
add_project_arguments(hardening_cflags, language: 'c')

# Read-only relocations (relro) plus immediate binding (now) make the GOT
# read-only after startup, blocking GOT overwrite attacks.
add_project_link_arguments(
  cc.get_supported_link_arguments('-Wl,-z,relro', '-Wl,-z,now'),
  language: 'c',
)

subdir('src')
subdir('data')
