#!/usr/bin/env bash
# perf_check.sh - efficiency check of the built `stocks` binary.
#
# Usage: tools/perf_check.sh [path/to/stocks]      (default: build/src/ui/stocks)
#
# Measures, under a private Xvfb display, with a seeded cache + watchlist and
# isolated XDG directories:
#   1. time from launch to the first mapped window  (X11 MapNotify on the root)
#   2. steady-state RSS (VmRSS in /proc/<pid>/status) after SETTLE_S seconds
#   3. idle CPU over IDLE_S seconds ((utime+stime delta) / wall time) with the
#      window visible AND with the window hidden
# and exits 1 if a measurement exceeds its budget, 2 if the harness itself
# could not run or its premises do not hold (no Xvfb/python3, no private
# namespaces, the app died or logged a warning, the app made a request, ...).
#
# "Hidden" is driven without a window manager (none runs under Xvfb): the
# script sets _NET_WM_STATE_HIDDEN on the app's toplevel, which is exactly what
# a window manager does when it minimises a window; GDK turns it into
# GDK_TOPLEVEL_STATE_MINIMIZED and the app reports "not visible".
#
# The visible and the hidden instance run at the same time (two separate
# processes with separate XDG directories) so that the whole check takes about
# SETTLE_S + IDLE_S + a few seconds. The time-to-window run happens first,
# alone.
#
# Isolation (everything below happens inside ONE unprivileged user namespace
# with its own mount, network and PID namespaces: `unshare -rmnpf --mount-proc
# --kill-child`):
#   * mount: a private tmpfs is mounted over /tmp, so Xvfb's socket directory
#     /tmp/.X11-unix and its /tmp/.X<n>-lock files, and the scratch directory,
#     are the script's own. The host's X sockets and locks (the live desktop's
#     display!) are never opened, unlinked or rebound. If a private mount
#     namespace cannot be created the script refuses to run (exit 2).
#   * network: only a down loopback device. GLib's default network monitor is
#     NetworkManager's, reached through the host's system D-Bus socket (a
#     file, not tied to the network namespace) and would report "online", so
#     GIO_USE_NETWORK_MONITOR=netlink is set: it reads the namespace's own
#     (empty) network state and reports "unavailable". The app then issues no
#     request at all. This is checked, not assumed: with G_MESSAGES_DEBUG=stocks
#     the app logs every request, and the run fails (exit 2) if any "request #"
#     line shows up in the log of either instance.
#   * pid: the kernel kills every process of the namespace when its init (pid 1)
#     dies. The outer part of this script keeps running as the parent of
#     `unshare`, traps SIGINT/SIGTERM, SIGKILLs unshare (--kill-child then
#     SIGKILLs pid 1) and waits until pid 1 is gone; `setpriv --pdeathsig`
#     does the same if the outer part itself is SIGKILLed. So no Xvfb, app,
#     timeout or bash survives SIGINT, SIGTERM, SIGKILL or an outer `timeout`.
# The live Nasdaq API is therefore never contacted. The market-open case (a
# refresh every 60 s while visible) is NOT measured; see README.md.
#
# Both the script and the binary may live under /tmp, which the private tmpfs
# hides: the script is handed to the namespace as an already open descriptor
# (never re-opened by path), the binary's top-level /tmp entry is bind-mounted
# from a descriptor opened before the tmpfs covers /tmp.
#
# Parameters (SETTLE_S, IDLE_S, MAP_TIMEOUT_S, BUDGET_*) are validated first, in
# every stage; anything but a plain decimal number exits 2 before any other
# action. The inner stages refuse to run unless they are where only this script
# puts them (see "Stages" below): a stray PERF_STAGE in the environment can
# never make the caller's own /tmp get a tmpfs mounted over it.
#
# Every process started inside runs under `timeout -k`; a cleanup trap stops
# them on normal exit, and the namespace teardown covers every other way out.

