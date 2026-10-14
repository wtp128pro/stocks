# Stocks

A small GTK4 desktop watchlist for stocks, ETFs, Fidelity mutual funds and
the main US market indices,
written in C with Meson. It shows a list of the instruments you follow (price,
change, the chart range chosen for it) and, for the selected one, a price chart for
1D / 1M / 3M / 6M / 1Y. It is built to idle cheaply: no periodic timer runs
unless the US market is open, the window is visible and the network is up.

It does not trade, log in to a broker, sync a Fidelity account, show news,
send alerts or compute portfolio P&L, and it never will as part of this
project.

## Features

- Watchlist of stocks, ETFs, mutual funds and the market indices (S&P 500,
  Dow Jones, Nasdaq Composite, Russell 2000), with per-row menu (Move up,
  Move down, Remove) and drag-and-drop reordering. Stored locally.
- Quote and chart-range pill per row; chart of the selected row for 1D, 1M,
  3M, 6M and 1Y (remembered per row and shown in its pill).
- "Add" window: browse the built-in Fidelity ETF / mutual-fund catalog and the
  four market indices (filter by text, kind - all, ETFs, mutual funds, indices -
  and category) or search symbols online.
- Cache: the latest quote and series of every symbol are kept on disk, so the
  first window paints from the cache without waiting for the network, and
  the app still shows data when offline. A failed refresh keeps the cached
  data and dims it (stale marker; the detail pane says "Stale" or "Offline").
- Application id `io.github.wtp128pro.Stocks`.

## Data source (read this)

