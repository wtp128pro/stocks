#!/usr/bin/env python3
"""Maintainer tool: rebuild data/fidelity_catalog.json.

Never run by the app or by meson; the app only reads the checked-in JSON that
is compiled into the binary as a GResource.

Sources (all fetched politely, one request at a time, with a clear
User-Agent):

  * ETFs/ETPs   https://www.fidelity.com/etfs/find-an-etf
  * Mutual funds https://institutional.fidelity.com/app/tabbed/avgannualreturns/...
  * Cross-check https://www.nasdaqtrader.com/dynamic/SymDir/{nasdaqlisted,otherlisted}.txt
  * Optional    https://api.nasdaq.com/api/quote/{SYM}/info?assetclass=...
                (--verify: every symbol must resolve; >= 2 s between calls)

Rules applied:

  * ETFs: every symbol on Fidelity's page that is an ETF on Nasdaq Trader.
    Nasdaq-Trader ETFs named "Fidelity ..." that the page omits are added too
    (category from NASDAQ_ONLY_CATEGORIES, else ""), except NASDAQ_ONLY_EXCLUDED.
  * Funds: every row of Fidelity's fund table that is not flagged
    "Closed to Public" / "Closed to new investors", plus MANUAL_FUNDS (funds
    that table does not list).
  * EXCLUDED_SYMBOLS are never emitted.

Usage:
  build_catalog.py                      fetch, build, print a diff (no write)
  build_catalog.py --write              ... and overwrite the JSON
  build_catalog.py --verify --write     also check each symbol on api.nasdaq.com
                                        (must resolve with a parseable price)
  build_catalog.py --offline-samples D  parse saved files from directory D
                                        (see SAMPLE_FILES) instead of fetching
  build_catalog.py --self-test          offline self-tests

Every download is capped at MAX_DOWNLOAD_BYTES (8 MiB). The only file ever
written is data/fidelity_catalog.json (--write).

Python standard library only.
"""

import argparse
import datetime
import difflib
import email.message
import contextlib
import functools
import html
import io
import os
import tempfile
import json
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from html.parser import HTMLParser
from pathlib import Path

USER_AGENT = "stocks-app-dev/0.1 (personal use; maintainer script build_catalog.py)"

URL_ETFS = "https://www.fidelity.com/etfs/find-an-etf"
URL_FUNDS = (
    "https://institutional.fidelity.com/app/tabbed/avgannualreturns/"
    "FIIS_PP_SP34_DPL6_AVG.html?tab=avgannualreturns&productLineId=6&navId=324"
)
URL_NASDAQ_LISTED = "https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt"
URL_OTHER_LISTED = "https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt"
URL_INFO = "https://api.nasdaq.com/api/quote/{symbol}/info?assetclass={assetclass}"

# File names expected inside the --offline-samples directory.
SAMPLE_FILES = {
    "etfs": "etfs.html",
    "funds": "funds.html",
    "nasdaq_listed": "nasdaqlisted.txt",
    "other_listed": "otherlisted.txt",
}

# Minimum delay between two requests to the same family of hosts.
REQUEST_GAP_S = 2.0
HTTP_TIMEOUT_S = 30
MAX_DOWNLOAD_BYTES = 8 * 1024 * 1024
VERIFY_RETRIES = 3

REPO_ROOT = Path(__file__).resolve().parent.parent
CATALOG_PATH = REPO_ROOT / "data" / "fidelity_catalog.json"

# Never emitted, whatever the sources say: unrelated tickers that look
# Fidelity-ish (FLCG Federated Hermes ETF; FIS, FNF, FDBC are stocks) and
# liquidated products (FSST, FDWM, FDHT, FSBD, FSLD).
EXCLUDED_SYMBOLS = frozenset(
    {"FLCG", "FIS", "FNF", "FDBC", "FSST", "FDWM", "FDHT", "FSBD", "FSLD"}
)

# Not added from Nasdaq Trader alone: FINA is accepted only if Fidelity's own
# ETF page lists it.
NASDAQ_ONLY_EXCLUDED = frozenset({"FINA"})