# ---------------------------------------------------------------------------
# Budgets (override from the environment for experiments; an empty value is an
# error, not "use the default"). Set to about twice the worst of runs 1-10 in
# README.md "Measured values and budgets" (default renderer, Xvfb); the
# "measured max" figures below are the max row of that table (runs 1-13).
# ---------------------------------------------------------------------------
: "${BUDGET_FIRST_WINDOW_MS=600}"    # launch -> first mapped window, ms   (measured max 295)
: "${BUDGET_RSS_MIB=640}"            # steady-state VmRSS, MiB             (measured max 311.2)
: "${BUDGET_CPU_VISIBLE_PCT=0.4}"    # idle CPU, window visible, % of core (measured max 0.233)
: "${BUDGET_CPU_HIDDEN_PCT=0.4}"     # idle CPU, window hidden,  % of core (measured max 0.183)

# Measurement parameters.
: "${SETTLE_S=10}"        # seconds after the window appeared before measuring
: "${IDLE_S=60}"          # idle CPU window, seconds
: "${MAP_TIMEOUT_S=30}"   # give up waiting for a window after this long

set -u

# ---------------------------------------------------------------------------
# Parameter validation: FIRST, before anything else, in every stage. No shell
# arithmetic is ever done on a value that has not passed this.
# ---------------------------------------------------------------------------
is_uint() { case $1 in ''|*[!0-9]*) return 1 ;; esac; [ "${#1}" -le 6 ]; }
is_dec()  { case $1 in ''|*[!0-9.]*|.*|*.|*.*.*) return 1 ;; esac; [ "${#1}" -le 12 ]; }
for v in SETTLE_S IDLE_S MAP_TIMEOUT_S; do
  is_uint "${!v}" || { echo "perf_check: $v must be a non-negative decimal integer of at most 6 digits, got '${!v}'" >&2; exit 2; }
  declare "$v=$((10#${!v}))"        # base 10: "08" is 8, not an octal error
done
for v in IDLE_S MAP_TIMEOUT_S; do
  [ "${!v}" -ge 1 ] || { echo "perf_check: $v must be at least 1" >&2; exit 2; }
done
for v in BUDGET_FIRST_WINDOW_MS BUDGET_RSS_MIB BUDGET_CPU_VISIBLE_PCT BUDGET_CPU_HIDDEN_PCT; do
  is_dec "${!v}" || { echo "perf_check: $v must be a non-negative decimal number (digits and at most one '.'), got '${!v}'" >&2; exit 2; }
done

