# Convenience wrapper over Meson/Ninja for "stocks". Meson stays the build
# system; every target here is a phony shortcut for commands you could type
# yourself. Needs GNU make >= 3.82 (not BSD make); `make` alone prints the help.
#
# Variables (override on the command line: `make install PREFIX=$HOME/stocks-test`):
#   PREFIX            install prefix, absolute      (default: $(HOME)/.local)
#   BUILDDIR          development build directory   (default: build)
#                     used by build, test, check, clean, distclean
#   RELEASE_BUILDDIR  release build directory       (default: build-release)
#                     used by release, install, uninstall, distclean; never BUILDDIR
#   DESTDIR           staging root for `make install` (default: empty)
#   RPMTOP            rpmbuild topdir for `make rpm`, absolute, outside the
#                     checkout (default: $(HOME)/.cache/stocks-rpmbuild)

ifeq ($(filter oneshell,$(.FEATURES)),)
$(error GNU make >= 3.82 is required (this make has no .ONESHELL))
endif

PREFIX ?= $(HOME)/.local
BUILDDIR ?= build
RELEASE_BUILDDIR ?= build-release
DESTDIR ?=
RPMTOP ?= $(HOME)/.cache/stocks-rpmbuild

SPEC := packaging/rpm/stocks.spec
# Written by `make rpm` into RPMTOP; `make distclean` deletes only a RPMTOP that has it.
RPM_MARKER := .stocks-rpmbuild-topdir

# Every recipe is one shell script that stops at the first error. The Make
# variables reach it through the environment, so no value is ever spliced into
# a shell command line: use "$$VAR" below, never $(VAR).
.ONESHELL:
.SHELLFLAGS := -eu -c
.DEFAULT_GOAL := help
export PREFIX BUILDDIR RELEASE_BUILDDIR DESTDIR RPMTOP

.PHONY: help all build test check release install uninstall rpm clean distclean