# Retail Fidelity funds that Fidelity's table does not list. Money-market
# funds (SPAXX, FDRXX) are deliberately absent: Nasdaq reports no price for them.
MANUAL_FUNDS = (
    ("FZROX", "Fidelity ZERO Total Market Index Fund", "Large Blend"),
    ("FZILX", "Fidelity ZERO International Index Fund", "Foreign Large Blend"),
    ("FNILX", "Fidelity ZERO Large Cap Index Fund", "Large Blend"),
    ("FZIPX", "Fidelity ZERO Extended Market Index Fund", "Mid-Cap Blend"),
)

# ETFs taken from Nasdaq Trader alone (not on Fidelity's page) get their
# category from this table, in the vocabulary of etf_category(); any other
# such ETF gets "" (unknown) rather than a guess.
NASDAQ_ONLY_CATEGORIES = {
    "FEMG": "Active Equity",
    "FEMV": "Active Equity",
    "FSEG": "Active Equity",
    "FSEV": "Active Equity",
}

KIND_ETF = "etf"
KIND_FUND = "fund"
ASSETCLASS = {KIND_ETF: "etf", KIND_FUND: "mutualfunds"}

# Same grammar as stk_symbol_is_valid() in src/core (the app re-checks it).
SYMBOL_RE = re.compile(r"^[A-Za-z0-9^][A-Za-z0-9.\-^=]{0,14}$")

MIN_ETFS = 50
MIN_FUNDS = 100
MIN_CATALOG_ETFS = 80  # final catalog, after --verify
MAX_DROPPED_FRACTION = 0.10

_last_request = [0.0]


# --------------------------------------------------------------------------
# Fetching
# --------------------------------------------------------------------------

def fetch(url, accept="*/*", min_gap=REQUEST_GAP_S):
    """GET url and return the decoded body (at most MAX_DOWNLOAD_BYTES; a
    larger response raises ValueError). Sleeps so that consecutive calls
    are at least min_gap seconds apart. Raises urllib.error.URLError / OSError."""
    wait = _last_request[0] + min_gap - time.monotonic()
    if wait > 0:
        time.sleep(wait)
    req = urllib.request.Request(
        url, headers={"User-Agent": USER_AGENT, "Accept": accept}
    )
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT_S) as resp:
            charset = resp.headers.get_content_charset() or "utf-8"
            body = resp.read(MAX_DOWNLOAD_BYTES + 1)
            if len(body) > MAX_DOWNLOAD_BYTES:
                raise ValueError(
                    "%s: response exceeds %d bytes; aborting" % (url, MAX_DOWNLOAD_BYTES)
                )
            return body.decode(charset, errors="replace")
    finally:
        _last_request[0] = time.monotonic()


def fetch_sources():
    """Fetch the four listing documents; returns dict keyed like SAMPLE_FILES."""
    urls = {
        "etfs": URL_ETFS,
        "funds": URL_FUNDS,
        "nasdaq_listed": URL_NASDAQ_LISTED,
        "other_listed": URL_OTHER_LISTED,
    }
    return {key: fetch(url) for key, url in urls.items()}


def load_samples(directory):
    """Read the saved documents named in SAMPLE_FILES from directory."""
    base = Path(directory)
    return {
        key: (base / name).read_text(encoding="utf-8", errors="replace")
        for key, name in SAMPLE_FILES.items()
    }


# --------------------------------------------------------------------------
# Parsing (pure functions: text in, data out)
# --------------------------------------------------------------------------

def clean_text(text):
    """Unescape entities, drop trademark marks, collapse whitespace."""
    text = html.unescape(text).replace("\u00a0", " ")
    text = text.replace("\u00ae", "").replace("\u2122", "")
    return re.sub(r"\s+", " ", text).strip()