# ===========================================================================
# Stages. Stage 0 is the only one a caller can start: PERF_STAGE is not set.
#   Stage 0 (outer): check the tools, start the private namespaces, forward signals.
#   Stage 1 (pid 1 of the namespaces): private /tmp, then run stage 2 under timeout.
#   Stage 2: the measurement.
# PERF_STAGE is set only in the environment of the command that starts the
# next stage, never in this shell. Stage 1 additionally refuses unless it is
# pid 1 (of a pid namespace stage 0 just created), stage 2 unless stage 1 left
# its marker in the private /tmp, so no caller can reach the mount or the
# measurement code in its own process or on its own /tmp.
# ===========================================================================
if [ -z "${PERF_STAGE:-}" ]; then
  BIN=${1:-build/src/ui/stocks}
  if [ ! -x "$BIN" ]; then
    echo "perf_check: '$BIN' is not an executable file (build first: meson compile -C build)" >&2
    exit 2
  fi
  BIN=$(readlink -f "$BIN")
  SELF=$(readlink -f "${BASH_SOURCE[0]}")
  for tool in Xvfb python3 timeout mktemp pgrep awk unshare setpriv mount seq; do
    command -v "$tool" >/dev/null 2>&1 || { echo "perf_check: '$tool' not found" >&2; exit 2; }
  done
  python3 -c 'import ctypes.util, sys; sys.exit(0 if ctypes.util.find_library("X11") else 1)' ||
    { echo "perf_check: libX11 not found for python3 ctypes" >&2; exit 2; }
  # The script is opened once, here, and handed down as descriptor 8: its path
  # may be hidden by the private /tmp.
  exec 8<"$SELF" || { echo "perf_check: cannot open '$SELF'" >&2; exit 2; }
  # Refuse to run without private mount + network + pid namespaces: Xvfb would
  # otherwise share /tmp/.X11-unix with the host's display server.
  if ! timeout -k 5 10 unshare -rmnpf --mount-proc --kill-child \
       sh -c 'mount -t tmpfs tmpfs /tmp' >/dev/null 2>&1; then
    echo "perf_check: cannot create private mount/network/pid namespaces (unshare -rmnpf --mount-proc);" >&2
    echo "perf_check: refusing to run, Xvfb would touch the host's /tmp/.X11-unix" >&2
    exit 2
  fi
  # unshare blocks SIGTERM/SIGINT while it waits for its child (a plain
  # `kill -TERM` of it would be ignored), so it runs as a background job of
  # this bash, which traps both and SIGKILLs unshare; --kill-child then makes
  # the kernel SIGKILL the namespace's pid 1, and with it every process in it.
  # `setpriv --pdeathsig` covers the death of this bash itself (SIGKILL).
  PERF_STAGE=1 PERF_BIN="$BIN" \
    setpriv --pdeathsig KILL unshare -rmnpf --mount-proc --kill-child bash /proc/self/fd/8 &
  ns=$!
  stop_ns() {
    local init i
    init=$(pgrep -P "$ns" | head -n1)
    kill -KILL "$ns" 2>/dev/null
    # pid 1 of the namespace stays (as a zombie) until every process in it is gone.
    for i in $(seq 1 50); do
      { [ -n "$init" ] && kill -0 "$init" 2>/dev/null; } || break
      sleep 0.1
    done
    wait "$ns" 2>/dev/null
  }
  trap 'stop_ns; exit 143' TERM
  trap 'stop_ns; exit 130' INT
  wait "$ns"
  exit $?
fi

