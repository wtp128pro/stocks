# Stocks

A small GTK4 desktop watchlist for stocks, ETFs and Fidelity mutual funds,
written in C with Meson. It shows a list of the instruments you follow (price,
change, a one-month sparkline) and, for the selected one, a price chart for
1D / 1M / 3M / 6M / 1Y. It is built to idle cheaply: no periodic timer runs
unless the US market is open, the window is visible and the network is up.

It does not trade, log in to a broker, sync a Fidelity account, show news,
send alerts or compute portfolio P&L, and it never will as part of this
project.

## Features

- Watchlist of stocks, ETFs and mutual funds, with per-row menu (Move up,
  Move down, Remove) and drag-and-drop reordering. Stored locally.
- Quote and 1M sparkline per row; chart of the selected row for 1D, 1M, 3M,
  6M and 1Y.
- "Add" window: browse the built-in Fidelity ETF / mutual-fund catalog (filter
  by text, kind and category) or search symbols online.
- Cache: the latest quote and series of every symbol are kept on disk, so the
  first window paints from the cache without waiting for the network, and
  the app still shows data when offline. A failed refresh keeps the cached
  data and dims it (stale marker; the detail pane says "Stale" or "Offline").
- Application id `org.stocks.Stocks`.

## Data source (read this)

All market data comes from the **unofficial Nasdaq web API**
(`https://api.nasdaq.com`): read-only HTTPS GETs, no account and no API key.
This is not a documented or supported public API. It is meant for personal,
low-volume use only; check Nasdaq's terms before using it for anything else.
It can change, throttle you or stop working at any time without notice, and
then quotes will simply fail to refresh (the cache and the stale marker keep
the window usable). Quotes are not guaranteed to be real-time. The detail pane
shows the date the quote refers to (New York calendar date, "As of Oct 08,
2026", or "NAV · Oct 08, 2026" for funds); the list rows show no such text.

All network access goes through one interface, `StkProvider`
(`src/provider/stk-provider.h`); the Nasdaq implementation is `StkNasdaq`
(`src/provider/stk-nasdaq.h`, which lists the exact endpoints, timeouts and
retry policy). A different data source means writing another implementation
of that interface; nothing else in the app talks to the network.

## Build, run, test

Dependencies (pkg-config names): `gtk4` >= 4.14, `libsoup-3.0`,
`json-glib-1.0` >= 1.10.0, `glib-2.0` >= 2.68, plus Meson >= 1.0, Ninja and a C11
compiler. The tools and tests below need `python3`; the GUI tests and
`tools/perf_check.sh` need `Xvfb` (package `xorg-x11-server-Xvfb` on Fedora);
`xvfb-run` is used by the test command below but not by the script.

```sh
meson setup build              # configure (debugoptimized, warning_level=3)
meson compile -C build         # builds build/src/ui/stocks
build/src/ui/stocks            # run
GTK_IM_MODULE=simple GTK_A11Y=none xvfb-run -a meson test -C build   # 8 test suites; the GUI tests need a display
```

Run the test gate as `GTK_IM_MODULE=simple GTK_A11Y=none xvfb-run -a meson test
-C build`: without `GTK_IM_MODULE=simple` the GTK test processes connect to
your real `ibus-daemon`, which makes typing bursts on your desktop. `meson
test` without `xvfb-run` works only if a display is available; the test suites
that open windows pin `GDK_BACKEND=x11`, so run them under `xvfb-run -a` to
keep them off your real session. The picker suite takes about 20 s.

## Refresh policy

The decisions live in `src/ui/stk-scheduler.h` (pure logic, unit-tested in
`src/ui/test-scheduler.c`):

- Requests are made only while the window is visible (mapped and not
  minimised) **and** the network is available.
- One refresh of everything (quotes, 1M sparklines, the selected chart) on
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
  per-request deadlines (10 s per attempt).
- Hiding the window or losing the network cancels requests in flight.
- A failed request keeps the cached data, marks the symbol stale, and is retried
  by the next refresh that includes it. The provider itself retries network
  errors, timeouts and HTTP 5xx twice with back-off (1 s, 2 s).
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
- After-hours and pre-market behaviour of the provider is inferred in part.
  The repository stores recorded provider responses only for after-hours
  quotes (`src/provider/fixtures/quote_stock.json`, `quote_etf.json`, and the
  1D chart `chart_1d_etf.json`) plus a fund quote, daily charts, a fund
  history, a search and an error body. The open, closed and pre-market
  layouts are exercised only by hand-written bodies in `test-nasdaq.c`.
  `stk-nasdaq.h` says that real responses for Open, After-Hours and Closed
  were seen and that Pre-Market is inferred; the Open and Closed ones are
  not stored in the repository.
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