class _EtfPageParser(HTMLParser):
    """Collects <h3 class="cp-build-filter ..."><a href="...summary?symbol=X">X</a>
    <br/>Name</h3> cards. The page nests an <h3> inside an <h3>, so this works on
    the token stream, not on a DOM. Text inside <sup> (footnote digits and the
    registered-trademark sign) is dropped."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.cards = []  # dicts: classes, symbol, name_parts, etp
        self._card = None
        self._sup = 0
        self._in_anchor = False
        self._in_font = False

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "h3" and "cp-build-filter" in (a.get("class") or "").split():
            self._card = {
                "classes": a["class"].split(),
                "symbol": None,
                "name_parts": [],
                "etp": False,
            }
            self.cards.append(self._card)
            self._sup = 0
            return
        if self._card is None:
            return
        if tag == "a":
            self._in_anchor = True
            if self._card["symbol"] is None:
                query = urllib.parse.urlparse(a.get("href") or "").query
                values = urllib.parse.parse_qs(query).get("symbol")
                if values:
                    self._card["symbol"] = values[0].strip().upper()
        elif tag == "sup":
            self._sup += 1
        elif tag == "font":
            self._in_font = True

    def handle_endtag(self, tag):
        if self._card is None:
            return
        if tag == "h3":
            self._card = None
        elif tag == "a":
            self._in_anchor = False
        elif tag == "sup" and self._sup:
            self._sup -= 1
        elif tag == "font":
            self._in_font = False

    def handle_data(self, data):
        if self._card is None or self._sup or self._in_anchor:
            return
        if self._in_font and data.strip().upper() == "ETP":
            self._card["etp"] = True
            return
        self._card["name_parts"].append(data)


_ETF_FLAG_ORDER = (
    ("Active", "Active"),
    ("Passive", "Passive"),
    ("Equity", "Equity"),
    ("Fixed_income", "Fixed Income"),
    ("Factor", "Factor"),
    ("Sector", "Sector"),
    ("Thematic", "Thematic"),
    ("Sustainable", "Sustainable"),
)


def etf_category(classes, etp):
    """Readable category from Fidelity's filter flags (cp-build-filter classes),
    e.g. ['cp-build-filter','Factor','Equity','Passive'] -> 'Passive Equity
    Factor'. Products marked ETP get 'ETP' appended."""
    flags = set(classes)
    words = [label for key, label in _ETF_FLAG_ORDER if key in flags]
    if etp:
        words.append("ETP")
    return " ".join(words)


def parse_etf_page(page):
    """Return [(symbol, name, category)] in page order from the find-an-etf
    page. Cards without a symbol or name are skipped."""
    parser = _EtfPageParser()
    parser.feed(page)
    parser.close()
    out = []
    for card in parser.cards:
        name = clean_text("".join(card["name_parts"]))
        if card["symbol"] and name:
            out.append((card["symbol"], name, etf_category(card["classes"], card["etp"])))
    return out


_ROW_RE = re.compile(r'<tr\b[^>]*\bid="tr\d+"[^>]*>.*?</tr>', re.S)
_ROW_SYMBOL_RE = re.compile(r'fund-symbol="([^"]+)"')
_ROW_NAME_RE = re.compile(
    r'<div\s+class="quick-view"[^>]*>\s*<a\b[^>]*>(.*?)</a>', re.S
)
_ROW_CAT_RE = re.compile(r'<td\b[^>]*\bid="tdMorningstarCat\d+"[^>]*>(.*?)</td>', re.S)
_ROW_CLOSED_RE = re.compile(
    r"\(\s*Closed\s+to\s+(?:the\s+)?(?:public|new\s+investors)\s*\)", re.I
)


def parse_fund_page(page):
    """Return [(symbol, name, category, closed)] from the institutional.fidelity.com
    table. category is Morningstar's ('' when Fidelity shows 'na'); closed is
    True when the row is flagged closed to the public / to new investors."""
    out = []
    for row in _ROW_RE.findall(page):
        sym = _ROW_SYMBOL_RE.search(row)
        name = _ROW_NAME_RE.search(row)
        if not sym or not name:
            continue
        cat = _ROW_CAT_RE.search(row)
        category = clean_text(re.sub(r"<[^>]+>", " ", cat.group(1))) if cat else ""
        if category.lower() == "na":
            category = ""
        out.append(
            (
                sym.group(1).strip().upper(),
                clean_text(re.sub(r"<[^>]+>", " ", name.group(1))),
                category,
                bool(_ROW_CLOSED_RE.search(row)),
            )
        )
    return out