if [ "$PERF_STAGE" = 1 ]; then
  if [ "$$" != 1 ] || [ -z "${PERF_BIN:-}" ]; then
    echo "perf_check: PERF_STAGE is internal (stage 1 only runs as pid 1 of its own pid namespace); unset it" >&2
    exit 2
  fi
  # A private /tmp. If the binary lives under /tmp (e.g. a scratch copy) its
  # top-level entry is carried over with a bind mount made from a descriptor
  # opened before the tmpfs hides it.
  hold=
  case $PERF_BIN in
    /tmp/*) rest=${PERF_BIN#/tmp/}; hold=/tmp/${rest%%/*}; exec 9<"$hold" || exit 2 ;;
  esac
  mount -t tmpfs -o mode=1777 tmpfs /tmp || { echo "perf_check: cannot mount a private /tmp" >&2; exit 2; }
  : >/tmp/.perf_check_stage1 || exit 2
  if [ -n "$hold" ]; then
    if [ -d /proc/self/fd/9 ]; then mkdir "$hold"; else : >"$hold"; fi
    mount -c --bind /proc/self/fd/9 "$hold" || { echo "perf_check: cannot expose $hold" >&2; exit 2; }
    exec 9<&-
  fi
  PERF_TOTAL_S=$((SETTLE_S + IDLE_S + 2 * MAP_TIMEOUT_S + 20))
  # Not exec'd: GNU timeout fails (exit 125) when it is itself pid 1. When this
  # bash (pid 1) exits, for whatever reason, the kernel kills the whole
  # namespace. The script comes from descriptor 8, not from its path.
  PERF_STAGE=2 TMPDIR=/tmp timeout -k 10 "$PERF_TOTAL_S" bash /proc/self/fd/8
  exit $?
fi

# Not stage 0, not stage 1: this must be stage 2, started by stage 1.
if [ "$PERF_STAGE" != 2 ] || [ ! -e /tmp/.perf_check_stage1 ]; then
  echo "perf_check: PERF_STAGE='$PERF_STAGE' is internal; unset it" >&2
  exit 2
fi
exec 8<&-


# ===========================================================================
# Stage 2: the measurement (inside the private namespaces).
# ===========================================================================
# The invariant of the whole design: no X socket or lock of the host is visible.
for f in /tmp/.X11-unix /tmp/.X*-lock; do
  [ -e "$f" ] && { echo "perf_check: $f is visible: /tmp is not private; refusing to run" >&2; exit 2; }
done
BIN=$PERF_BIN
TMP=$(mktemp -d /tmp/stocks-perf.XXXXXX) || exit 2
CLK_TCK=$(getconf CLK_TCK)
PIDS=()   # every process started here: killed by cleanup

cleanup() {
  trap '' EXIT INT TERM
  local p i
  for p in "${PIDS[@]}"; do kill -TERM "$p" 2>/dev/null; done
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
    local alive=0
    for p in "${PIDS[@]}"; do kill -0 "$p" 2>/dev/null && alive=1; done
    [ "$alive" = 0 ] && break
    sleep 0.2
  done
  for p in "${PIDS[@]}"; do
    pkill -KILL -P "$p" 2>/dev/null
    kill -KILL "$p" 2>/dev/null
  done
  rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

die() { echo "perf_check: $*" >&2; exit 2; }

# ---------------------------------------------------------------------------
# Python helper for the two things the shell cannot do: wait for a window
# (event driven, no polling) and flag a window as minimised (plain Xlib).
# ---------------------------------------------------------------------------
cat >"$TMP/x11.py" <<'PYEOF'
import ctypes, ctypes.util, select, sys, time

x = ctypes.CDLL(ctypes.util.find_library("X11"))
x.XOpenDisplay.restype = ctypes.c_void_p
x.XOpenDisplay.argtypes = [ctypes.c_char_p]
x.XDefaultRootWindow.restype = ctypes.c_ulong
x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
x.XSelectInput.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_long]
x.XPending.argtypes = [ctypes.c_void_p]
x.XNextEvent.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
x.XConnectionNumber.argtypes = [ctypes.c_void_p]
x.XInternAtom.restype = ctypes.c_ulong
x.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
x.XChangeProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong,
                              ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
x.XCloseDisplay.argtypes = [ctypes.c_void_p]

SubstructureNotifyMask = 1 << 19
MapNotify = 19
XA_ATOM = 4
PropModeReplace = 0

dpy = x.XOpenDisplay(None)
if not dpy:
    sys.exit("cannot open display")
root = x.XDefaultRootWindow(dpy)
cmd = sys.argv[1]

if cmd == "wait-map":
    # Print "ready", then "mapped <time_ns> <window id>" at the first MapNotify
    # of a child of the root window ("timeout" and exit 1 after argv[2] s).
    deadline = time.monotonic() + float(sys.argv[2])
    x.XSelectInput(dpy, root, SubstructureNotifyMask)
    x.XSync(dpy, 0)
    print("ready", flush=True)
    fd = x.XConnectionNumber(dpy)
    ev = ctypes.create_string_buffer(192)  # sizeof(XEvent) = 192 on LP64
    while True:
        while x.XPending(dpy):
            x.XNextEvent(dpy, ev)
            now = time.time_ns()
            if ctypes.c_int.from_buffer(ev).value == MapNotify:
                win = ctypes.c_ulong.from_buffer(ev, 40).value  # XMapEvent.window
                print("mapped %d %d" % (now, win), flush=True)
                sys.exit(0)
        left = deadline - time.monotonic()
        if left <= 0:
            print("timeout", flush=True)
            sys.exit(1)
        select.select([fd], [], [], left)
elif cmd == "hide":
    win = int(sys.argv[2])
    prop = x.XInternAtom(dpy, b"_NET_WM_STATE", 0)
    hidden = ctypes.c_ulong(x.XInternAtom(dpy, b"_NET_WM_STATE_HIDDEN", 0))
    x.XChangeProperty(dpy, win, prop, XA_ATOM, 32, PropModeReplace,
                      ctypes.byref(hidden), 1)
    x.XSync(dpy, 0)
else:
    sys.exit("unknown command")
x.XCloseDisplay(dpy)
PYEOF

# ---------------------------------------------------------------------------
# Seed: watchlist.json and cache.json in the documented formats of
# src/storage (stk-watchlist.h, stk-cache.h), so the first window paints from
# the cache. Quotes are fetched "now", prices are deterministic.
# ---------------------------------------------------------------------------
cat >"$TMP/seed.py" <<'PYEOF'
import json, math, os, sys, time

config_dir, cache_dir = sys.argv[1], sys.argv[2]
now = int(time.time() * 1000)
syms = [("AAPL", "Apple Inc.", "stock"), ("MSFT", "Microsoft Corporation", "stock"),
        ("NVDA", "NVIDIA Corporation", "stock"), ("SPY", "SPDR S&P 500 ETF Trust", "etf"),
        ("QQQ", "Invesco QQQ Trust", "etf"), ("VTI", "Vanguard Total Stock Market ETF", "etf"),
        ("FBND", "Fidelity Total Bond ETF", "etf"), ("FDVV", "Fidelity High Dividend ETF", "etf"),
        ("FXAIX", "Fidelity 500 Index Fund", "fund"), ("FSKAX", "Fidelity Total Market Index Fund", "fund"),
        ("FZROX", "Fidelity ZERO Total Market Index Fund", "fund"), ("AMZN", "Amazon.com, Inc.", "stock")]

def series(base, n, step_ms, end_ms, phase):
    t = [end_ms - (n - 1 - i) * step_ms for i in range(n)]
    v = [round(base * (1 + 0.02 * math.sin(i / 5.0 + phase) + 0.0005 * i), 4) for i in range(n)]
    return {"fetched_at_ms": now, "prev_close": round(base, 4), "t": t, "v": v}

entries = []
for i, (sym, name, kind) in enumerate(syms):
    base = 20.0 + 37.0 * (i + 1)
    price = round(base * 1.01, 2)
    e = {"symbol": sym,
         "quote": {"fetched_at_ms": now, "name": name, "kind": kind, "price": price,
                   "change": round(price - base, 2), "change_pct": round((price - base) / base * 100, 2),
                   "as_of_ms": now - 3600000, "is_realtime": False, "market_open": False,
                   "currency": "USD"},
         "series": {"1M": series(base, 21, 86400000, now - 3600000, i)}}
    if kind != "fund":
        e["series"]["1D"] = series(base, 390, 60000, now - 3600000, i)
    entries.append(e)

os.makedirs(os.path.join(config_dir, "stocks"), exist_ok=True)
os.makedirs(os.path.join(cache_dir, "stocks"), exist_ok=True)
with open(os.path.join(config_dir, "stocks", "watchlist.json"), "w") as f:
    json.dump({"version": 1, "entries": [{"symbol": s, "name": n, "kind": k} for s, n, k in syms]}, f)
with open(os.path.join(cache_dir, "stocks", "cache.json"), "w") as f:
    json.dump({"version": 1, "entries": entries}, f)
PYEOF

# ---------------------------------------------------------------------------
# Display
# ---------------------------------------------------------------------------
export GDK_BACKEND=x11 GSETTINGS_BACKEND=memory NO_AT_BRIDGE=1
# Offline for real: the namespace-local netlink monitor (see the header), and
# the app's debug log (every request is logged) to verify it.
export GIO_USE_NETWORK_MONITOR=netlink G_MESSAGES_DEBUG=stocks
export GSK_DEBUG="${GSK_DEBUG:+$GSK_DEBUG,}renderer"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$TMP/no-such-bus"
unset WAYLAND_DISPLAY XDG_SESSION_TYPE

timeout -k 10 $((SETTLE_S + IDLE_S + 3 * MAP_TIMEOUT_S + 60)) \
  Xvfb -screen 0 1280x800x24 -nolisten tcp -displayfd 3 3>"$TMP/display" 2>"$TMP/xvfb.log" &
PIDS+=($!)
for i in $(seq 1 100); do
  [ -s "$TMP/display" ] && break
  kill -0 "${PIDS[0]}" 2>/dev/null || die "Xvfb exited: $(cat "$TMP/xvfb.log")"
  sleep 0.1
done
[ -s "$TMP/display" ] || die "Xvfb did not report a display number"
export DISPLAY=":$(head -n1 "$TMP/display")"

# ---------------------------------------------------------------------------
# Instances
# ---------------------------------------------------------------------------
# launch NAME: seeds NAME's directories, starts the app and waits for its
# window. Sets T_<NAME> (timeout wrapper pid), P_<NAME> (app pid),
# W_<NAME> (window id) and MS_<NAME> (launch -> mapped, ms).
launch() {
  local name=$1 base="$TMP/$1" hp line t0 tm i
  mkdir -p "$base/config" "$base/cache" "$base/data"
  python3 "$TMP/seed.py" "$base/config" "$base/cache" || die "seeding failed"
  mkfifo "$base/ev"
  timeout -k 5 $((MAP_TIMEOUT_S + 5)) python3 "$TMP/x11.py" wait-map "$MAP_TIMEOUT_S" >"$base/ev" &
  hp=$!; PIDS+=("$hp")
  exec 7<"$base/ev"
  read -r -t "$MAP_TIMEOUT_S" -u 7 line && [ "$line" = ready ] || die "X helper did not start"
  t0=$(date +%s%N)
  XDG_CONFIG_HOME="$base/config" XDG_CACHE_HOME="$base/cache" XDG_DATA_HOME="$base/data" \
    timeout -k 10 $((SETTLE_S + IDLE_S + 2 * MAP_TIMEOUT_S + 30)) "$BIN" >"$base/app.log" 2>&1 &
  eval "T_$name=$!"; PIDS+=($!)
  read -r -t "$MAP_TIMEOUT_S" -u 7 line || line=timeout
  exec 7<&-
  wait "$hp" 2>/dev/null
  case $line in
    mapped*) set -- $line; tm=$2; eval "W_$name=$3"
             eval "MS_$name=$(awk -v a="$t0" -v b="$tm" 'BEGIN{printf "%.0f", (b-a)/1e6}')" ;;
    *) die "no window within ${MAP_TIMEOUT_S}s for instance $name; app log: $(cat "$base/app.log")" ;;
  esac
  local tvar="T_$name"
  for i in $(seq 1 50); do
    eval "P_$name=\$(pgrep -P \"\${$tvar}\" | head -n1)"
    eval "[ -n \"\${P_$name}\" ]" && break
    sleep 0.02
  done
  eval "[ -n \"\${P_$name}\" ]" || die "cannot find the pid of instance $name"
  PIDS+=("$(eval echo "\$P_$name")")
}

cpu_ticks() { awk '{ s = $0; sub(/^.*\) /, "", s); split(s, f, " "); print f[12] + f[13] }' "/proc/$1/stat" 2>/dev/null; }
rss_kib()   { awk '/^VmRSS:/ { print $2 }' "/proc/$1/status" 2>/dev/null; }
# num NAME VALUE: VALUE must be an unsigned integer (a /proc read that failed gives "").
num()       { case $2 in ''|*[!0-9]*) die "could not read $1 from /proc (instance died?)" ;; esac; }

launch V                      # visible instance: time-to-window is measured on this one
launch H                      # instance that is minimised
timeout -k 5 20 python3 "$TMP/x11.py" hide "$W_H" || die "could not hide instance H"

sleep "$SETTLE_S"
for n in V H; do
  eval "kill -0 \"\$P_$n\"" 2>/dev/null || die "instance $n died; log: $(cat "$TMP/$n/app.log")"
done

RSS_V=$(rss_kib "$P_V"); RSS_H=$(rss_kib "$P_H")
W0=$(date +%s%N); CV0=$(cpu_ticks "$P_V"); CH0=$(cpu_ticks "$P_H")
sleep "$IDLE_S"
W1=$(date +%s%N); CV1=$(cpu_ticks "$P_V"); CH1=$(cpu_ticks "$P_H")
num "VmRSS of V" "$RSS_V"; num "VmRSS of H" "$RSS_H"
num "CPU ticks of V (start)" "$CV0"; num "CPU ticks of H (start)" "$CH0"
num "CPU ticks of V (end)" "$CV1"; num "CPU ticks of H (end)" "$CH1"

for n in V H; do
  eval "kill -0 \"\$P_$n\"" 2>/dev/null || die "instance $n died during measurement"
  # The app's own g_warning()/g_critical() (log domain "stocks") and any
  # GLib/GTK critical fail the run; Mesa's DRI3 notes and GTK's "no session
  # bus" warning (the bus is disabled on purpose) are expected under Xvfb.
  if grep -E 'stocks-(WARNING|CRITICAL|ERROR)|-CRITICAL \*\*' "$TMP/$n/app.log" | grep -qv 'session bus'; then
    die "instance $n logged problems: $(cat "$TMP/$n/app.log")"
  fi
  # The offline premise is verified, not assumed: the debug log must be on
  # (the app logs "window visible=N" at least once) and no request may have
  # been issued, neither before nor during the idle window.
  grep -q 'window visible=' "$TMP/$n/app.log" || die "instance $n: no debug log (G_MESSAGES_DEBUG=stocks ineffective), cannot verify the offline premise"
  if grep -q 'request #' "$TMP/$n/app.log"; then
    die "instance $n issued requests although the network is meant to be unavailable: $(grep -m10 'request #' "$TMP/$n/app.log")"
  fi
  # The app's own last word on its visibility must match the instance's role.
  want=1; [ "$n" = H ] && want=0
  [ "$(grep -o 'window visible=[01]' "$TMP/$n/app.log" | tail -n1)" = "window visible=$want" ] ||
    die "instance $n: the app's last reported visibility is not $want"
done

pct() { awk -v d="$(($2 - $1))" -v t="$CLK_TCK" -v w0="$W0" -v w1="$W1" \
          'BEGIN { printf "%.3f", d / t / ((w1 - w0) / 1e9) * 100 }'; }
CPU_V=$(pct "$CV0" "$CV1"); CPU_H=$(pct "$CH0" "$CH1")
RSS_V_MIB=$(awk -v k="$RSS_V" 'BEGIN { printf "%.1f", k / 1024 }')
RSS_H_MIB=$(awk -v k="$RSS_H" 'BEGIN { printf "%.1f", k / 1024 }')

FAIL=0
# row LABEL VALUE UNIT BUDGET
row() {
  local verdict=ok
  awk -v v="$2" -v b="$4" 'BEGIN { exit !(v + 0 > b + 0) }' && { verdict=FAIL; FAIL=1; }
  printf '%-44s %10s %-4s %10s  %s\n' "$1" "$2" "$3" "$4" "$verdict"
}

RENDERER=$(sed -n "s/.*Using renderer '\([^']*\)'.*/\1/p" "$TMP/V/app.log" | head -n1)
echo "perf_check: $BIN"
echo "network: offline, verified (netlink monitor, 0 requests logged)   display: $DISPLAY (Xvfb, private /tmp)"
echo "renderer: ${RENDERER:-unknown} (GSK_RENDERER=${GSK_RENDERER:-unset})   settle ${SETTLE_S}s   idle window $(awk -v a="$W0" -v b="$W1" 'BEGIN { printf "%.1f", (b - a) / 1e9 }')s"
printf '%-44s %10s %-4s %10s  %s\n' measurement value unit budget result
row "launch -> first mapped window"             "$MS_V"   ms   "$BUDGET_FIRST_WINDOW_MS"
row "steady-state RSS (window visible)"         "$RSS_V_MIB" MiB "$BUDGET_RSS_MIB"
row "idle CPU, window visible"                  "$CPU_V"  %    "$BUDGET_CPU_VISIBLE_PCT"
row "idle CPU, window hidden (minimised)"       "$CPU_H"  %    "$BUDGET_CPU_HIDDEN_PCT"
printf '%-44s %10s %-4s %10s  %s\n' "info: RSS of the hidden instance" "$RSS_H_MIB" MiB - -
if [ "$FAIL" = 0 ]; then echo "perf_check: PASS"; else echo "perf_check: FAIL (budget exceeded)"; fi
exit "$FAIL"