# Shell helpers shared by the recipes.
#   die MSG            print "make: MSG" to stderr and fail
#   need TOOL PACKAGE  fail, naming the tool, unless TOOL is in PATH
#   absp PATH          absolute path with symlinks resolved; PATH may not exist
#   under PATH DIR     true if PATH is DIR or inside it
define PRELUDE
die() { echo "make: $$*" >&2; exit 1; }
need() { command -v "$$1" >/dev/null 2>&1 || die "required tool '$$1' not found in PATH (Fedora package: $$2)"; }
need realpath coreutils
absp() { realpath -m -- "$$1"; }
under() { case "$$1" in "$$2"|"$$2"/*) return 0;; esac; return 1; }
case "$$BUILDDIR" in -*) die "BUILDDIR must not start with '-' (got '$$BUILDDIR'); write ./$$BUILDDIR if you mean a relative directory";; esac
case "$$RELEASE_BUILDDIR" in -*) die "RELEASE_BUILDDIR must not start with '-' (got '$$RELEASE_BUILDDIR'); write ./$$RELEASE_BUILDDIR if you mean a relative directory";; esac
endef

# PREFIX must be an absolute path other than /; it is replaced by its normalised
# form (symlinks resolved, no //, no trailing /), which is what meson gets and
# what uninstall compares against.
define NORM_PREFIX
case "$$PREFIX" in
  *'
'*) die "PREFIX must not contain a newline";;
  /*) ;;
  *) die "PREFIX must be an absolute path (got '$$PREFIX')";;
esac
PREFIX=$$(absp "$$PREFIX")
[ "$$PREFIX" != / ] || die "PREFIX must not be / ; use the RPM for a system-wide install"
endef

# Refresh the icon cache and desktop database of $PREFIX after install and
# uninstall. Only when DESTDIR is empty. Never creates an icon-theme.cache, only
# updates one that exists. A missing or failing tool is a note, not an error.
define REFRESH_CACHES
if [ -n "$${DESTDIR:-}" ]; then
  echo "DESTDIR is set: icon cache and desktop database are not refreshed."
else
  icondir="$$PREFIX/share/icons/hicolor"
  if [ -f "$$icondir/icon-theme.cache" ]; then
    if command -v gtk4-update-icon-cache >/dev/null 2>&1; then tool=gtk4-update-icon-cache
    elif command -v gtk-update-icon-cache >/dev/null 2>&1; then tool=gtk-update-icon-cache
    else tool=
    fi
    if [ -z "$$tool" ]; then
      echo "note: neither gtk4-update-icon-cache nor gtk-update-icon-cache found; $$icondir/icon-theme.cache left as it is"
    else
      "$$tool" -q -t -f "$$icondir" || echo "note: $$tool failed (ignored)"
    fi
  else
    echo "note: no $$icondir/icon-theme.cache; none created"
    # Only bump the directory's mtime (so a running desktop notices); never create anything.
    touch -c -- "$$icondir" 2>/dev/null || true
  fi
  appdir="$$PREFIX/share/applications"
  if command -v update-desktop-database >/dev/null 2>&1; then
    if [ -d "$$appdir" ]; then
      update-desktop-database -q "$$appdir" || echo "note: update-desktop-database failed (ignored)"
    fi
  else
    echo "note: update-desktop-database not found; desktop database not refreshed"
  fi
fi
endef

# safe_rm DIR KIND MODE: with MODE check only validate, with MODE remove delete
# DIR (KIND meson: a Meson build directory; KIND rpm: a topdir made by
# `make rpm`). Refuses an empty name, /, the checkout, $HOME, a build directory
# without meson-private/coredata.dat and a topdir without the marker. A
# directory that does not exist is skipped.
define SAFE_RM
safe_rm() {
  [ -n "$$1" ] || die "refusing to remove: empty directory name"
  p=$$(absp "$$1")
  here=$$(pwd -P)
  home=$$(absp "$${HOME:-/}")
  case "$$p" in
    /|"$$here"|"$$home") die "refusing to remove '$$1' (it resolves to $$p)";;
  esac
  [ -e "$$p" ] || { [ "$$3" = check ] || echo "$$1: nothing to remove"; return 0; }
  case "$$2" in
    meson) [ -f "$$p/meson-private/coredata.dat" ] || die "refusing to remove '$$p': not a Meson build directory (no meson-private/coredata.dat)";;
    rpm) [ -f "$$p/$(RPM_MARKER)" ] || die "refusing to remove '$$p': it has no $(RPM_MARKER) marker, so 'make rpm' did not create it";;
  esac
  [ "$$3" = check ] && return 0
  rm -rf -- "$$p"
  echo "removed $$p"
}
endef

help:
	@printf '%s\n' \
	  'stocks - convenience targets over Meson/Ninja (GNU make >= 3.82 required)' \
	  '' \
	  '  make help        this text (default target)' \
	  '  make build       configure BUILDDIR if needed, then compile (alias: make all)' \
	  '  make test        meson test in BUILDDIR; DISPLAY and WAYLAND_DISPLAY are unset,' \
	  '                   so the GUI tests (chart-widget, picker) SKIP; no window opens' \
	  '  make check       the whole suite in BUILDDIR, GUI tests included, under Xvfb inside' \
	  '                   `unshare -rmn` with a private /tmp and loopback only (safe on a desktop)' \
	  '  make release     configure RELEASE_BUILDDIR as a release build (buildtype, debug, optimization' \
	  '                   and prefix enforced and verified on every run), then compile; not BUILDDIR' \
	  '  make install     release, then a verified scratch install (5 files, modes, stripped binary),' \
	  '                   then `meson install --strip` into PREFIX (honours DESTDIR), then refresh' \
	  '                   the icon cache and desktop database; refuses root; never uses sudo' \
	  '  make uninstall   remove the files the last install logged (from' \
	  '                   RELEASE_BUILDDIR/meson-logs/install-log.txt, no rebuild), refresh caches' \
	  '  make rpm         source + binary RPM of git HEAD (packaging/rpm/stocks.spec) built under' \
	  '                   unshare -rmn; topdir RPMTOP, outside the checkout' \
	  '  make clean       remove BUILDDIR' \
	  '  make distclean   remove BUILDDIR, RELEASE_BUILDDIR and RPMTOP; run `make uninstall`' \
	  '                   first, the install log lives in RELEASE_BUILDDIR' \
	  '' \
	  'Variables (override on the command line, e.g. make install PREFIX=$$HOME/stocks-test):' \
	  '  (install only writes where you can; it never uses sudo - system-wide: use the RPM)'
	printf '  %-17s %s\n' PREFIX "$$PREFIX" BUILDDIR "$$BUILDDIR" RELEASE_BUILDDIR "$$RELEASE_BUILDDIR" \
	  DESTDIR "$$DESTDIR" RPMTOP "$$RPMTOP"

# Configure BUILDDIR if it is not a Meson build directory yet, then compile.
define BUILD_DEV
need meson meson
need ninja ninja-build
[ "$$BUILDDIR" ] || die "BUILDDIR is empty"
[ -f "$$BUILDDIR/meson-private/coredata.dat" ] || meson setup "$$BUILDDIR"
meson compile -C "$$BUILDDIR"
endef

all: build

build:
	@$(PRELUDE)
	$(BUILD_DEV)

test: build
	@$(PRELUDE)
	need meson meson
	# No display variable: the GUI tests exit 77 (SKIP) instead of opening a window on the desktop.
	exec env -u DISPLAY -u WAYLAND_DISPLAY GTK_IM_MODULE=simple GTK_A11Y=none \
	  meson test -C "$$BUILDDIR" --print-errorlogs

# The tmpfs over /tmp hides a checkout or build directory that lives in /tmp, so
# those are refused. The inner script gets the build directory as $1.
check:
	@$(PRELUDE)
	need unshare util-linux
	need ip iproute
	need xvfb-run xorg-x11-server-Xvfb
	here=$$(pwd -P)
	b=$$(absp "$$BUILDDIR")
	if under "$$here" /tmp; then die "the checkout ($$here) is under /tmp, which 'make check' hides with a tmpfs; use a checkout elsewhere"; fi
	if under "$$b" /tmp; then die "BUILDDIR ($$b) is under /tmp, which 'make check' hides with a tmpfs; use another BUILDDIR"; fi
	$(BUILD_DEV)
	exec env -u DISPLAY -u WAYLAND_DISPLAY GTK_IM_MODULE=simple GTK_A11Y=none \
	  unshare -rmn sh -c 'ip link set lo up && mount -t tmpfs tmpfs /tmp && exec xvfb-run -a meson test -C "$$1" --print-errorlogs' stocks-check "$$b"

# Release build, enforced on every run (also on an existing directory): the
# buildtype, debug, optimization, the install permissions (install_umask=022),
# the install directories (bindir=bin, datadir=share) and the other options that
# would turn a release build into something else, or change what `make install`
# installs and where, are passed explicitly and then verified with `meson
# introspect`. b_ndebug=false keeps asserts enabled, as the project's release
# buildtype does by default and as the test gate ran; b_lto, b_sanitize,
# b_coverage and b_pgo are reset to off; wrap_mode=nodownload keeps Meson off
# the network. STRIP is unset (it would select the tool at configure time).
# Flags in c_args/c_link_args (CFLAGS at the first configure) are not touched,
# but -g* and -O* in them are refused.
release:
	@$(PRELUDE)
	need meson meson
	need ninja ninja-build
	need python3 python3
	need id coreutils
	[ "$$(id -u)" -ne 0 ] || die "refusing to build as root: it would leave root-owned files in $$RELEASE_BUILDDIR. Run make as your own user; for a system-wide install use the RPM (make rpm, then dnf install)."
	[ "$$RELEASE_BUILDDIR" ] || die "RELEASE_BUILDDIR is empty"
	$(NORM_PREFIX)
	[ "$$(absp "$$RELEASE_BUILDDIR")" != "$$(absp "$$BUILDDIR")" ] || die "RELEASE_BUILDDIR and BUILDDIR are the same directory"
	unset STRIP
	set -- --buildtype=release -Ddebug=false -Doptimization=3 -Db_ndebug=false -Db_lto=false \
	  -Db_sanitize=none -Db_coverage=false -Db_pgo=off -Dinstall_umask=022 -Dbindir=bin -Ddatadir=share \
	  -Dwrap_mode=nodownload "--prefix=$$PREFIX"
	if [ -f "$$RELEASE_BUILDDIR/meson-private/coredata.dat" ]; then
	  meson setup --reconfigure "$$RELEASE_BUILDDIR" "$$@"
	else
	  meson setup "$$RELEASE_BUILDDIR" "$$@"
	fi
	meson introspect --buildoptions "$$RELEASE_BUILDDIR" | PREFIX="$$PREFIX" python3 -c "
	# make strips leading whitespace of recipe lines, so the checker has no indented blocks.
	import json, os, sys
	o = {x['name']: x['value'] for x in json.load(sys.stdin)}
	want = {'buildtype': 'release', 'debug': False, 'optimization': '3', 'b_ndebug': 'false', 'b_lto': False, 'b_sanitize': 'none', 'b_coverage': False, 'b_pgo': 'off', 'install_umask': 18, 'bindir': 'bin', 'datadir': 'share', 'wrap_mode': 'nodownload'}
	bad = ['%s=%s (want %s)' % (k, o.get(k), v) for k, v in want.items() if o.get(k) not in (v, [v])]
	bad += ['prefix=%s (want %s)' % (o.get('prefix'), os.environ['PREFIX'])] * (os.path.normpath(str(o.get('prefix'))) != os.path.normpath(os.environ['PREFIX']))
	bad += ['%s contains %s' % (k, w) for k in ('c_args', 'c_link_args') for w in (o.get(k) or []) if w.startswith(('-g', '-O'))]
	sys.exit('; '.join(bad) or None)
	" || die "$$RELEASE_BUILDDIR is not a release build after configuring (see above). Fix it with 'meson configure' (e.g. -Dc_args=) or remove the directory."
	meson compile -C "$$RELEASE_BUILDDIR"

# meson install offers to re-run itself through sudo/doas/run0/pkexec when it
# hits a permission error, but only if stdout and stderr are terminals. This
# Makefile never uses sudo: it refuses a prefix that this user cannot write and
# runs meson with stdin from /dev/null and its output in a file (printed
# afterwards), so Meson can never elevate, however deep the permission error is.
#
# The install is verified, not trusted. First everything is installed into a
# scratch DESTDIR and checked: exactly 5 regular files, all under $PREFIX, the
# binary 0755 and the other four 0644, every directory 0755, and the binary has
# no .symtab and no .debug_* section. Only then is the real install made (the
# install log of an earlier install is put back after the scratch run, so that
# `make uninstall` keeps working), and its binary is checked again. Nothing is
# installed if the check of the scratch install fails.
install: release
	@$(PRELUDE)
	need strip binutils
	need readelf binutils
	need mktemp coreutils
	need stat coreutils
	need find findutils
	$(NORM_PREFIX)
	unset STRIP
	if [ -n "$$DESTDIR" ]; then
	  final="$$(absp "$$DESTDIR")"
	  set -- --destdir "$$final"
	else
	  final=
	  set --
	  a="$$PREFIX"
	  while [ ! -e "$$a" ] && [ ! -L "$$a" ]; do a=$${a%/*}; [ -n "$$a" ] || a=/; done
	  [ -d "$$a" ] && [ -w "$$a" ] && [ -x "$$a" ] || die "cannot install into $$PREFIX: $$a is not a directory you can write. This Makefile never uses sudo; choose a PREFIX you own (e.g. make install PREFIX=\$$HOME/stocks-test), or use the RPM for a system-wide install."
	fi
	stage=$$(mktemp -d "$${TMPDIR:-/tmp}/stocks-stage.XXXXXX")
	trap 'rm -rf "$$stage" "$$stage.out" "$$stage.log" "$$stage.lst"' EXIT
	trap 'exit 130' INT TERM HUP
	log="$$RELEASE_BUILDDIR/meson-logs/install-log.txt"
	# run_install [meson install args]: output to $stage.out, status in $rc
	run_install() {
	  if meson install -C "$$RELEASE_BUILDDIR" --no-rebuild --strip "$$@" </dev/null >"$$stage.out" 2>&1; then rc=0; else rc=$$?; fi
	}
	# check_stripped FILE WHAT ADVICE: no symbol table and no debug sections
	check_stripped() {
	  [ -f "$$1" ] || die "$$2 ($$1) does not exist"
	  secs=$$(readelf -S -W "$$1") || die "readelf cannot read $$1"
	  if printf '%s\n' "$$secs" | grep -q -E '\.symtab|\.debug_'; then
	    die "$$2 ($${1#$$stage}) is NOT stripped (it has a .symtab or .debug_* section): the strip tool used by 'meson install --strip' did not work (STRIP, a native file or the build directory may select another tool). $$3"
	  fi
	}
	[ ! -f "$$log" ] || cp -p "$$log" "$$stage.log"
	run_install --destdir "$$stage"
	if [ -f "$$stage.log" ]; then mv -f "$$stage.log" "$$log"; else rm -f "$$log"; fi
	[ "$$rc" -eq 0 ] || { cat "$$stage.out"; die "the scratch install failed (exit $$rc), see above; nothing was installed."; }
	sroot="$$stage$$PREFIX"
	find "$$stage" -type f > "$$stage.lst"
	n=0
	while IFS= read -r f; do
	  n=$$((n + 1))
	  case "$$f" in "$$sroot"/*) ;; *) die "the install would put '$${f#$$stage}' outside PREFIX ($$PREFIX); an option stored in $$RELEASE_BUILDDIR redirects it. Nothing was installed.";; esac
	  want=644
	  [ "$$f" != "$$sroot/bin/stocks" ] || want=755
	  m=$$(stat -c %a -- "$$f")
	  [ "$$m" = "$$want" ] || die "the install would give '$${f#$$stage}' mode $$m instead of $$want. Nothing was installed."
	done < "$$stage.lst"
	[ "$$n" -eq 5 ] || die "the install would put $$n files, not 5, under $$PREFIX. Nothing was installed."
	[ -z "$$(find "$$stage" ! -type f ! -type d -print -quit)" ] || die "the install would create something that is neither a file nor a directory. Nothing was installed."
	[ -z "$$(find "$$stage" -mindepth 1 -type d ! -perm 0755 -print -quit)" ] || die "the install would create a directory whose mode is not 0755. Nothing was installed."
	check_stripped "$$sroot/bin/stocks" "the binary of the scratch install" "Nothing was installed."
	run_install "$$@"
	cat "$$stage.out"
	[ "$$rc" -eq 0 ] || die "meson install failed (exit $$rc), see above. This Makefile never uses sudo; a permission error means that something under $$PREFIX$${DESTDIR:+ (or under DESTDIR)} is not writable by you."
	check_stripped "$$final$$PREFIX/bin/stocks" "the installed binary" "Run 'make uninstall' to remove it."
	$(REFRESH_CACHES)
	echo "Installed the release build under $$final$$PREFIX: 5 files, bin/stocks stripped (no .symtab, no .debug_*), modes 0755/0644."
	if [ -z "$$DESTDIR" ]; then
	  found=
	  oldifs=$$IFS; IFS=:; set -f
	  for e in $$PATH; do
	    case "$$e" in /*) if [ "$$(absp "$$e")" = "$$(absp "$$PREFIX/bin")" ]; then found=1; fi;; esac
	  done
	  IFS=$$oldifs; set +f
	  [ -n "$$found" ] || echo "note: $$PREFIX/bin is not in PATH; the desktop entry's Exec=stocks needs it on the session PATH"
	fi

# Driven by the install log alone: no ninja, no Meson, so nothing is reconfigured
# or rebuilt and the release directory is not modified, even when the tree no
# longer configures. The log is not trusted: an entry is acted on only if it is
# an absolute path without . or .. components whose parent directory (symlinks
# resolved, the entry itself never followed) lies strictly inside the normalised
# root, $PREFIX or $DESTDIR$PREFIX for a staged install. Any other entry is
# skipped with a warning and makes the target fail at the end. After removing a
# file, empty parent directories are removed upward, strictly inside the root;
# the root itself and any non-empty directory stay.
uninstall:
	@$(PRELUDE)
	$(NORM_PREFIX)
	log="$$RELEASE_BUILDDIR/meson-logs/install-log.txt"
	[ -f "$$log" ] || die "no install log ($$log): nothing to uninstall. Run 'make install' first; the log lists only the last install, and a DESTDIR install overwrites it."
	grep -q '^/' "$$log" || die "the install log ($$log) lists no files: nothing to uninstall."
	root="$$PREFIX"
	[ -z "$$DESTDIR" ] || root=$$(absp "$$(absp "$$DESTDIR")$$PREFIX")
	[ "$$root" != / ] || die "the uninstall root resolves to /; refusing"
	prune() {
	  d="$$1"
	  while case "$$d" in "$$root"/?*) true;; *) false;; esac; do
	    rmdir -- "$$d" 2>/dev/null || break
	    d=$${d%/*}
	  done
	}
	removed=0
	skipped=0
	skip() { echo "warning: skipping log entry '$$1': $$2" >&2; skipped=$$((skipped + 1)); }
	while IFS= read -r p || [ -n "$$p" ]; do
	  case "$$p" in
	    ''|'#'*) continue;;
	    /*) ;;
	    *) skip "$$p" "not an absolute path"; continue;;
	  esac
	  while case "$$p" in ?*/) true;; *) false;; esac; do p=$${p%/}; done
	  case "/$$p/" in
	    */../*|*/./*) skip "$$p" "has a . or .. component"; continue;;
	  esac
	  dir=$${p%/*}
	  [ -n "$$dir" ] || dir=/
	  np="$$(absp "$$dir")"
	  np="$${np%/}/$${p##*/}"
	  case "$$np" in
	    "$$root"/?*) ;;
	    *)
	      # The log also lists the directories the install created, up to
	      # the root and above; those are never removed, so they are no warning.
	      if [ -d "$$np" ] && [ ! -L "$$np" ]; then
	        case "$$root/" in "$${np%/}/"*) continue;; esac
	      fi
	      skip "$$p" "not inside $$root (a DESTDIR install needs the same DESTDIR and PREFIX here)"
	      continue;;
	  esac
	  if [ -d "$$np" ] && [ ! -L "$$np" ]; then
	    prune "$$np"
	    continue
	  fi
	  if [ -e "$$np" ] || [ -L "$$np" ]; then
	    rm -f -- "$$np"
	    removed=$$((removed + 1))
	  fi
	  prune "$${np%/*}"
	done < "$$log"
	echo "Uninstalled: $$removed file(s) removed from $$root (log: $$log)."
	$(REFRESH_CACHES)
	[ "$$skipped" -eq 0 ] || die "$$skipped log entries were skipped (see the warnings above); they were not touched."