def parse_nasdaq_trader(nasdaq_listed, other_listed):
    """Return {symbol: name} of live (non-test) ETFs from both Nasdaq Trader
    directory files. Lines that do not match a file's header layout (the
    trailing 'File Creation Time' line) are ignored."""
    etfs = {}
    for text, sym_col, name_col, etf_col, test_col in (
        (nasdaq_listed, "Symbol", "Security Name", "ETF", "Test Issue"),
        (other_listed, "NASDAQ Symbol", "Security Name", "ETF", "Test Issue"),
    ):
        lines = text.splitlines()
        if not lines:
            continue
        header = lines[0].split("|")
        try:
            idx = [header.index(c) for c in (sym_col, name_col, etf_col, test_col)]
        except ValueError:
            raise ValueError("unexpected Nasdaq Trader header: %r" % lines[0])
        for line in lines[1:]:
            cols = line.split("|")
            if len(cols) != len(header):
                continue
            sym, name, etf, test = (cols[i].strip() for i in idx)
            if etf == "Y" and test == "N" and sym:
                etfs[sym.upper()] = clean_text(name)
    return etfs


# --------------------------------------------------------------------------
# Building
# --------------------------------------------------------------------------

def build_items(etf_cards, fund_rows, nasdaq_etfs):
    """Merge parsed sources into a list of catalog items sorted by symbol.
    Returns (items, notes) where notes are human-readable decisions/warnings."""
    notes = []
    items = {}

    def add(symbol, name, kind, category):
        if not SYMBOL_RE.match(symbol):
            notes.append("skipped invalid symbol %r" % symbol)
        elif symbol in EXCLUDED_SYMBOLS:
            notes.append("excluded %s" % symbol)
        elif symbol in items:
            notes.append("duplicate %s ignored" % symbol)
        else:
            items[symbol] = {
                "symbol": symbol,
                "name": name,
                "kind": kind,
                "category": category,
            }

    page_symbols = set()
    for symbol, name, category in etf_cards:
        page_symbols.add(symbol)
        if symbol not in nasdaq_etfs:
            notes.append("dropped ETF %s: not a live ETF in Nasdaq Trader" % symbol)
            continue
        add(symbol, name, KIND_ETF, category)

    for symbol in sorted(nasdaq_etfs):
        name = nasdaq_etfs[symbol]
        if symbol in page_symbols or not name.startswith("Fidelity "):
            continue
        if symbol in NASDAQ_ONLY_EXCLUDED:
            notes.append("not adding %s: Nasdaq Trader only, not on Fidelity page" % symbol)
            continue
        notes.append("added %s from Nasdaq Trader (not on Fidelity page)" % symbol)
        category = NASDAQ_ONLY_CATEGORIES.get(symbol, "")
        if not category:
            notes.append("no category known for %s" % symbol)
        add(symbol, name, KIND_ETF, category)

    for symbol, name, category, closed in fund_rows:
        if closed:
            notes.append("skipped closed fund %s" % symbol)
            continue
        add(symbol, name, KIND_FUND, category)

    for symbol, name, category in MANUAL_FUNDS:
        add(symbol, name, KIND_FUND, category)

    return [items[s] for s in sorted(items)], notes


def render(items, generated):
    """Serialise the catalog, one item per line so diffs stay readable."""
    sources = [URL_ETFS, URL_FUNDS, URL_NASDAQ_LISTED, URL_OTHER_LISTED]
    lines = ["{", '  "version": 1,', '  "generated": %s,' % json.dumps(generated)]
    lines.append('  "sources": [')
    lines.append(",\n".join("    " + json.dumps(u) for u in sources))
    lines.append("  ],")
    lines.append('  "items": [')
    lines.append(
        ",\n".join(
            "    " + json.dumps(item, ensure_ascii=False, separators=(", ", ": "))
            for item in items
        )
    )
    lines.append("  ]")
    lines.append("}")
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------
# Verification against api.nasdaq.com
# --------------------------------------------------------------------------