Market data for stocks, ETFs and mutual funds comes from the **unofficial Nasdaq web API**
(`https://api.nasdaq.com`): read-only HTTPS GETs, no account and no API key.
This is not a documented or supported public API. It is meant for personal,
low-volume use only; check Nasdaq's terms before using it for anything else.
It can change, throttle you or stop working at any time without notice, and
then quotes will simply fail to refresh (the cache and the stale marker keep
the window usable). Quotes are not guaranteed to be real-time. The detail pane
shows the date the quote refers to (New York calendar date, "As of Oct 08,
2026", or "NAV · Oct 08, 2026" for funds); the list rows show no such text.

The four **market indices** (S&P 500 `^GSPC`, Dow Jones `^DJI`, Nasdaq
Composite `^IXIC`, Russell 2000 `^RUT`) are the one exception: Nasdaq's API does
not serve them, so they come from a **second unofficial source**, the Yahoo
Finance chart endpoint (`https://query1.finance.yahoo.com/v8/finance/chart/<symbol>`),
used ONLY for those four symbols: read-only HTTPS GETs, no account, no API
key, no cookies, sent with the app's identifying User-Agent
`stocks-app/0.1 (personal use)`. Like the Nasdaq API it is not a
documented or supported public API and using it sits in a terms-of-service
gray area; it is meant for personal, low-volume use only, and you should
check Yahoo's terms before using it for anything else. To stay polite the Yahoo
provider makes at most one request at a time and starts requests at least 3 s
apart, and the app fetches the indices one at a time, using at most one of its
4 request slots (see "Refresh policy"). While an index request is
outstanding it takes 1 of the 4 slots, so the Nasdaq-backed symbols share the
other 3: they may start later, but are never starved. A full refresh of the
indices takes at least about 3 s per index job. An index's change is measured
against the previous session's close read from Yahoo's daily bars (the quote
request is `?range=5d&interval=1d`), never from the response's `previousClose`
field, which Yahoo has served stale (on 2026-10-13 it gave the S&P 500 and Dow
the close of Friday 2026-10-09, skipping Monday's session). The 1D chart of an
index therefore takes two requests (the intraday chart, then the daily bars
for its dashed previous-close line), about 3 s more. Yahoo may throttle (HTTP 429,
not retried) or break this endpoint at any time, or be slow or unreachable;
then only the index rows go stale (cached data is kept) while the
Nasdaq-backed symbols keep refreshing, with 3 slots while an index request is
outstanding. Index quotes are not guaranteed to be
real-time, and the symbol search in the Add window only searches Nasdaq (the
indices are found in the catalog, filter "Indices").

All network access goes through one interface, `StkProvider`
(`src/provider/stk-provider.h`); the Nasdaq implementation is `StkNasdaq`
(`src/provider/stk-nasdaq.h`, which lists the exact endpoints, timeouts and
retry policy) for stocks, ETFs and funds, and `StkYahoo`
(`src/indices/stk-yahoo.h`) for the market indices; `StkRouter`
(`src/indices/stk-router.h`) sends symbols starting with `^` to Yahoo and every
other symbol (and every search) to Nasdaq. A different data source means
writing another implementation of that interface; nothing else in the app
talks to the network.

## Install

### From the signed RPM repository (Fedora, recommended)

The RPMs and the repository metadata are signed. Add the repository and
install; `dnf` shows the key fingerprint and asks you to confirm it:

```sh
sudo curl -fsSLo /etc/yum.repos.d/stocks.repo https://wtp128pro.github.io/stocks/stocks.repo
sudo dnf install stocks
```

The signing key is

```
Stocks Release Signing Key <wtp128pro@users.noreply.github.com>
RSA 4096, created 2026-10-10, expires 2029-10-09
Fingerprint: 1910 F9DB 58D0 7BF5 83CC  BB6F 9226 BBD4 F352 B9A6
```

Compare the fingerprint with the one `dnf` prints, with the key file
`packaging/rpm/RPM-GPG-KEY-stocks` in this repository, with the GitHub release
notes and with https://wtp128pro.github.io/stocks/. The repository serves
Fedora 44 (`x86_64` and source) only, because the packages are built there.

### From a downloaded RPM (Fedora)

Download `stocks-0.2.2-1.<dist>.<arch>.rpm` and `RPM-GPG-KEY-stocks` from the
[GitHub releases](https://github.com/wtp128pro/stocks/releases) page, check the
signature and install:

```sh
gpg --show-keys RPM-GPG-KEY-stocks            # compare the fingerprint above
sudo rpm --import RPM-GPG-KEY-stocks
rpm -K stocks-0.2.2-1.*.rpm                   # must say "digests signatures OK"
sudo dnf install ./stocks-0.2.2-1.*.rpm
```

`rpm -K` also exits 0 for an unsigned package, which it reports as just
`digests OK`; look for `signatures OK`. The release also has `SHA256SUMS` and
detached signatures (`SHA256SUMS.asc`, `stocks-0.2.2.tar.gz.asc`):

```sh
gpg --import RPM-GPG-KEY-stocks
gpg --verify SHA256SUMS.asc SHA256SUMS && sha256sum -c SHA256SUMS
```

It installs the `stocks` command, a desktop entry (Office / Finance), an
AppStream metainfo file and a full-colour and a symbolic icon, and needs no
runtime dependency beyond the libraries `dnf` resolves from the binary and
`hicolor-icon-theme`. The source RPM (`stocks-0.2.2-1.*.src.rpm`) is on the
same page; see "Packaging notes" below to build it yourself.

### Build from source

Dependencies (pkg-config names): `gtk4` >= 4.14, `libsoup-3.0`,
`json-glib-1.0` >= 1.10.0, `glib-2.0` >= 2.68 (with `glib-compile-resources`),
plus Meson >= 1.0, Ninja and a C11 compiler. On Fedora:

```sh
sudo dnf install gcc meson ninja-build gtk4-devel libsoup3-devel json-glib-devel glib2-devel
# to run the whole test suite as well:
sudo dnf install desktop-file-utils libappstream-glib appstream xorg-x11-server-Xvfb
```

The tools and tests below need `python3`; the GUI tests and
`tools/perf_check.sh` need `Xvfb` (package `xorg-x11-server-Xvfb` on Fedora);
`xvfb-run` is used by the test command below but not by the script.

```sh
meson setup build              # configure (debugoptimized, warning_level=3)
meson compile -C build         # builds build/src/ui/stocks
```

If you already have a `build` directory from an older checkout, run
`meson setup --reconfigure build` instead. Meson re-runs the configuration
only when a `meson.build` it has already read changes, and `src/meson.build`
includes a module directory only if it exists at configure time, so a build
directory configured before a module directory appeared keeps ignoring it
(its targets and tests are silently missing).

To install, `meson install -C build` (default prefix `/usr/local`; choose
another one with `meson setup --prefix=... build`). It installs the binary,
the desktop entry, the metainfo file and the two icons under the prefix; it
does not refresh icon or desktop-entry caches.

### Makefile

The `Makefile` at the repository root is a thin wrapper over Meson and Ninja,
for convenience; Meson remains the build system. It needs **GNU make** (3.82
or newer, not BSD make) and no other dependency. `make` alone prints the help.
The `Makefile` is not in the v0.1.0 tarball (it was added afterwards) and is
not part of the published RPMs.

| Target | What it does |
| --- | --- |
| `make help` | The default target: the list of targets and the current variables. |
| `make build` (alias `make all`) | `meson setup $(BUILDDIR)` if that is not yet a Meson build directory, then `meson compile`. Always compiles, even though `build/` exists. This is the development build (the project default, `debugoptimized`). |
| `make test` | `meson test` in `$(BUILDDIR)` with `DISPLAY` and `WAYLAND_DISPLAY` unset and `GTK_IM_MODULE=simple GTK_A11Y=none`, so `chart-widget` and `picker` report SKIP and nothing can open a window on your desktop. |
| `make check` | The whole suite, GUI tests included, in `$(BUILDDIR)`: inside `unshare -rmn sh -c 'ip link set lo up && mount -t tmpfs tmpfs /tmp && ...'` (private `/tmp`, loopback only) under `xvfb-run -a`, with the same two environment variables. Safe on a desktop; refuses a checkout or `BUILDDIR` under `/tmp`. |
| `make release` | Configures `$(RELEASE_BUILDDIR)` as a release build (`--buildtype=release -Ddebug=false -Doptimization=3`, other options reset, `--prefix=$(PREFIX)`), on every run, also on an existing directory, verifies the result, then compiles. Never reads or writes `$(BUILDDIR)`. Refuses to run as root. |
| `make install` | `make release`, then a verified scratch install (5 files, modes, stripped binary), then `meson install --strip` (plus `--destdir` when `DESTDIR` is set) and, only when `DESTDIR` is empty, the cache refresh below. Refuses a `PREFIX` you cannot write; never uses `sudo`. |
| `make uninstall` | Deletes the files listed in `$(RELEASE_BUILDDIR)/meson-logs/install-log.txt` (no Meson, no Ninja, no rebuild), prunes the directories that became empty inside the prefix, then the same cache refresh. |
| `make rpm` | Source and binary RPMs of git `HEAD` (tarball, spec and version all from `HEAD`) in the isolated form of "Packaging notes"; prints the RPM paths; does not run `rpmlint`. |
| `make clean` | Removes `$(BUILDDIR)`. |
| `make distclean` | Removes `$(BUILDDIR)`, `$(RELEASE_BUILDDIR)` and `$(RPMTOP)`. |

Variables, all overridable on the command line (`make install PREFIX=$HOME/stocks-test`):

| Variable | Default | Used by |
| --- | --- | --- |
| `PREFIX` | `$(HOME)/.local` (absolute, not `/`; install needs a prefix you can write) | `release`, `install`, `uninstall` |
| `BUILDDIR` | `build` (must not start with `-`) | `build`, `test`, `check`, `clean`, `distclean` |
| `RELEASE_BUILDDIR` | `build-release` (must not start with `-`) | `release`, `install`, `uninstall`, `distclean` |
| `DESTDIR` | empty | `install`, `uninstall` |
| `RPMTOP` | `$(HOME)/.cache/stocks-rpmbuild` (absolute; plain characters only, see `make rpm`) | `rpm`, `distclean` |

- **Release, stripped install.** `make install` installs a **release** build,
  never the development one: it builds in its own directory
  (`RELEASE_BUILDDIR`, ignored by `.gitignore` as `/build-*/`). On every run,
  also on an existing directory, `make release` passes `--buildtype=release
  -Ddebug=false -Doptimization=3 -Db_ndebug=false -Db_lto=false
  -Db_sanitize=none -Db_coverage=false -Db_pgo=off -Dinstall_umask=022
  -Dbindir=bin -Ddatadir=share -Dwrap_mode=nodownload` and the normalised
  `--prefix` to `meson setup` (`--reconfigure` when the directory exists) with
  `STRIP` unset, then reads `meson introspect --buildoptions` back and stops
  if the buildtype is not `release`, `debug` is not false, `optimization` is
  not 3, any of the other options differs, the prefix is not the requested
  one, or `c_args` / `c_link_args` hold a `-g*` or `-O*` flag (for example
  from `CFLAGS` at the first configure; other flags in them are left alone;
  fix it with `meson configure -Dc_args= build-release`). A `build-release`
  that was configured with `-Ddebug=true`, `-Doptimization=0`,
  `-Db_lto=true`, `-Db_ndebug=true`, a sanitizer, another buildtype, another
  `install_umask`, an absolute `bindir` / `datadir` (from `meson configure`
  or a native file) or another prefix is therefore brought back to `-O3`
  without `-g`, permissions 0755/0644 and the install directories under the
  prefix. `b_ndebug=false` keeps the asserts enabled, as in the project's
  release buildtype and in the test runs, so "release" here means `-O3`, no
  debug information, no LTO, no sanitizer. The install is
  `meson install --strip`, and it is verified, not trusted: it is first made
  into a scratch `DESTDIR` (under `$TMPDIR`, removed afterwards), which must
  contain exactly five regular files, all under the prefix (so an option that
  redirects an install is caught, whatever sets it), `bin/stocks` with mode
  0755 and the other four 0644, every directory 0755, and `bin/stocks` must
  have no `.symtab` and no `.debug_*` section in `readelf -S` (`file` then
  says "stripped"). If any of that fails, nothing is installed and the message
  says why (for example a `strip` tool replaced through a native file). Only
  then is the real install made, and its `bin/stocks` is checked again before
  the success message, which claims "stripped" only after that. The install
  log of an earlier install is put back after the scratch run. `strip` and
  `readelf` (package `binutils`) are required. Exactly five files are
  installed under the prefix: `bin/stocks` (mode 0755), the desktop entry, the
  metainfo file and the two icons (0644). The project default
  (`debugoptimized`, used by `build/`) is unchanged. The suite passes against
  the release build too: `make release && make check BUILDDIR=build-release`
  (the first command guarantees that `build-release` is a release build). Not
  covered: tools and libraries picked up from your environment (`CC`,
  `PKG_CONFIG_PATH`, `NINJA`, `LDFLAGS` without `-g`/`-O`, `-DNDEBUG` in
  `CFLAGS`) are what you chose and are not second-guessed.
- **Caches.** Only when `DESTDIR` is empty, `install` and `uninstall` refresh
  the caches of `$(PREFIX)`: an `icon-theme.cache` that already exists in
  `$(PREFIX)/share/icons/hicolor` is updated with `gtk4-update-icon-cache` (or
  `gtk-update-icon-cache`) `-q -t -f`. A new one is never created: when there is
  none, the directory `$(PREFIX)/share/icons/hicolor` is only `touch -c`ed (its
  timestamp is bumped if it exists; nothing is created). `update-desktop-database`
  runs on `$(PREFIX)/share/applications` if it is
  installed (it writes `mimeinfo.cache` there, which `uninstall` leaves
  behind). A missing or failing cache tool only prints a note.
- **PATH.** The desktop entry says `Exec=stocks`, so `$(PREFIX)/bin`
  (`~/.local/bin` by default) must be on the `PATH` of your session for the
  launcher to find the binary. `make install` (without `DESTDIR`) prints a
  note if it is not on the `PATH` of the shell that ran it. The comparison is
  made after normalising both sides the same way (`realpath -m`: `//`, `.`,
  `..` and symlinks resolved), so a symlinked prefix or `PATH` entry gives no
  false note; empty and relative `PATH` entries are ignored. The note only
  concerns that shell: your desktop session's `PATH` may differ.
- **No sudo, no root.** The Makefile never calls `sudo`, and Meson cannot do
  it on its behalf. `meson install` offers to re-run itself through `sudo`,
  `doas`, `run0` or `pkexec` after a permission error when its output is a
  terminal. `make install` therefore (1) refuses a `PREFIX` when the nearest
  existing directory at or above it is not writable by you (for example
  `/usr/local` or `/opt/stocks`), before invoking Meson, and (2) runs Meson
  with its output captured in a temporary file that is printed afterwards, so
  that a permission error deeper inside the prefix just fails. Use a prefix
  you own, e.g. `make install PREFIX=$HOME/stocks-test`. `make release` (and
  so `make install`) also refuses to run as root, so a `sudo make install`
  cannot leave root-owned files in `build-release/`. For a system-wide
  install use the RPM (built with `make rpm`, or the published one) and
  `dnf`; the Makefile supports nothing else.
- **Uninstall.** `make uninstall` is driven by the install log alone,
  `$(RELEASE_BUILDDIR)/meson-logs/install-log.txt`: it runs neither Meson nor
  Ninja, so it never reconfigures or rebuilds, leaves the release directory
  unchanged (also when a `meson.build` is newer) and works even if the source
  tree no longer configures. It fails with a message when the log is missing
  or lists no files. The log holds only the **last** install: a later install
  overwrites it, and a `DESTDIR` install overwrites it with the staged paths,
  so remove a staged install with `make uninstall DESTDIR=... PREFIX=...`
  using the same `DESTDIR` and `PREFIX`. The log is not trusted. `PREFIX` must
  be an absolute path other than `/`; `PREFIX` and `DESTDIR` are normalised
  (`//`, trailing `/`, symlinks resolved), and a logged path is acted on only
  if it is absolute, has no `.` or `..` component and its parent directory
  (symlinks resolved; a logged symlink itself is removed, never followed) lies
  strictly inside `$(PREFIX)` (`$(DESTDIR)$(PREFIX)` for a staged install).
  Every other entry is left alone, with a warning, and the target then exits
  non-zero (for example a log from a `DESTDIR` install used without that
  `DESTDIR`, or a different `PREFIX` than at install time; the directories
  that the install created above the prefix are not warned about, only never
  removed). After deleting, it walks up from each removed file and removes
  parent directories that are now empty, but only strictly inside the prefix:
  the prefix itself and every directory that still holds something (for
  example `share/applications` with its `mimeinfo.cache`) stay. This holds
  after repeated installs too. Run `make uninstall` **before**
  `make distclean`, which deletes `$(RELEASE_BUILDDIR)` and with it the log.
- **`make rpm`.** It reads the version from the spec and from `meson.build`
  **as committed in `HEAD`** (`git show HEAD:...`), checks that they are equal,
  and runs `git archive --prefix=stocks-<version>/ HEAD` into
  `$(RPMTOP)/SOURCES/stocks-<version>.tar.gz`. The spec that `rpmbuild` gets is
  the one from `HEAD` too, copied to `$(RPMTOP)/SPECS/stocks.spec`; it does not
  run `rpmlint`. `RPMTOP` must be an absolute
  path outside the checkout and not under `/tmp` (the build runs with a tmpfs
  over `/tmp`; use e.g. a directory under `/var/tmp`). It may contain only
  letters, digits and `. _ / + @ , = -`: `rpmbuild` expands it as an RPM macro
  value, and one with whitespace, quotes, `%` or `$` would make it write
  outside the topdir, so `make rpm` refuses such a path (and a `$HOME` with a
  space in the default) before it creates anything. An existing `RPMTOP` must
  be empty or made by an earlier `make rpm` (it holds a
  `.stocks-rpmbuild-topdir` marker file). It builds `HEAD`, not the working
  tree: uncommitted changes, including edits of the spec or `meson.build`, are
  not packaged (the target says so), and commits made after a release tag are
  built under the spec's version, so the result is not that published release
  until the tag points at the commit.
- **`clean` and `distclean`** refuse an empty value, `/`, `.`, the checkout
  and `$HOME`, a build directory that has no `meson-private/coredata.dat` and
  an `RPMTOP` without the marker file; a directory that does not exist is
  skipped.

### Run

```sh
build/src/ui/stocks            # from the build directory
stocks                         # after installing
```

The application id is `io.github.wtp128pro.Stocks`; the installed desktop
entry, metainfo file and icons carry that name.

### Test

```sh
GTK_IM_MODULE=simple GTK_A11Y=none xvfb-run -a meson test -C build   # 13 tests in 9 suites; the GUI tests need a display
```

Run the test gate as `GTK_IM_MODULE=simple GTK_A11Y=none xvfb-run -a meson test
-C build`: without `GTK_IM_MODULE=simple` the GTK test processes connect to
your real `ibus-daemon`, which makes typing bursts on your desktop. `meson
test` without `xvfb-run` works only if a display is available; the test suites
that open windows pin `GDK_BACKEND=x11`, so run them under `xvfb-run -a` to
keep them off your real session. The picker suite takes about 20 s.

`make check` (see "Makefile") runs the whole suite in that spirit, but inside a
private user, mount and network namespace with its own `/tmp` and loopback, so
Xvfb shares neither its sockets nor its lock files with your session; `make
test` runs it with no display variable set, so the two GUI tests are skipped.

The 13 tests are in the suites `core`, `provider`, `indices`, `storage`,
`catalog`, `chart` (2 tests), `picker`, `ui` and `data` (4 tests). None needs
the network. Only `chart-widget` and `picker` need a display (they exit 77,
which Meson reports as skipped, when none can be opened). The `data` suite
checks the packaging files: `app-id` (a shell script,
`data/check-app-id.sh`, that compares the application id in
`src/ui/stk-app.h` with the desktop entry, metainfo and icon names and the
GResource paths, and rejects any leftover use of the retired id) always runs; the other
three run `desktop-file-validate`, `appstreamcli validate --no-net` and
`appstream-util validate-relax --nonet` on the desktop entry and the metainfo
file, and are registered only when the tool is installed (packages
`desktop-file-utils`, `appstream` and `libappstream-glib` on Fedora), so
without them the suite has 1 test and the total is 10.

## Refresh policy

The decisions live in `src/ui/stk-scheduler.h` (pure logic, unit-tested in
`src/ui/test-scheduler.c`):

- Requests are made only while the window is visible (mapped and not
  minimised) **and** the network is available.
- One refresh of everything (quotes, the 1M series of every row, the selected chart) on
  start-up/becoming visible, when the window regains focus, and when the
  network comes back. A job that succeeded less than 5 s ago is skipped.
- Every 60 s a periodic refresh of quotes (and the selected chart), **only
  while the US market is open** (Mon-Fri 09:30-16:00 America/New_York, NYSE
  holidays and 13:00 early closes by rule) and the window is visible.
  Mutual funds publish one NAV per day, so healthy funds are not part of the
  periodic round.
- With the market closed, the window hidden or the network offline no periodic
  or refresh timer is armed. A refresh deadline armed before the close fires at
  most once after it and triggers exactly one final round (the official
  close); then nothing is armed. Hiding the window or losing the network
  clears the deadline without a final round.
- The only other timers are one-shot: the save debounce (3 s after the first
  change) and the Add window's search debounce (300 ms), plus the provider's
  per-request deadlines (10 s per attempt) and, for the market indices, the
  Yahoo provider's pacing timer (a request waits until at least 3 s have
  passed since the previous Yahoo request started; it exists only while such
  a request is queued).
- Hiding the window or losing the network cancels requests in flight.
- At most 4 requests are outstanding at once. Index symbols are fetched one at
  a time: at most one index request is outstanding, and it takes one of
  those 4 slots. While it is outstanding (even a slow or hanging one), the
  other index jobs wait, and the Nasdaq-backed symbols share the other 3
  slots: they may start later than they would with all 4 slots free, but they
  are never starved, because the index request holds only one slot. A slow or
  unreachable Yahoo therefore leaves only the index rows stale, while the
  Nasdaq-backed symbols keep refreshing, at most 3 at a time until that index
  request finishes. A full refresh of the indices takes at
  least about 3 s per index job (the Yahoo provider starts requests at least
  3 s apart).
- A failed request keeps the cached data, marks the symbol stale, and is retried
  by the next refresh that includes it. The Nasdaq provider itself retries
  network errors, timeouts and HTTP 5xx twice with back-off (1 s, 2 s); the
  Yahoo provider (market indices) retries the same errors at most twice, never
  429 or other 4xx (see "Data source").
- Pre-market, the 1D chart of a stock or ETF keeps showing the last trading
  session until today's regular session has points: Nasdaq returns only
  pre-market points before 09:30 ET and the provider keeps the regular session
  only, so the refreshed 1D series is empty. An empty 1D series never replaces
  a non-empty one, on screen or in the cache file, and does not mark the symbol
  stale; only when no 1D series is held does the chart show "No data".

## Mutual funds: NAV only

Mutual funds have one price per day (NAV). They have no intraday data: the 1D
button is hidden for a selected fund, only 1M..1Y charts are shown, and the
quote is labelled "NAV".

## Fidelity catalog

The four market indices come from the separate `data/market_indices.json`
(kind `index`, category "Market indices"; also a GResource, merged with the
Fidelity catalog at load and shown by the "Indices" kind filter of the Add
window); the generator below does not touch it.

`data/fidelity_catalog.json` (390 entries: 84 ETFs, 306 mutual funds,
generated 2026-10-08) is compiled into the binary as a GResource; the app
reads it from memory and never touches the network for it. Regenerate it
(maintainer task, Python standard library only):

```sh
python3 tools/build_catalog.py                  # fetch, build, print a diff; writes nothing
python3 tools/build_catalog.py --write          # overwrite data/fidelity_catalog.json
python3 tools/build_catalog.py --verify --write # also check every symbol on api.nasdaq.com
python3 tools/build_catalog.py --self-test      # offline self-tests (this one needs no network)
python3 tools/build_catalog.py --offline-samples DIR   # parse saved pages instead of fetching
```

Rebuild the app afterwards (`meson compile -C build`). Caveats:

- The ETF list and fund table are scraped from Fidelity web pages
  (`fidelity.com/etfs/find-an-etf` and an institutional "average annual
  returns" page), cross-checked with the Nasdaq Trader symbol directories.
  A page layout change can break or empty the scrape; the script has size
  floors and prints a diff, so review it before `--write`.
- The catalog is a snapshot; it is not updated at run time. New or closed
  funds appear only after a regeneration. Funds closed to new investors are
  dropped by the script, but that is judged from the page at generation time.
- `--verify` makes one request per symbol, at least 2 s apart (a few
  hundred symbols take many minutes), and drops symbols that Nasdaq does not
  resolve.
- Known script quirk: a fund row whose cleaned name is empty is not
  pre-validated, and the app's catalog loader rejects the **whole** catalog
  because of it (`stk-catalog.c`: "missing or empty name"). The catalog is
  loaded when the Add button is first clicked; if it is rejected, the click
  logs a warning, opens nothing and greys out both Add buttons, so the Add
  window, including its online search, is unreachable until the app is
  rebuilt with a good catalog. Check the regenerated file before shipping it:
  `meson compile -C build && meson test -C build catalog` loads the compiled-in
  catalog through the app's own loader.
- Symbols that are not in the catalog can be found with the online search in
  the Add window, as long as the catalog itself loads.

## Files

| What | Path (defaults, honouring `XDG_*_HOME`) |
| --- | --- |
| Watchlist | `$XDG_CONFIG_HOME/stocks/watchlist.json` (`~/.config/stocks/watchlist.json`) |
| Quote/series cache | `$XDG_CACHE_HOME/stocks/cache.json` (`~/.cache/stocks/cache.json`) |

Both are JSON with a `"version": 1` member, written atomically (temporary file,
fsync, rename; mode 0600) at most once every 3 s after a change and at exit.
A file that is missing is normal on first run; one that cannot be read or
parsed is logged and treated as empty. The cache holds at most 200 symbols
and 250 000 series points (least recently used evicted) and a file larger than
16 MiB is refused. A `*.stk-tmp-XXXXXX` file next to them is residue of an
interrupted write and is removed at the next start once it is a minute old.
The app writes no other files. Delete the files to reset it.

Entries of kind `"index"` (the market indices) are stored in both files with
the same `"version": 1`. An OLDER build of the app does not know that kind: it
drops the `index` entries when it loads `watchlist.json` and writes the file
back without them on the next edit of the watchlist, so downgrading removes
the indices from your list. From `cache.json` it drops only the index quotes;
the chart series of the indices stay in the file.

Each watchlist entry also stores the view you chose for it: the chart range
(`"range"`) and whether the list row shows points instead of percent
(`"points"`). Both keys are optional and the file version stays 1, so older
files load with 1D/percent and an older build ignores the two keys (and
drops them when it next saves the watchlist).

## Performance

`tools/perf_check.sh [path/to/stocks]` (default `build/src/ui/stocks`) launches
the built binary on a private Xvfb display with a seeded watchlist (12
symbols) and cache in the documented formats, in a throw-away
`XDG_CONFIG_HOME`/`XDG_CACHE_HOME`/`XDG_DATA_HOME`, and prints a table:

1. time from launch to the first mapped window (X11 MapNotify; the first
   frame paints from the cache);
2. steady-state RSS (`VmRSS`) 10 s after the window appeared;
3. idle CPU over 60 s, `(utime + stime) / wall time` from `/proc/<pid>/stat`,
   with the window visible, and with it hidden.

It exits 1 if a value exceeds its budget (variables at the top of the
script) and 2 if the harness could not run or one of its premises (below) does
not hold. It takes about 71 s. Run it from the repository root after
`meson compile -C build`:

```sh
timeout -k 10 150 tools/perf_check.sh
```

(150 s is also the script's own time limit with the default parameters.)
Needs `Xvfb`, `python3` (ctypes + libX11) and util-linux `unshare`, `setpriv`
and `mount` with unprivileged user, mount, network and PID namespaces enabled.
Without them the script refuses to run (exit 2). Details of the method:

- **Isolation.** The whole measurement runs in one private user namespace with
  its own mount, network and PID namespaces (`unshare -rmnpf --mount-proc
  --kill-child`), with a private tmpfs mounted over `/tmp`. Xvfb's socket
  directory `/tmp/.X11-unix` and its `/tmp/.X<n>-lock` files are therefore not
  the host's: the script never opens, unlinks or rebinds the sockets of your
  running desktop (it also aborts if it can see any `/tmp/.X11-unix` or
  `/tmp/.X*-lock` inside). Listings of `/tmp/.X11-unix` and `/tmp/.X*-lock`
  taken with `ls -li` before and after runs of the script were identical.
  The Xvfb display number inside is meaningless (it is `:0` of the private
  `/tmp`). The script itself and the binary may live anywhere, including
  under `/tmp`: the script is handed to the namespace as an open file
  descriptor, and the binary's top-level `/tmp` entry is bind-mounted into the
  private `/tmp`. The inner stages of the script refuse to run (exit 2)
  unless they were started by the outer part: stage 1 (the one that mounts
  the tmpfs) only runs as pid 1 of a fresh PID namespace, so it can never
  touch the `/tmp` of the shell that called the script.
- **Stopping.** The outer part of the script traps SIGINT/SIGTERM, SIGKILLs
  `unshare` and waits until the namespace's pid 1 is gone, which takes every
  process inside (Xvfb, both app instances, helpers) with it; `setpriv
  --pdeathsig` does the same when the script itself is SIGKILLed. Tested on
  the final script (inside a throw-away mount namespace with decoy X sockets,
  see above) by sending SIGTERM, SIGINT (to the process group, like Ctrl-C)
  and SIGKILL to the script and SIGKILL to the whole process group at 8 s,
  by running it under `timeout -k 2 2` and `timeout -k 2 12`, and by SIGTERM
  and SIGKILL 0.05, 0.3, 0.8 and 1.5 s after the start (while the namespaces
  and Xvfb are still being set up): in every case no new Xvfb, `stocks`,
  `perf_check`, `unshare` or `setpriv` process existed 3 s later, and no
  scratch directory was left.
- **Offline.** The network namespace has only a down loopback, so the live API
  cannot be reached. That alone does not make the *app* offline: GLib's
  default network monitor asks NetworkManager over the host's system D-Bus
  socket (a file, not tied to the network namespace) and reports "online". The
  script therefore sets `GIO_USE_NETWORK_MONITOR=netlink`, which reads the
  namespace's own, empty, network state. The premise is verified in every run
  instead of assumed: the app runs with `G_MESSAGES_DEBUG=stocks`, which logs
  every request it makes, and the script fails with exit 2 if the log of
  either instance contains any `request #` line (and if no debug output is
  seen at all, so the check cannot pass vacuously). With no request issued the
  start-up refresh never happens, so the idle windows contain no refresh
  activity. Without the netlink setting the check does fail: the app then
  issues its start-up requests (tried). The market-open case (a refresh round
  every 60 s while the window is visible, per `stk-scheduler.h`) is **not**
  measured: expect the CPU wake-ups and network traffic of one round per
  minute on top of the idle numbers.
- **Hidden.** No window manager runs under Xvfb, so the script sets
  `_NET_WM_STATE_HIDDEN` on the window with plain Xlib (python3 ctypes), which
  is what a window manager does when it minimises one. GDK turns that into
  the "minimised" toplevel state and the app reports "not visible"; the script
  checks in the app's debug log that its last reported `window visible=` is 1
  for the visible instance and 0 for the hidden one. The hidden instance is a
  second process running at the same time as the visible one (same Xvfb,
  separate XDG directories), which keeps the run short. Offline, the
  scheduler is inactive in both instances, so the hidden number cannot show
  the saving of cancelling the 60 s timer; it shows what the process costs
  while minimised. The two idle numbers are within run-to-run noise of each
  other; the clock-tick resolution is 10 ms, i.e. 0.017 % over 60 s.
- **Rendering.** The script sets `GSK_DEBUG=renderer` and prints the renderer
  GTK picked. In the runs below it was `GskVulkanRenderer`, and the Vulkan
  device was not software: with `GDK_DEBUG=vulkan` GTK listed `Vulkan Device 0:
  NVIDIA GeForce RTX 5060 Ti (Discrete GPU)` and `Vulkan Device 1: llvmpipe
  (CPU)` and logged `Using Vulkan device 0`; the nvidia kernel modules were
  loaded and `libnvidia-*` libraries were mapped into the `stocks` process
  (checked in `/proc/<pid>/maps`). Only GL fell back to llvmpipe (`Not using
  GL: renderer is llvmpipe`). Xvfb itself is not accelerated; the app renders
  through the host's GPU driver anyway. **All budgets and the main table below
  were measured with that default renderer on that host.** The ~311 MiB RSS
  therefore includes the GPU driver's mappings and is host-dependent: on a
  machine with another or no Vulkan driver the default renderer, and the RSS,
  will differ. The cairo (~160 MiB) and ngl (~258 MiB) rows of the second
  table show how much the renderer matters. Treat the budgets as a regression
  guard for this host and setup, not as a general claim. `GSK_RENDERER=cairo
  tools/perf_check.sh` measures another renderer. The script exports
  `GTK_IM_MODULE=simple GTK_A11Y=none` for every process it starts (see
  "Input method" below).
- **Input method.** GTK processes under Xvfb with default settings connect to
  the host's real `ibus-daemon` (a socket under `~/.cache/ibus`), which makes
  the desktop's input method react. The script therefore sets
  `GTK_IM_MODULE=simple`; under `strace -f -e trace=connect` a short run
  showed no connect to any ibus socket (only the private X socket, the
  disabled session bus path and the system bus socket).

### Measured values and budgets

Sixteen runs of the script on the development machine (Intel Core Ultra 7
265K, NVIDIA GeForce RTX 5060 Ti, 20 CPUs, Linux 7.2 x86-64, GTK 4.22.5, Xvfb,
default renderer `GskVulkanRenderer` on the NVIDIA device, offline and verified
request-free; each row is a separate full run, nothing else started on
purpose). Runs 1-8 were made before one more assertion (the visibility check
above) was added to the script; runs 9-10 before the last change of the
start-up code (parameter validation, stage checks, handing the script down as a
file descriptor), runs 11-13 after it, runs 14-16 after `GTK_IM_MODULE=simple`
was added. None of these changes alters what is measured or how. Idle CPU
varied from 0.050 to 0.233 % (a few clock ticks; the cause of the variation
was not investigated), which is why its budget is set well above the worst
run:

| Run | first window (ms) | RSS visible (MiB) | idle CPU visible (%) | idle CPU hidden (%) |
| --- | --- | --- | --- | --- |
| 1 | 272 | 310.0 | 0.083 | 0.067 |
| 2 | 295 | 309.8 | 0.067 | 0.067 |
| 3 | 272 | 309.9 | 0.067 | 0.067 |
| 4 | 267 | 309.8 | 0.067 | 0.067 |
| 5 | 277 | 309.8 | 0.133 | 0.050 |
| 6 | 277 | 310.1 | 0.067 | 0.133 |
| 7 | 285 | 309.9 | 0.050 | 0.117 |
| 8 | 286 | 309.8 | 0.100 | 0.067 |
| 9 | 286 | 311.2 | 0.200 | 0.183 |
| 10 | 281 | 310.9 | 0.050 | 0.100 |
| 11 | 276 | 310.8 | 0.067 | 0.067 |
| 12 | 290 | 311.1 | 0.233 | 0.067 |
| 13 | 281 | 310.9 | 0.117 | 0.083 |
| 14 | 253 | 310.3 | 0.100 | 0.067 |
| 15 | 273 | 310.5 | 0.067 | 0.067 |
| 16 | 278 | 310.8 | 0.067 | 0.100 |
| **max** | **295** | **311.2** | **0.233** | **0.183** |

Budgets were set at about twice the worst of runs 1-10 and were kept when runs
11-16 stayed below them; the final script (with `GTK_IM_MODULE=simple`)
measured runs 14-16 and no measurement demanded a change (the idle CPU budget
is 1.7 times the worst value seen):

| Measurement | Budget variable | Value |
| --- | --- | --- |
| Launch to first mapped window | `BUDGET_FIRST_WINDOW_MS` | 600 ms |
| Steady-state RSS | `BUDGET_RSS_MIB` | 640 MiB |
| Idle CPU, visible | `BUDGET_CPU_VISIBLE_PCT` | 0.4 % of one core |
| Idle CPU, hidden | `BUDGET_CPU_HIDDEN_PCT` | 0.4 % of one core |

One run each with the other renderers (same machine and script, made before the
visibility assertion was added; not used for the budgets, single runs):

| `GSK_RENDERER` | reported renderer | first window (ms) | RSS visible (MiB) | idle CPU visible / hidden (%) |
| --- | --- | --- | --- | --- |
| unset (default) | `GskVulkanRenderer` (NVIDIA discrete GPU, see "Rendering") | 253-295 (16 runs) | 309.8-311.2 | 0.050-0.233 / 0.050-0.183 |
| `cairo` | `GskCairoRenderer` | 93 | 159.8 | 0.017 / 0.000 |
| `ngl` | `GskGLRenderer` | 98 | 257.5 | 0.000 / 0.000 |

The budgets and the timing parameters can be overridden from the
environment for experiments, e.g. `BUDGET_RSS_MIB=200 tools/perf_check.sh`
(fails, exit 1, with the default renderer here). `SETTLE_S` (>= 0),
`IDLE_S` and `MAP_TIMEOUT_S` (both >= 1) are plain decimal integers of at most
6 digits (leading zeros are read as decimal); the `BUDGET_*` variables are
non-negative decimal numbers (`0.4`, `600`). Anything else (`1.5`, `abc`,
empty, negative) exits 2 immediately, before anything is started.

## Known limits

- **Already running at the open.** There is no timer while the market is
  closed, so an instance that is left running (visible, online) before 09:30 ET
  does not start refreshing by itself at the open: it refreshes on the next
  show, focus regain or network event, and periodic refreshes begin one period
  after that round (`stk-scheduler.h`).
- **Final round after the close.** A window left visible and online across
  16:00 ET gets exactly one more refresh when the deadline armed before the
  close fires (up to one period after 16:00), then nothing. That round skips a
  quote or chart that succeeded in the 5 s before the close. A hidden or
  offline app gets no such round; showing it or reconnecting refreshes.
- **Pre-market 1D chart.** In pre-market any non-empty 1D series that is held
  is kept, even when it is older than the last session (for example after the
  app was not run for several days). Nasdaq returns only pre-market points
  before 09:30 ET and the provider keeps the regular session only, so the
  1D chart shows the last session until today's regular session has points.
- Quotes depend on the unofficial API (see above); a failed refresh shows
  the previous data dimmed, not an error dialog. Fidelity funds'
  NAV arrives once a day.
- After-hours and pre-market behaviour of the Nasdaq provider is inferred in
  part. For that provider the repository stores recorded responses only for
  after-hours quotes (`src/provider/fixtures/quote_stock.json`,
  `quote_etf.json`, and the 1D chart `chart_1d_etf.json`) plus a fund quote,
  daily charts, a fund history, a search and an error body. The open, closed
  and pre-market layouts are exercised only by hand-written bodies in
  `test-nasdaq.c`. `stk-nasdaq.h` says that real responses for Open,
  After-Hours and Closed were seen and that Pre-Market is inferred; the Open
  and Closed ones are not stored in the repository. The Yahoo provider's
  captured responses (market indices) are listed under "Data provenance and
  trademarks"; this limit is about the Nasdaq provider only.
- Market hours come from a built-in rule calendar for 2022-2100 (US NYSE
  holidays and early closes); outside it the market counts as closed.
- Quitting while the start-up load is still running (the main window is only
  created once the load has finished) waits for pending I/O for at most 5 s,
  then logs "Gave up waiting for pending saves/requests" and exits.
- Display servers: GTK picks Wayland or X11 as usual. The test suites, the
  measurements and the "hidden" check above were run on X11 under Xvfb only;
  the app relies on GDK's mapped/minimised/suspended toplevel state and the
  `is-active` property for visibility and focus and has not been exercised
  on a Wayland compositor in this project.
- The unit tests of the scheduler cover only its pure logic; the glue in
  `StkApp` was checked by live smoke runs, not by repository tests.

## Licence

Stocks is free software, licensed under the GNU General Public License,
version 3 or (at your option) any later version (`GPL-3.0-or-later`), the
licence used by GNOME applications. The full text is in [`COPYING`](COPYING);
every C source and header under `src/` starts with an SPDX identifier and a
copyright line (`Copyright (C) 2026 wtp128pro`). The AppStream metainfo file
is `CC0-1.0`, as AppStream recommends. The icons were drawn for this
project and are covered by the same licence as the program.

## Data provenance and trademarks

- `data/fidelity_catalog.json` is a factual list of the symbols, names and
  categories of funds and ETFs that Fidelity lists publicly. It is a snapshot
  (`generated` 2026-10-08) built by `tools/build_catalog.py` from public
  Fidelity web pages (`fidelity.com/etfs/find-an-etf` and an institutional
  "average annual returns" page), cross-checked with the Nasdaq Trader symbol
  directories (`nasdaqtrader.com`); the script's docstring lists the exact
  sources and rules. It holds no prices and no other Fidelity content.
  `data/market_indices.json` lists the four indices of "Data source".
- Quotes and charts come from the **unofficial** Nasdaq web API and, for the
  four indices, the **unofficial** Yahoo Finance chart endpoint, both
  keyless (see "Data source (read this)"). They are not documented or
  supported public APIs, are meant for personal, low-volume use only, may
  change, throttle you or stop working at any time, and their terms of use
  are yours to check.
- `src/provider/fixtures/` (Nasdaq API) and `src/indices/fixtures/` (Yahoo
  chart endpoint: `chart_1d_index.json`, `chart_5d_index.json`,
  `chart_1y_index.json` and `chart_error.json`) hold captured responses (the
  Yahoo ones trimmed, except `chart_5d_index.json`, which is complete) that
  the unit tests (`test-nasdaq.c`, `test-indices.c`) read as offline inputs. Nothing reads
  them at run time and `meson install` does not install them: they ship in the
  source tarball and the source RPM, not in the binary RPM.
- Stocks is not affiliated with, endorsed by or sponsored by Fidelity,
  Nasdaq, Yahoo or Apple. All company, fund, index and product names that
  appear in the app, in the data files or in this repository (for example
  Fidelity, Nasdaq, Yahoo, Apple, S&P 500, Dow Jones and Russell 2000)
  belong to their respective owners and are used only to identify the
  instruments.

## Packaging notes (Fedora maintainers)

The package is meant to be picked up by Fedora, and the repository carries
what a reviewer needs:

- Spec file: `packaging/rpm/stocks.spec` (name `stocks`, version 0.2.2,
  `License: GPL-3.0-or-later AND CC0-1.0`, `URL` https://github.com/wtp128pro/stocks).
- Source: `Source0` is `%{url}/archive/v%{version}/%{name}-%{version}.tar.gz`,
  i.e. GitHub's archive of the tag `v0.2.2`, named `stocks-0.2.2.tar.gz`, whose
  top-level directory is `stocks-0.2.2/` (the spec uses `%autosetup`).
- Build with Meson (`%meson`, `%meson_build`, `%meson_install`); no
  patches, no bundled libraries, no Internet access at build or test time.
  `BuildRequires` (all resolve with `rpm -q --whatprovides`): `gcc`, `meson`,
  `glib2-devel` (`glib-compile-resources`), `pkgconfig(gtk4)` >= 4.14,
  `pkgconfig(libsoup-3.0)`, `pkgconfig(json-glib-1.0)` >= 1.10.0,
  `pkgconfig(glib-2.0)` and `pkgconfig(gio-2.0)` >= 2.68, and for `%check`
  `desktop-file-utils`, `libappstream-glib`, `appstream` and
  `xorg-x11-server-Xvfb`. `Requires: hicolor-icon-theme` (owner of the icon
  directories); library requirements are generated from the binary.
- `%files` lists exact paths: the binary, the desktop entry, the metainfo
  file, the two icons, `%license COPYING` and `%doc README.md`.

Build the source and binary RPMs from a checkout (the tarball must contain the
tag's tree, so commit and tag first). The topdir is outside the checkout and
not under `/tmp`, and the whole build, `%check` included, runs in a private
user, mount and network namespace (explained below):

```sh
top=/var/tmp/stocks-rpmbuild
mkdir -p "$top/SOURCES"
git archive --prefix=stocks-0.2.2/ -o "$top/SOURCES/stocks-0.2.2.tar.gz" HEAD
unshare -rmn sh -c 'ip link set lo up && mount -t tmpfs tmpfs /tmp && exec rpmbuild -ba --define "_topdir $1" packaging/rpm/stocks.spec' sh "$top"
rpmlint packaging/rpm/stocks.spec "$top"/RPMS/*/stocks-*.rpm "$top"/SRPMS/stocks-*.src.rpm
```

`make rpm` (see "Makefile") does the same, with its own checks, except that it
takes the spec file and the version from `HEAD` (not the working tree) and does
**not** run `rpmlint`: run the last command above on the paths it prints. As in
this recipe, the topdir must be a plain path (letters, digits and
`. _ / + @ , = -` only): `rpmbuild` expands it as an RPM macro value, and a
topdir with whitespace, quotes, `%` or `$` makes it write outside the topdir.

`rpmbuild/`, `*.rpm` and `*.tar.gz` are in `.gitignore`. `%check` runs, in
this order, `desktop-file-validate` on the installed desktop entry,
`appstream-util validate-relax --nonet` on the installed metainfo file, and
`GTK_IM_MODULE=simple GTK_A11Y=none xvfb-run -a %{shrink:%meson_test}` (`%shrink`
because `%meson_test` expands with a leading line break), i.e. the whole
test suite of the "Test" subsection (13 tests, the validators included). Two of the tests
(`chart-widget` and `picker`) open GTK windows, which needs an X display and
therefore `xorg-x11-server-Xvfb`; they skip themselves (exit 77) when none is
available, so without Xvfb the build would silently test less.
`GTK_IM_MODULE=simple` keeps the test processes from connecting to an
input-method daemon.

`xvfb-run` creates its X socket in `/tmp/.X11-unix`, a lock file
`/tmp/.X<n>-lock` and an abstract socket, the same places a desktop session on
that machine uses. A plain mount namespace (`unshare -rm rpmbuild ...`) does
**not** isolate them: it shares the host's `/tmp` until something is mounted
over it, and abstract sockets belong to the network namespace. The recipe above
therefore uses `unshare -rmn`: `-n` gives the build its own network namespace
(its abstract sockets are private) and `ip link set lo up` brings up that
namespace's loopback, which the build needs (the `nasdaq` and `indices` tests
talk to a fake server on `127.0.0.1`, so `-n` without `lo` up fails those two
tests); `mount -t tmpfs tmpfs /tmp` hides the host's `/tmp/.X11-unix` and
lock files. This needs unprivileged user and network namespaces, and a checkout
and a topdir that are **not** under `/tmp` (the tmpfs hides them; use
`/var/tmp` or your home directory). Mock and Koji build in their own chroot,
where none of this is needed.

The binary in an RPM built this way (and in the published v0.1.0 RPM, built
from the same spec) comes from Fedora's `%meson` as `buildtype=plain` with
Fedora's `-O2` hardening flags. It is stripped, and it carries Fedora's
standard `.gnu_debugdata` mini-symbol table. `rpmbuild` also produces
`-debuginfo` and `-debugsource` packages with the full debug information;
they are **not** among the published v0.1.0 release assets. The binary of
`make install` is a different build, a plain `--buildtype=release` one
stripped completely (see "Makefile").

### Signing and publishing (maintainer)

Releases are signed by `tools/sign-release.sh IN_DIR OUT_DIR` with a dedicated
RSA 4096 key (not the maintainer's personal key), kept in its own `GNUPGHOME`
and protected by a passphrase. The script, given the unsigned `*.rpm` and
`*.tar.gz` files:

1. signs every RPM with `rpmsign --addsign` (the payload is untouched; only the
   header signature is added) and checks each one with `rpm -K` against a
   scratch rpmdb that trusts only that key, since a bare `rpm -K` passes an
   unsigned package;
2. writes `SHA256SUMS` and detached ASCII-armored signatures for it and for the
   tarball, and verifies them with `gpg --verify`;
3. builds the dnf repository tree (`fedora/<N>/<arch>/`, `fedora/<N>/SRPMS/`)
   with `createrepo_c` and signs each `repodata/repomd.xml`
   (`repo_gpgcheck=1` in `stocks.repo`);
4. refuses to run if `packaging/rpm/RPM-GPG-KEY-stocks` is not the exported
   public key of `SIGN_KEY`.

`OUT_DIR/release/` holds the GitHub release assets and `OUT_DIR/site/` is the
content of the `gh-pages` branch (GitHub Pages, https://wtp128pro.github.io/stocks/).
Only the public key is ever published. Replacing or rotating the key means a
new key, a new `packaging/rpm/RPM-GPG-KEY-stocks`, a re-signed repository and a
release note; the revocation certificate generated with the key must be kept
offline. The key expires on 2029-10-09 and must be extended or replaced
before then.