# Builds git HEAD (not the working tree). The spec and the meson.build version
# are read from HEAD too, and the HEAD spec is the one rpmbuild gets (written to
# $RPMTOP/SPECS), so tarball, spec and versions always agree. The whole
# rpmbuild, %check included, runs in a private network namespace with its own
# /tmp, so RPMTOP and the checkout must not be under /tmp. An existing RPMTOP is
# reused only when it is empty or made by an earlier `make rpm`. RPMTOP is
# limited to a safe character set because rpmbuild expands it as an RPM macro
# value (whitespace, quotes, %, $ and the like make it write elsewhere).
# Nothing is created before every check has passed. rpmlint is not run.
rpm:
	@$(PRELUDE)
	need git git
	need rpmbuild rpm-build
	need unshare util-linux
	need ip iproute
	need xvfb-run xorg-x11-server-Xvfb
	here=$$(pwd -P)
	git rev-parse --verify -q HEAD >/dev/null || die "$$here is not a git checkout with a commit; 'make rpm' packages git HEAD"
	git cat-file -e "HEAD:$(SPEC)" 2>/dev/null || die "HEAD has no $(SPEC); run make from the repository root"
	version=$$(git show "HEAD:$(SPEC)" | sed -n '/^Version:/{s/^Version:[[:space:]]*//;s/[[:space:]]*$$//;p;q}')
	mversion=$$(git show "HEAD:meson.build" | sed -n "/^[[:space:]]*version:/{s/.*'\([^']*\)'.*/\1/p;q}")
	[ -n "$$version" ] && [ -n "$$mversion" ] || die "cannot read the version from HEAD:$(SPEC) ('$$version') or HEAD:meson.build ('$$mversion')"
	[ "$$version" = "$$mversion" ] || die "version mismatch in HEAD: $(SPEC) says $$version, meson.build says $$mversion"
	case "$$version" in *[!A-Za-z0-9._+~^-]*) die "unusable version '$$version' in HEAD:$(SPEC)";; esac
	case "$$RPMTOP" in /?*) ;; *) die "RPMTOP must be an absolute path (got '$$RPMTOP'); set it on the command line, e.g. make rpm RPMTOP=/var/tmp/stocks-rpmbuild";; esac
	top=$$(absp "$$RPMTOP")
	case "$$RPMTOP$$top" in
	  *[!A-Za-z0-9._/+@,=-]*) die "RPMTOP ('$$RPMTOP') may contain only letters, digits and . _ / + @ , = - : rpmbuild expands it as an RPM macro and would write elsewhere on whitespace, quotes, % or \$$. Set RPMTOP to a plain path under /var/tmp.";;
	esac
	if under "$$here" /tmp; then die "the checkout ($$here) is under /tmp, which 'make rpm' hides with a tmpfs; use a checkout elsewhere"; fi
	if under "$$top" /tmp; then die "RPMTOP ($$top) is under /tmp, which 'make rpm' hides with a tmpfs; use e.g. a directory under /var/tmp"; fi
	if under "$$top" "$$here"; then die "RPMTOP ($$top) is inside the checkout ($$here); use a directory outside it"; fi
	if [ -e "$$top" ] && [ ! -f "$$top/$(RPM_MARKER)" ]; then
	  [ -d "$$top" ] && [ -z "$$(ls -A "$$top")" ] || die "RPMTOP ($$top) exists, is not empty and was not made by 'make rpm'; use a new or empty directory"
	fi
	echo "warning: 'make rpm' packages git HEAD ($$(git rev-parse --short HEAD)); uncommitted changes in the working tree, $(SPEC) and meson.build included, are NOT used."
	echo "warning: it builds HEAD as version $$version (the version in HEAD:$(SPEC)), not the published v$$version release."
	if [ -n "$$(git status --porcelain)" ]; then echo "warning: the working tree has uncommitted changes (git status --short)."; fi
	mkdir -p "$$top/SOURCES" "$$top/SPECS"
	: > "$$top/$(RPM_MARKER)"
	git show "HEAD:$(SPEC)" > "$$top/SPECS/stocks.spec"
	git archive --prefix="stocks-$$version/" -o "$$top/SOURCES/stocks-$$version.tar.gz" HEAD
	env -u DISPLAY -u WAYLAND_DISPLAY \
	  unshare -rmn sh -c 'ip link set lo up && mount -t tmpfs tmpfs /tmp && exec rpmbuild -ba --define "_topdir $$1" "$$1/SPECS/stocks.spec"' stocks-rpm "$$top"
	echo "Built (rpmlint was not run):"
	find "$$top/RPMS" "$$top/SRPMS" -name '*.rpm' | sort

clean:
	@$(PRELUDE)
	$(SAFE_RM)
	safe_rm "$$BUILDDIR" meson remove

distclean:
	@$(PRELUDE)
	$(SAFE_RM)
	echo "note: the install log is in $$RELEASE_BUILDDIR; run 'make uninstall' before distclean to remove an installed copy."
	safe_rm "$$BUILDDIR" meson check
	safe_rm "$$RELEASE_BUILDDIR" meson check
	safe_rm "$$RPMTOP" rpm check
	safe_rm "$$BUILDDIR" meson remove
	safe_rm "$$RELEASE_BUILDDIR" meson remove
	safe_rm "$$RPMTOP" rpm remove