# The price grammar of stk_parse_price() in src/core/stk-parse.c: trimmed text
# of at most 63 bytes that is "N/A" (any case) or an optional '$' and an
# unsigned number with strict ",ddd" grouping and an optional fraction.
_PRICE_RE = re.compile(
    r"(?:[nN]/[aA]|\$?(?:[0-9]+|[0-9]{1,3}(?:,[0-9]{3})+)(?:\.[0-9]+)?)"
)
_PRICE_WS = " \t\n\r\f\v"


def price_text_parses(text):
    """True if text is accepted by stk_parse_price() (a number or N/A)."""
    if not isinstance(text, str):
        return False
    text = text.strip(_PRICE_WS)
    return len(text.encode("utf-8")) <= 63 and _PRICE_RE.fullmatch(text) is not None


def info_is_quotable(data, symbol, assetclass):
    """True if the data object of a Nasdaq info answer is this symbol with
    this asset class and primaryData or secondaryData carries a lastSalePrice
    that the app's Nasdaq provider can parse (number or N/A). Symbols without
    one (e.g. money-market funds reporting "Average Maturity: 37 Days") would
    fail every quote with a parse error, so they are not catalog material."""
    if not isinstance(data, dict):
        return False
    if (str(data.get("symbol", "")).upper() != symbol
            or str(data.get("assetClass", "")).lower() != assetclass):
        return False
    for key in ("primaryData", "secondaryData"):
        block = data.get(key)
        if isinstance(block, dict) and price_text_parses(block.get("lastSalePrice")):
            return True
    return False


def verify_symbol(symbol, kind):
    """Ask the Nasdaq info endpoint about symbol with the assetclass of kind.

    Returns True if it resolves to that symbol and asset class and carries a
    parseable lastSalePrice (see info_is_quotable). Returns False if it does
    not, if the API says the symbol does not exist (rCode 400) or answers
    twice, 5 s apart, with no data (it reports unknown mutual-fund share
    classes as "Something went wrong"). Raises RuntimeError after
    VERIFY_RETRIES transport / HTTP / non-JSON failures."""
    assetclass = ASSETCLASS[kind]
    url = URL_INFO.format(
        symbol=urllib.parse.quote(symbol, safe=""), assetclass=assetclass
    )
    last = None
    empty_answers = 0
    attempt = 0
    while attempt < VERIFY_RETRIES:
        try:
            doc = json.loads(fetch(url, accept="application/json"))
        except (OSError, ValueError) as exc:
            last = exc
            attempt += 1
            time.sleep(10 * attempt)
            continue
        data = doc.get("data") if isinstance(doc, dict) else None
        if isinstance(data, dict):
            return info_is_quotable(data, symbol, assetclass)
        status = doc.get("status") if isinstance(doc, dict) else None
        if not isinstance(status, dict):
            last = "unexpected response shape"
            attempt += 1
            time.sleep(10 * attempt)
            continue
        if status.get("rCode") == 400:
            return False
        empty_answers += 1
        if empty_answers >= 2:
            return False
        time.sleep(5)
    raise RuntimeError("could not verify %s (%s): %s" % (symbol, assetclass, last))


def verify_items(items, log=sys.stderr):
    """Return (kept, dropped) after verifying every item, one request at a
    time. Progress goes to log."""
    kept, dropped = [], []
    for n, item in enumerate(items, 1):
        ok = verify_symbol(item["symbol"], item["kind"])
        print(
            "verify %d/%d %s %s %s"
            % (n, len(items), item["symbol"], item["kind"], "OK" if ok else "DROPPED"),
            file=log,
            flush=True,
        )
        (kept if ok else dropped).append(item)
    return kept, dropped


# --------------------------------------------------------------------------
# Output guard and self-test
# --------------------------------------------------------------------------

