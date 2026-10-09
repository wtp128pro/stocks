/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-yahoo.h - StkYahoo: StkProvider for MARKET INDICES (^GSPC, ^DJI, ^IXIC,
 * ^RUT) backed by the unofficial, keyless Yahoo Finance chart endpoint.
 * Read-only GETs over HTTPS (plain HTTP is accepted only towards loopback
 * hosts, for tests). No API key, cookie, crumb, consent handling, credential
 * or referer is ever sent, and the app's identifying User-Agent is not
 * disguised (libsoup adds Host and Accept-Encoding); nothing is stored. If
 * Yahoo refuses the requests (HTTP 4xx, throttling) the operations fail;
 * they are never retried differently or worked around.
 *
 * The ONLY request ever sent is
 *
 *   GET <base>/v8/finance/chart/SYM?range=R&interval=I
 *
 * (SYM upper-cased and percent-encoded, "^GSPC" -> "%5EGSPC"):
 *
 *   STK_RANGE_1D  range=1d   interval=5m
 *   STK_RANGE_1M  range=1mo  interval=1d
 *   STK_RANGE_3M  range=3mo  interval=1d
 *   STK_RANGE_6M  range=6mo  interval=1d
 *   STK_RANGE_1Y  range=1y   interval=1d
 *
 * A quote is served from the STK_RANGE_1D URL. No other Yahoo endpoint
 * (search, quote, crumb, ...) and no other host is used.
 *
 * Kinds: only STK_KIND_INDEX is accepted. Any other kind, an invalid symbol
 * (stk_symbol_is_valid()) or an invalid range is reported through the
 * callback as STK_ERROR_INVALID_ARG without network traffic.
 *
 * Politeness (enforced here, per StkYahoo instance; the scheduler does no
 * pacing). All operations of an instance share ONE queue:
 *  - At most one request is on the wire at any time. The next one starts
 *    only after the previous attempt has ended AND at least
 *    STK_YAHOO_MIN_GAP_MS have passed since the previous attempt STARTED
 *    (the instant it was handed to the HTTP session). This holds for retries
 *    too: a retry is an attempt like any other and waits for its slot.
 *  - Retries: only after STK_ERROR_NETWORK, STK_ERROR_TIMEOUT and HTTP 5xx
 *    (STK_ERROR_HTTP), at most STK_YAHOO_MAX_RETRIES times per operation. The
 *    spacing above is the only delay between attempts. HTTP 429 and every
 *    other 4xx, parse errors and everything else are final: a 429 is
 *    reported (STK_ERROR_RATE_LIMITED) and the request is not repeated. A
 *    retry re-enters the queue at its head.
 *  - An operation counts toward STK_YAHOO_MAX_INFLIGHT from the moment it
 *    is accepted (queued, on the wire or waiting for a retry) until its
 *    callback has returned. An operation that would exceed the cap fails
 *    through the callback with STK_ERROR_RATE_LIMITED and no network
 *    traffic; argument validation comes first.
 *  - The per-attempt deadline (timeout, STK_YAHOO_TIMEOUT_MS by default)
 *    starts when the attempt is sent, not when the operation is queued, and
 *    covers headers and body: exceeding it is STK_ERROR_TIMEOUT.
 *  - Cancelling a queued operation (before its first attempt or while it
 *    waits for a retry) completes it promptly with G_IO_ERROR_CANCELLED and
 *    sends nothing; cancelling an attempt on the wire aborts it, and the
 *    operation completes with G_IO_ERROR_CANCELLED.
 *  - The callback is never invoked before the *_async function returned
 *    and every operation completes exactly once.
 *
 * Status mapping: 2xx -> the body is parsed; 404 -> STK_ERROR_NOT_FOUND;
 * 429 -> STK_ERROR_RATE_LIMITED; any other status (3xx are not followed,
 * other 4xx, 5xx) -> STK_ERROR_HTTP. A JSON body whose chart.error is set
 * (Yahoo's way to say "unknown symbol", usually with HTTP 404) ->
 * STK_ERROR_NOT_FOUND; malformed JSON or a body of the wrong shape ->
 * STK_ERROR_PARSE; a body that ends before its declared Content-Length or a
 * transport failure -> STK_ERROR_NETWORK. The body is limited to
 * STK_YAHOO_MAX_BODY_BYTES after content decoding (larger -> STK_ERROR_PARSE).
 * TLS certificates are always verified.
 *
 * Search: Yahoo's search endpoint is not used. search_async() completes with
 * an EMPTY array for every text (the text is not inspected) and never
 * touches the network; the indices are found through the catalog.
 *
 * All functions must be used from the thread that runs the GLib main context
 * (normally the main thread); StkYahoo is not thread-safe. Operations keep
 * the provider alive until they complete.
 */
#ifndef STK_YAHOO_H
#define STK_YAHOO_H

#include <gio/gio.h>

#include "stk-provider.h"

G_BEGIN_DECLS

/** Production origin. */
#define STK_YAHOO_BASE_URL "https://query1.finance.yahoo.com"
/** Default per-attempt deadline, milliseconds (starts when sent). */
#define STK_YAHOO_TIMEOUT_MS 10000
/** Default minimum time between two request STARTS, milliseconds. */
#define STK_YAHOO_MIN_GAP_MS 3000
/** Maximum number of retries after the first attempt. */
#define STK_YAHOO_MAX_RETRIES 2
/** Maximum accepted response body, bytes (after content decoding). */
#define STK_YAHOO_MAX_BODY_BYTES (2 * 1024 * 1024)
/** Maximum number of outstanding operations (queued + on the wire). */
#define STK_YAHOO_MAX_INFLIGHT 16

#define STK_TYPE_YAHOO (stk_yahoo_get_type())
G_DECLARE_FINAL_TYPE(StkYahoo, stk_yahoo, STK, YAHOO, GObject)

/**
 * stk_yahoo_new:
 *
 * Returns: (transfer full): a StkProvider talking to STK_YAHOO_BASE_URL with
 *   STK_YAHOO_TIMEOUT_MS and STK_YAHOO_MIN_GAP_MS. It owns one HTTP session
 *   and the request queue, so create ONE instance and share it: the spacing
 *   only holds per instance.
 */
StkYahoo *stk_yahoo_new(void);

/**
 * stk_yahoo_new_custom:
 * @base_url: origin without path, e.g. "https://query1.finance.yahoo.com".
 *   Only "https://" origins and "http://" origins whose host is exactly
 *   127.0.0.1, ::1 or localhost are accepted (the latter exist so tests can
 *   use a loopback fake server); userinfo, query, fragment and any path
 *   other than "" or "/" are rejected. A trailing "/" is dropped.
 * @timeout_ms: per-attempt deadline, 1..600000.
 * @min_gap_ms: minimum time between request starts, 0..600000. Production
 *   uses STK_YAHOO_MIN_GAP_MS; smaller values exist so tests need not sleep.
 * @error: (nullable): receives STK_ERROR_INVALID_ARG if @base_url is not
 *   acceptable.
 *
 * Returns: (transfer full) (nullable): the provider, or NULL with @error set.
 *   Out-of-range numbers are a programming error (critical, NULL).
 */
StkYahoo *stk_yahoo_new_custom(const char *base_url, guint timeout_ms, guint min_gap_ms,
                               GError **error);

/* ------------------------------------------------------------------------ */
/* Request builder and response parsers (pure; they run offline in tests)    */
/* ------------------------------------------------------------------------ */

/**
 * stk_yahoo_chart_url:
 * @base_url: origin as accepted by stk_yahoo_new_custom(), no trailing "/".
 * @symbol: must satisfy stk_symbol_is_valid(); upper-cased and
 *   percent-encoded into the URL path.
 * @kind: must be STK_KIND_INDEX.
 * @range: selects range and interval (table in the file comment).
 * @error: (nullable): receives STK_ERROR_INVALID_ARG.
 *
 * Returns: (transfer full): the chart URL (also the quote URL when @range is
 *   STK_RANGE_1D); NULL with STK_ERROR_INVALID_ARG if @symbol, @kind or
 *   @range is invalid.
 */
char *stk_yahoo_chart_url(const char *base_url, const char *symbol, StkKind kind, StkRange range,
                          GError **error);

/**
 * stk_yahoo_parse_quote:
 * @data: body of a range=1d chart response (@len bytes, need not be
 *   NUL-terminated). The caller MUST pass a range=1d response: the previous
 *   close of other ranges is the close before the range start and would give
 *   a wrong change.
 * @len: body length; at most STK_YAHOO_MAX_BODY_BYTES.
 * @symbol: the requested symbol, satisfying stk_symbol_is_valid().
 * @kind: the requested kind, STK_KIND_INDEX.
 * @error: (nullable): see the error mapping in the file comment.
 *
 * Maps chart.result[0].meta to a StkQuote. The result's symbol and kind are
 * the arguments, never response content. price = regularMarketPrice (a finite
 * number > 0, else STK_ERROR_PARSE). With previous close p = previousClose if
 * that is a finite number > 0, else chartPreviousClose likewise: change =
 * price - p and change_pct = change / p * 100; without a usable p (or if
 * either result is not finite) both are NAN. as_of_ms = regularMarketTime *
 * 1000 (0 if absent or outside
 * 2022-01-01..2100-12-31). is_realtime is FALSE (delayed data) and
 * market_open is FALSE (the chart endpoint does not report the market
 * state). currency = meta.currency if it is three upper-case ASCII letters,
 * else "USD". name = meta.longName, else meta.shortName, when that is at most
 * 256 bytes without control characters; else "".
 *
 * Returns: (transfer full): the quote, or NULL with @error set.
 */
StkQuote *stk_yahoo_parse_quote(const guint8 *data, gsize len, const char *symbol, StkKind kind,
                                GError **error);

/**
 * stk_yahoo_parse_series:
 * @data: chart response body fetched for @range.
 * @len: body length; at most STK_YAHOO_MAX_BODY_BYTES.
 * @range: the requested range; the series' range.
 * @error: (nullable): see the error mapping in the file comment.
 *
 * Maps chart.result[0] (timestamp[] in epoch seconds, indicators.quote[0]
 * .close[]) to a StkSeries. A sample is skipped if its timestamp is missing,
 * null, non-numeric or outside 2022-01-01..2100-12-31, if its close is
 * missing, null, non-numeric or non-finite, or if it would break the
 * ascending order. A response without timestamps is an empty series (valid).
 *
 * STK_RANGE_1D: only samples with regular.start <= t < regular.end are kept,
 * where meta.currentTradingPeriod.regular.{start,end} (epoch seconds within
 * the same window, start < end; required, else STK_ERROR_PARSE) is the
 * trading day the response describes; t_ms is the true UTC instant
 * (seconds * 1000). prev_close = meta.previousClose if a finite number > 0,
 * else meta.chartPreviousClose likewise, else NAN. (Valid for a range=1d
 * response only.)
 *
 * Daily ranges: t_ms is 00:00 UTC of the America/New_York calendar date of
 * the sample's timestamp; there is at most one point per date (the latest
 * sample with a close wins); prev_close is NAN (chartPreviousClose of these
 * responses is the close before the range start and is never used).
 *
 * Returns: (transfer full): series with range == @range (possibly empty), or
 *   NULL with @error set.
 */
StkSeries *stk_yahoo_parse_series(const guint8 *data, gsize len, StkRange range, GError **error);

G_END_DECLS

#endif /* STK_YAHOO_H */