def write_catalog(path, text, target=CATALOG_PATH):
    """Atomically replace target (default: the repo's data/fidelity_catalog.json)
    with text. path must name target exactly (no resolving, no '..'). Refuses
    if target or its parent directory is a symlink or the parent is not an
    existing directory. The data goes to a temp file in the same directory and
    os.replace swaps it in, so nothing a link points at is ever written."""
    path = Path(os.path.abspath(path))
    target = Path(os.path.abspath(target))
    if path != target:
        raise ValueError("refusing to write %s: only %s may be written" % (path, target))
    parent = target.parent
    if os.path.islink(parent) or not os.path.isdir(parent):
        raise ValueError("refusing to write: %s is a symlink or not a directory" % parent)
    if os.path.islink(target):
        raise ValueError("refusing to write: %s is a symlink" % target)
    fd, tmp = tempfile.mkstemp(prefix=".catalog-", suffix=".tmp", dir=parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
        os.chmod(tmp, 0o644)
        if os.path.islink(parent) or os.path.islink(target):  # re-check at write time
            raise ValueError("refusing to write: %s changed into a symlink" % target)
        os.replace(tmp, target)
    except BaseException:
        if os.path.lexists(tmp):
            os.unlink(tmp)
        raise


def check_floors(items, dropped):
    """Raise ValueError unless the final item list is plausible: at least
    MIN_ETFS ETFs, MIN_FUNDS funds, MIN_CATALOG_ETFS ETFs (briefing), and at
    most MAX_DROPPED_FRACTION of the candidates dropped by --verify."""
    n_etf = sum(1 for i in items if i["kind"] == KIND_ETF)
    n_fund = len(items) - n_etf
    total = len(items) + len(dropped)
    if n_etf < max(MIN_ETFS, MIN_CATALOG_ETFS) or n_fund < MIN_FUNDS:
        raise ValueError("only %d ETFs (min %d) and %d funds (min %d) survive"
                         % (n_etf, max(MIN_ETFS, MIN_CATALOG_ETFS), n_fund, MIN_FUNDS))
    if len(dropped) > MAX_DROPPED_FRACTION * total:
        raise ValueError("%d of %d items dropped (> 10%%); Nasdaq outage?" % (len(dropped), total))


class _FakeResp:
    def __init__(self, body):
        self._body = body
        self.headers = email.message.Message()

    def read(self, n=-1):
        return self._body if n < 0 else self._body[:n]

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def self_test():
    """Offline checks of the output guard, the download cap, the price check
    and the shipped catalog. Returns 0 or raises AssertionError."""
    import tempfile
    from unittest import mock

    # write guard: only the catalog file itself
    with tempfile.TemporaryDirectory() as tmp:
        for bad in (
            Path(tmp) / "x.json",
            Path(tmp) / "newdir" / "x.json",
            REPO_ROOT / "data" / "other.json",
            REPO_ROOT / "data" / ".." / "tools" / "build_catalog.py",
        ):
            try:
                write_catalog(bad, "x")
            except ValueError:
                pass
            else:
                raise AssertionError("write_catalog accepted %s" % bad)
        assert not (Path(tmp) / "x.json").exists()
        assert not (Path(tmp) / "newdir").exists()
        assert not (REPO_ROOT / "data" / "other.json").exists()
        link = Path(tmp) / "link.json"
        link.symlink_to(Path(tmp) / "target.json")
        try:
            write_catalog(link, "x")
        except ValueError:
            pass
        else:
            raise AssertionError("write_catalog followed a symlink")
        assert not (Path(tmp) / "target.json").exists()

        # symlinked target / parent, with target parameter
        for mode in ("file", "dir"):
            root = Path(tmp) / mode
            (root / "outside").mkdir(parents=True)
            victim = root / "outside" / "victim.json"
            victim.write_text("orig")
            if mode == "file":
                (root / "data").mkdir()
                tgt = root / "data" / "fidelity_catalog.json"
                tgt.symlink_to(victim)
            else:
                (root / "data").symlink_to(root / "outside")
                tgt = root / "data" / "victim.json"
            try:
                write_catalog(tgt, "x", target=tgt)
            except ValueError:
                pass
            else:
                raise AssertionError("write_catalog accepted symlink (%s)" % mode)
            assert victim.read_text() == "orig"
            assert sorted(os.listdir(root / "outside")) == ["victim.json"]
        # normal atomic write, replaces content, leaves no temp files
        d = Path(tmp) / "ok"
        d.mkdir()
        t = d / "fidelity_catalog.json"
        t.write_text("old")
        write_catalog(t, "new", target=t)
        assert t.read_text() == "new" and os.listdir(d) == [t.name]

    # sanity floors, incl. a Nasdaq outage ('Something went wrong' for all)
    def mk(n_etf, n_fund):
        return ([{"symbol": "E%d" % i, "kind": KIND_ETF} for i in range(n_etf)]
                + [{"symbol": "F%d" % i, "kind": "fund"} for i in range(n_fund)])
    check_floors(mk(100, 200), [])
    check_floors(mk(100, 200), mk(10, 20))
    check_floors(mk(100, 200), mk(10, 20))
    for items_, dr in ((mk(100, 200), mk(20, 20)), (mk(79, 200), []), (mk(100, 99), []), ([], [])):
        try:
            check_floors(items_, dr)
        except ValueError:
            pass
        else:
            raise AssertionError("floors accepted %d/%d" % (len(items_), len(dr)))
    outage = {"data": None, "status": {"rCode": 200, "bCodeMessage": [
        {"code": 1000, "errorMessage": "Something went wrong.Please try again later."}]}}
    shipped = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))["items"]
    me = sys.modules[__name__]
    with tempfile.TemporaryDirectory() as tmp2:
        cat = Path(tmp2) / "fidelity_catalog.json"
        cat.write_text("orig")
        with mock.patch.object(me, "CATALOG_PATH", cat), \
             mock.patch.object(me, "fetch_sources", lambda: dict.fromkeys(SAMPLE_FILES, "")), \
             mock.patch.object(me, "parse_etf_page", lambda p: [0] * MIN_ETFS), \
             mock.patch.object(me, "parse_fund_page", lambda p: [0] * MIN_FUNDS), \
             mock.patch.object(me, "parse_nasdaq_trader", lambda a, b: []), \
             mock.patch.object(me, "build_items", lambda *a: (shipped, [])), \
             mock.patch.object(me, "fetch", lambda *a, **k: json.dumps(outage)), \
             mock.patch.object(time, "sleep", lambda s: None), \
             mock.patch.object(me, "verify_items",
                               functools.partial(verify_items, log=io.StringIO())), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            try:
                main(["--verify", "--write"])
            except SystemExit as exc:
                assert exc.code not in (0, None), exc.code
            else:
                raise AssertionError("outage run exited 0")
        assert cat.read_text() == "orig"

    # download cap
    with mock.patch.object(urllib.request, "urlopen",
                           lambda *a, **k: _FakeResp(b"x" * (MAX_DOWNLOAD_BYTES + 1))):
        try:
            fetch("https://example.invalid/", min_gap=0)
        except ValueError as exc:
            assert "exceeds" in str(exc)
        else:
            raise AssertionError("oversize download accepted")
    with mock.patch.object(urllib.request, "urlopen",
                           lambda *a, **k: _FakeResp(b"y" * MAX_DOWNLOAD_BYTES)):
        assert len(fetch("https://example.invalid/", min_gap=0)) == MAX_DOWNLOAD_BYTES

    # price check (bodies as served by api.nasdaq.com on 2026-10-08)
    def info(sym, ac, primary=None, secondary=None):
        return {"symbol": sym, "assetClass": ac,
                "primaryData": primary, "secondaryData": secondary}

    good = [
        info("FXAIX", "MUTUALFUNDS", {"lastSalePrice": "$230.12"}),
        info("FBCG", "ETF", {"lastSalePrice": "N/A"}),
        info("FBCG", "ETF", {"lastSalePrice": ""}, {"lastSalePrice": "$1,234.50"}),
    ]
    for d in good:
        assert info_is_quotable(d, d["symbol"], d["assetClass"].lower()), d
    bad = [
        info("SPAXX", "MUTUALFUNDS", {"lastSalePrice": "Average Maturity: 37 Days"}),
        info("FMSNX", "MUTUALFUNDS", {"lastSalePrice": ""}),
        info("X", "ETF", None, None),
        info("X", "ETF", {"lastSalePrice": None}),
        info("X", "ETF", {"lastSalePrice": 12.5}),
        info("X", "ETF", {"lastSalePrice": "-$3"}),
        info("X", "ETF", {"lastSalePrice": "1234,567"}),
        info("X", "ETF", {"lastSalePrice": "$1."}),
    ]
    for d in bad:
        assert not info_is_quotable(d, d["symbol"], d["assetClass"].lower()), d
    assert not info_is_quotable(info("FBCG", "ETF", {"lastSalePrice": "$1"}), "FBCG", "mutualfunds")
    assert not info_is_quotable(info("FBCG", "ETF", {"lastSalePrice": "$1"}), "OTHER", "etf")
    assert not info_is_quotable("x", "X", "etf")

    # shipped catalog
    doc = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
    assert not {i["symbol"] for i in doc["items"]} & {"SPAXX", "FDRXX", "FMSNX"}
    for item in doc["items"]:
        if item["kind"] == KIND_ETF:
            assert item["category"] not in ("Equity", "Fixed Income"), item
    print("self-test OK", file=sys.stderr)
    return 0


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Rebuild data/fidelity_catalog.json from Fidelity and Nasdaq "
        "sources and print a unified diff against the existing file. "
        "Writes only data/fidelity_catalog.json, and only with --write. "
        "Maintainer tool; never run by the app."
    )
    ap.add_argument("--write", action="store_true", help="overwrite the catalog file")
    ap.add_argument(
        "--verify",
        action="store_true",
        help="check every symbol on api.nasdaq.com (one request per symbol, "
        ">= %.0f s apart) and drop those that do not resolve or whose info "
        "answer has no parseable lastSalePrice" % REQUEST_GAP_S,
    )
    ap.add_argument(
        "--offline-samples",
        metavar="DIR",
        help="parse saved documents (%s) from DIR instead of fetching"
        % ", ".join(SAMPLE_FILES.values()),
    )
    ap.add_argument(
        "--self-test",
        action="store_true",
        help="run the offline self-tests (write guard, download cap, price check) and exit",
    )
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.verify and args.offline_samples:
        ap.error("--verify needs the network; it cannot be combined with --offline-samples")

    docs = load_samples(args.offline_samples) if args.offline_samples else fetch_sources()

    etf_cards = parse_etf_page(docs["etfs"])
    fund_rows = parse_fund_page(docs["funds"])
    nasdaq_etfs = parse_nasdaq_trader(docs["nasdaq_listed"], docs["other_listed"])
    if len(etf_cards) < MIN_ETFS or len(fund_rows) < MIN_FUNDS:
        sys.exit(
            "parser sanity check failed: %d ETF cards (min %d), %d fund rows (min %d); "
            "the page layout probably changed" % (len(etf_cards), MIN_ETFS, len(fund_rows), MIN_FUNDS)
        )

    items, notes = build_items(etf_cards, fund_rows, nasdaq_etfs)
    for note in notes:
        print("note:", note, file=sys.stderr)

    dropped = []
    if args.verify:
        items, dropped = verify_items(items)
        for item in dropped:
            print("dropped (does not resolve or has no parseable price): %s %s"
                  % (item["symbol"], item["kind"]), file=sys.stderr)

    old = CATALOG_PATH.read_text(encoding="utf-8") if CATALOG_PATH.exists() else ""
    new = render(items, datetime.date.today().isoformat())
    diff = list(
        difflib.unified_diff(
            old.splitlines(keepends=True),
            new.splitlines(keepends=True),
            fromfile=str(CATALOG_PATH),
            tofile=str(CATALOG_PATH) + " (new)",
        )
    )
    sys.stdout.writelines(diff if diff else ["no changes\n"])
    n_etf = sum(1 for i in items if i["kind"] == KIND_ETF)
    print("%d items (%d etf, %d fund)" % (len(items), n_etf, len(items) - n_etf),
          file=sys.stderr)

    try:
        check_floors(items, dropped)
    except ValueError as exc:
        sys.exit("sanity floor failed, nothing written: %s" % exc)

    if args.write:
        write_catalog(CATALOG_PATH, new)
        print("wrote", CATALOG_PATH, file=sys.stderr)
    else:
        print("not written (use --write)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
