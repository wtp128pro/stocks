/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-nasdaq.h - StkNasdaq: StkProvider backed by the unofficial Nasdaq web
 * API (https://api.nasdaq.com). Read-only GETs over HTTPS (plain HTTP is
 * accepted only towards loopback hosts, for tests); no credentials, API keys
 * or cookies.
 *
 * Serves STK_KIND_STOCK, STK_KIND_ETF and STK_KIND_FUND only: STK_KIND_INDEX
 * is rejected with STK_ERROR_INVALID_ARG and no network traffic (indices come
 * from StkYahoo; StkRouter sends each symbol to the right provider).
 *
 * Endpoints used (SYM is percent-encoded; ASSET comes only from the StkKind:
 * stocks / etf / mutualfunds):
 *   quote        /api/quote/SYM/info?assetclass=ASSET
 *   1D series    /api/quote/SYM/chart?assetclass=ASSET            (stock, ETF)
 *   1M..1Y       /api/quote/SYM/chart?assetclass=ASSET&charttype=real
 *                    &fromdate=YYYY-MM-DD&todate=YYYY-MM-DD       (stock, ETF)
 *   fund 1M..1Y  /api/quote/SYM/historical?assetclass=mutualfunds
 *                    &fromdate=...&todate=...&limit=N
 *   search       /api/autocomplete/slookup/10?search=TEXT
 *
 * Request policy (per operation; every request is one GET):
 *  - A request attempt that has not completed (headers and body) within the
 *    timeout (STK_NASDAQ_TIMEOUT_MS by default) fails with STK_ERROR_TIMEOUT.
 *  - After a failed attempt the request is retried at most
 *    STK_NASDAQ_MAX_RETRIES times, waiting backoff, 2 * backoff, ... between
 *    attempts (STK_NASDAQ_BACKOFF_BASE_MS by default), and ONLY for
 *    STK_ERROR_NETWORK, STK_ERROR_TIMEOUT and HTTP 5xx (STK_ERROR_HTTP).
 *    Everything else (4xx, parse errors, ...) is final. Cancelling during an
 *    attempt or during a back-off wait ends the operation immediately with
 *    G_IO_ERROR_CANCELLED.
 *  - HTTP status mapping: 2xx -> body is parsed; 429 -> STK_ERROR_RATE_LIMITED;
 *    400 and 404 -> STK_ERROR_NOT_FOUND (the API answers 400 for an unknown
 *    symbol or an assetclass that does not match the instrument); every other
 *    status (3xx are not followed, 5xx, anything else) -> STK_ERROR_HTTP.
 *    Only statuses 500..599 are retried.
 *  - Body: at most STK_NASDAQ_MAX_BODY_BYTES bytes after content decoding;
 *    a larger body is rejected with STK_ERROR_PARSE (a declared Content-Length
 *    above the cap is refused before reading; otherwise at most cap + 1 bytes
 *    are ever buffered).
 *  - The body must be UTF-8 JSON of the shape
 *    {"data": ..., "status": {"rCode": N, ...}}. rCode != 200 (the API
 *    reports errors this way, usually with HTTP 200) -> STK_ERROR_NOT_FOUND;
 *    invalid JSON or a missing/mistyped required field -> STK_ERROR_PARSE.
 *    Required: status.rCode, a quote's price, the data container and, for charts, data.chart
 *    (array or null) and, for fund history, data.tradesTable (object or null). Every other
 *    field is optional and never aborts parsing: a quote field that is
 *    missing or "N/A" becomes NAN, an unusable chart point or row is skipped.
 *  - Transport failures (DNS, connect, TLS, I/O) -> STK_ERROR_NETWORK; so is
 *    a body that ends before its declared Content-Length (checked only when
 *    the response has no Content-Encoding). TLS
 *    certificate validation is never disabled and redirects are not followed.
 *  - Every operation is one GET, except a STK_RANGE_1D series, which issues
 *    two GETs in parallel: the chart and the quote (/info). Retries, the
 *    timeout and cancellation apply to each of them independently, and the
 *    series completes when both have ended. The series' prev_close is
 *    price - change of the quote's REGULAR-session block (see
 *    stk_nasdaq_parse_quote()) when that is finite and > 0, else NAN; a quote
 *    whose regular-session block has no usable price (so that the other
 *    block would be used) yields NAN, because the other block's change is
 *    relative to a different close. The chart's own data.previousClose is NEVER
 *    used: it lags until the provider's nightly roll (after hours it is the
 *    close from two sessions back). Regular-session block by market state
 *    (Open: primaryData; Pre-Market/After-Hours: secondaryData; Closed: secondaryData if
 *    the response still carries one, else primaryData): its price minus netChange is the
 *    previous session's close (AAPL 340.42 - 3.75 = 336.67 both after hours
 *    and after the roll; open: live price minus netChange). Captures exist
 *    for Open, After-Hours and Closed; Pre-Market is inferred. If the
 *    quote request fails (any error, including its
 *    retries), the series still succeeds with prev_close NAN; if the chart
 *    request fails the series fails with the chart's error and the quote
 *    request is cancelled. If the caller cancels, both are cancelled and the
 *    series fails with G_IO_ERROR_CANCELLED (also when only the quote was
 *    still pending).
 *  - Concurrency: libsoup's connection limits apply (default 2 connections
 *    per host); further requests wait in libsoup's queue. At most
 *    STK_NASDAQ_MAX_INFLIGHT requests (a 1D series counts as 2) are
 *    outstanding per provider; an operation that would exceed the cap fails
 *    immediately (through the callback, no network traffic) with
 *    STK_ERROR_RATE_LIMITED. Argument validation comes first, so invalid
 *    arguments are still STK_ERROR_INVALID_ARG. A request stays outstanding
 *    until its operation has completed (including its retries and back-off)
 *    and its callback has returned.
 *
 * StkNasdaq instances (stk_nasdaq_new(), stk_nasdaq_new_custom() and the StkProvider
 * operations on them) must be used from the thread that runs the GLib main context
 * (normally the main thread); StkNasdaq is not thread-safe. The URL builders
 * (stk_nasdaq_quote_url(), stk_nasdaq_series_url(), stk_nasdaq_search_url()) and the
 * stk_nasdaq_parse_*() functions are pure and may be called from any thread.
 */
#ifndef STK_NASDAQ_H
#define STK_NASDAQ_H

#include <gio/gio.h>

#include "stk-provider.h"

G_BEGIN_DECLS

/** Production origin. */
#define STK_NASDAQ_BASE_URL "https://api.nasdaq.com"
/** Default per-attempt deadline, milliseconds. */
#define STK_NASDAQ_TIMEOUT_MS 10000
/** Default first back-off delay, milliseconds (doubles per retry). */
#define STK_NASDAQ_BACKOFF_BASE_MS 1000
/** Maximum number of retries after the first attempt. */
#define STK_NASDAQ_MAX_RETRIES 2
/** Maximum accepted response body, bytes (after content decoding). */
#define STK_NASDAQ_MAX_BODY_BYTES (2 * 1024 * 1024)
/** Maximum number of outstanding requests per provider (see above). */
#define STK_NASDAQ_MAX_INFLIGHT 64
/** Maximum number of hits stk_provider_search_finish() returns. */
#define STK_NASDAQ_MAX_SEARCH_HITS 10

#define STK_TYPE_NASDAQ (stk_nasdaq_get_type())
G_DECLARE_FINAL_TYPE(StkNasdaq, stk_nasdaq, STK, NASDAQ, GObject)

/**
 * stk_nasdaq_new:
 *
 * Creates the production provider.
 *
 * Preconditions:
 *  - Called from the thread that will run the GLib main context.
 * Postconditions:
 *  - The instance uses STK_NASDAQ_BASE_URL, STK_NASDAQ_TIMEOUT_MS and
 *    STK_NASDAQ_BACKOFF_BASE_MS and has no outstanding request.
 * Thread affinity: main thread only.
 * Ownership: the caller owns the returned reference.
 *
 * Returns: (transfer full): a StkProvider talking to STK_NASDAQ_BASE_URL with
 *   the default timeout and back-off. It owns one HTTP session (created here,
 *   reused by every request); create one instance and share it.
 */
StkNasdaq *stk_nasdaq_new(void);

/**
 * stk_nasdaq_new_custom:
 * @base_url: origin without path, e.g. "https://api.nasdaq.com". Only
 *   "https://" origins and "http://" origins whose host is exactly 127.0.0.1,
 *   ::1 or localhost are accepted (the latter exist so tests can use a
 *   loopback fake server); userinfo, query, fragment and any path other than
 *   "" or "/" are rejected.
 * @timeout_ms: per-attempt deadline, 1..600000.
 * @backoff_base_ms: first back-off delay, 0..600000 (0 retries immediately).
 *
 * Creates a provider with a custom origin and timing.
 *
 * Preconditions:
 *  - @base_url is non-NULL and NUL-terminated; @timeout_ms and
 *    @backoff_base_ms are in the ranges above (violations are a
 *    g_return_val_if_fail() critical returning NULL).
 *  - Called from the thread that will run the GLib main context.
 * Postconditions:
 *  - On success the provider holds a normalised copy of @base_url (a single
 *    trailing "/" removed) and the two timings. If @base_url is not
 *    acceptable NULL is returned and nothing is allocated that outlives the
 *    call.
 * Thread affinity: main thread only.
 * Ownership: @base_url stays with the caller (copied); the caller owns the
 *   returned reference.
 *
 * Returns: (transfer full) (nullable): the provider, or NULL if @base_url is
 *   not acceptable. Out-of-range numbers are a programming error (critical,
 *   NULL).
 */
StkNasdaq *stk_nasdaq_new_custom(const char *base_url, guint timeout_ms, guint backoff_base_ms);

/* ------------------------------------------------------------------------ */
/* Request builders (pure; used by the provider and by the offline tests)    */
/* ------------------------------------------------------------------------ */

/**
 * stk_nasdaq_quote_url:
 * @base_url: origin as accepted by stk_nasdaq_new_custom(), no trailing "/".
 * @symbol: must satisfy stk_symbol_is_valid(); upper-cased and
 *   percent-encoded into the URL.
 * @kind: selects assetclass (stocks / etf / mutualfunds).
 * @error: (nullable): receives STK_ERROR_INVALID_ARG.
 *
 * Builds the /info URL of a quote request.
 *
 * Preconditions:
 *  - @base_url is non-NULL and NUL-terminated (it is inserted verbatim; it is
 *    not validated here).
 *  - @symbol is NULL or NUL-terminated (NULL fails stk_symbol_is_valid()).
 *  - @error is NULL or points to a NULL GError* (else critical, NULL).
 * Postconditions:
 *  - On success *@error is untouched. On failure NULL is returned, *@error
 *    (when @error is non-NULL) is set to STK_ERROR_INVALID_ARG, and nothing is
 *    allocated that outlives the call.
 * Thread affinity: any thread (pure).
 * Ownership: @base_url and @symbol stay with the caller; the caller owns the
 *   returned string.
 *
 * Returns: (transfer full): the quote URL; NULL with STK_ERROR_INVALID_ARG if
 *   @symbol or @kind is invalid.
 */
char *stk_nasdaq_quote_url(const char *base_url, const char *symbol, StkKind kind,
                           GError **error);

/**
 * stk_nasdaq_series_url:
 * @base_url: as for stk_nasdaq_quote_url().
 * @symbol: as for stk_nasdaq_quote_url().
 * @kind: selects assetclass; STK_KIND_FUND selects the historical endpoint.
 * @range: STK_RANGE_1D has no date parameters; the other ranges use
 *   todate = @today and fromdate = @today minus 1, 3, 6 or 12 calendar months
 *   (the day of month is clamped to the length of the target month).
 * @today: valid GDate, the New York calendar date (year within
 *   [STK_MARKET_MIN_YEAR, STK_MARKET_MAX_YEAR]); ignored for STK_RANGE_1D.
 * @error: (nullable): receives STK_ERROR_INVALID_ARG.
 *
 * Builds the chart (stocks, ETFs) or historical (funds) URL of a series
 * request.
 *
 * Preconditions:
 *  - @base_url is non-NULL and NUL-terminated (inserted verbatim).
 *  - @symbol is NULL or NUL-terminated.
 *  - @today is NULL or points to a GDate; it is read only for ranges other
 *    than STK_RANGE_1D, where NULL or an invalid date is rejected.
 *  - @error is NULL or points to a NULL GError* (else critical, NULL).
 * Postconditions:
 *  - On success *@error is untouched. On failure NULL is returned and *@error
 *    (when non-NULL) is set to STK_ERROR_INVALID_ARG.
 * Thread affinity: any thread (pure).
 * Ownership: all arguments stay with the caller; the caller owns the returned
 *   string.
 *
 * Returns: (transfer full): the URL; NULL with STK_ERROR_INVALID_ARG if the
 *   symbol, kind, range or date is invalid or if kind is STK_KIND_FUND and
 *   range is STK_RANGE_1D (funds have no intraday data).
 */
char *stk_nasdaq_series_url(const char *base_url, const char *symbol, StkKind kind,
                            StkRange range, const GDate *today, GError **error);

/**
 * stk_nasdaq_search_url:
 * @base_url: as for stk_nasdaq_quote_url().
 * @text: user text; leading/trailing ASCII whitespace is dropped, then it
 *   must be 1..64 bytes of valid UTF-8 without control characters. It is
 *   percent-encoded into the URL.
 * @error: (nullable): receives STK_ERROR_INVALID_ARG.
 *
 * Builds the autocomplete URL of a search request.
 *
 * Preconditions:
 *  - @base_url is non-NULL and NUL-terminated (inserted verbatim).
 *  - @text is NULL (rejected) or NUL-terminated. It is scanned once for the NUL (the scan
 *    stops early as soon as the text up to its last non-whitespace byte is longer than 64
 *    bytes, which is rejected anyway); validation then re-reads only the trimmed text, at
 *    most 64 bytes.
 *  - @error is NULL or points to a NULL GError* (else critical, NULL).
 * Postconditions:
 *  - On success *@error is untouched. On failure NULL is returned and *@error
 *    (when non-NULL) is set to STK_ERROR_INVALID_ARG.
 * Thread affinity: any thread (pure).
 * Ownership: @base_url and @text stay with the caller; the caller owns the
 *   returned string.
 *
 * Returns: (transfer full): the URL; NULL with STK_ERROR_INVALID_ARG if @text
 *   is unacceptable.
 */
char *stk_nasdaq_search_url(const char *base_url, const char *text, GError **error);

/* ------------------------------------------------------------------------ */
/* Response parsers (pure; take the raw body so they run offline)           */
/* ------------------------------------------------------------------------ */

/**
 * stk_nasdaq_parse_quote:
 * @data: response body (@len bytes, need not be NUL-terminated).
 * @len: body length; at most STK_NASDAQ_MAX_BODY_BYTES.
 * @symbol: the requested symbol, satisfying stk_symbol_is_valid().
 * @kind: the requested kind.
 * @error: (nullable): see the error mapping in the file comment.
 *
 * Maps a /info response to a StkQuote. The result's symbol and kind are the
 * arguments, never response content. Of the response's two price blocks,
 * the regular-session one is preferred: primaryData while data.marketStatus
 * is "Open"; secondaryData (the official close) in pre-market and
 * after-hours; in "Closed" (after the nightly roll, when the provider nulls
 * secondaryData and puts the official close, date-only lastTradeTimestamp,
 * isRealTime false, in primaryData) primaryData, or secondaryData if the
 * response still carries one. In pre-market and after-hours primaryData is
 * never regular: its change is measured from today's close. If the
 * preferred block has no usable price the other one is used. From the
 * chosen block: lastSalePrice -> price,
 * netChange -> change, percentageChange -> change_pct (NAN when "N/A",
 * missing or malformed, except that a missing or empty one is 0 when the
 * block's deltaIndicator is "unch"), isRealTime -> is_realtime,
 * lastTradeTimestamp ("Oct 8, 2026 7:25 PM ET", "Closed at Oct 8, 2026 4:00
 * PM ET" or date only "Oct 8, 2026") -> as_of_ms (a date without a time
 * maps to 12:00 America/New_York; 0 if unparsable), currency (only a
 * three-letter upper-case code is kept, else "USD"). market_open is
 * data.marketStatus == "Open" (case-insensitive). If neither block has a
 * usable price (non-negative, parsable lastSalePrice): when at least one
 * block's lastSalePrice is "N/A" the quote is returned with NAN
 * price/change/change_pct (as_of_ms 0, is_realtime FALSE); otherwise
 * (missing, null, empty or malformed price) the result is STK_ERROR_PARSE.
 * The quote's name is data.companyName only if it is non-empty, at most 256
 * bytes, valid UTF-8 and free of control characters (the StkYahoo rule);
 * any other companyName is dropped and the name is "".
 *
 * Preconditions:
 *  - @data is NULL only with @len == 0, else it points to @len readable bytes.
 *    @len > STK_NASDAQ_MAX_BODY_BYTES is rejected with STK_ERROR_PARSE.
 *  - @symbol is NULL or NUL-terminated (it must satisfy stk_symbol_is_valid(),
 *    else STK_ERROR_INVALID_ARG); @kind must be stock, ETF or fund.
 *  - @error is NULL or points to a NULL GError* (else critical, NULL).
 * Postconditions:
 *  - On success *@error is untouched. On failure NULL is returned and *@error
 *    (when non-NULL) is set (STK_ERROR_INVALID_ARG, _PARSE or _NOT_FOUND).
 * Thread affinity: any thread (pure; the only shared state is a read-only,
 *   once-initialised time zone).
 * Ownership: @data and @symbol stay with the caller and are not retained; the
 *   caller owns the returned quote.
 *
 * Returns: (transfer full): the quote, or NULL with @error set.
 */
StkQuote *stk_nasdaq_parse_quote(const guint8 *data, gsize len, const char *symbol,
                                 StkKind kind, GError **error);

/**
 * stk_nasdaq_parse_chart:
 * @data: /chart response body.
 * @len: body length.
 * @range: the requested range (selects the point semantics below).
 * @error: (nullable): see the error mapping in the file comment.
 *
 * Maps a stock/ETF /chart response (data.chart, JSON null = no points) to a
 * StkSeries. Points whose x or y is missing, non-numeric, negative or outside
 * 2022-01-01..2100-12-31 are skipped, as are points that would break the
 * ascending order.
 *
 * STK_RANGE_1D: the provider encodes x as the New York WALL-CLOCK time read
 * as if it were UTC (the point labelled "4:00 AM ET" has an x of 04:00 UTC).
 * That shift is undone, so the series' t_ms are true UTC instants. The
 * provider labels each minute bucket by its START time, so only points inside
 * the half-open regular session [09:30, close) of their New York date are
 * kept: the bucket labelled 15:59 is the last session minute, the one
 * labelled 16:00 (13:00 on early-close days) is the first after-hours minute
 * and is dropped, as are pre-market, after-hours and non-session days (see
 * stk_market_session()). prev_close is NAN: the response's data.previousClose
 * is unreliable and ignored; stk_nasdaq_parse_quote() supplies the value
 * (see the request policy).
 *
 * Other ranges: t_ms = x, which the provider sets to 00:00 UTC of the
 * trading day's date; prev_close is NAN.
 *
 * Preconditions:
 *  - @data is NULL only with @len == 0, else it points to @len readable bytes.
 *    @len > STK_NASDAQ_MAX_BODY_BYTES is rejected with STK_ERROR_PARSE.
 *  - @error is NULL or points to a NULL GError* (else critical, NULL).
 * Postconditions:
 *  - On success *@error is untouched. On failure NULL is returned and *@error
 *    (when non-NULL) is set (STK_ERROR_INVALID_ARG for an invalid @range,
 *    _PARSE or _NOT_FOUND otherwise).
 * Thread affinity: any thread (pure; the only shared state is a read-only,
 *   once-initialised time zone).
 * Ownership: @data stays with the caller and is not retained; the caller owns
 *   the returned series.
 *
 * Returns: (transfer full): series with range == @range (possibly empty), or
 *   NULL with @error set.
 */
StkSeries *stk_nasdaq_parse_chart(const guint8 *data, gsize len, StkRange range,
                                  GError **error);

/**
 * stk_nasdaq_parse_historical:
 * @data: /historical (mutual fund) response body.
 * @len: body length.
 * @range: the requested range; STK_RANGE_1D is rejected with
 *   STK_ERROR_INVALID_ARG.
 * @error: (nullable): see the error mapping in the file comment.
 *
 * Maps data.tradesTable.rows to a StkSeries sorted ascending (tradesTable
 * must be an object or null, else STK_ERROR_PARSE; a missing, null or
 * non-array rows means "no rows"), the provider listing rows newest first
 * (the order is not relied upon). Each row maps independently:
 * Rows with an unparsable date
 * ("MM/DD/YYYY" required), a "close" that is not a valid non-negative price
 * (including "N/A") or a date outside 2022-01-01..2100-12-31 are skipped.
 * t_ms is 00:00 UTC of the row's date; prev_close is NAN.
 *
 * Preconditions:
 *  - @data is NULL only with @len == 0, else it points to @len readable bytes.
 *    @len > STK_NASDAQ_MAX_BODY_BYTES is rejected with STK_ERROR_PARSE.
 *  - @error is NULL or points to a NULL GError* (else critical, NULL).
 * Postconditions:
 *  - On success *@error is untouched. On failure NULL is returned and *@error
 *    (when non-NULL) is set (STK_ERROR_INVALID_ARG for STK_RANGE_1D or an
 *    invalid @range, _PARSE or _NOT_FOUND otherwise).
 * Thread affinity: any thread (pure).
 * Ownership: @data stays with the caller and is not retained; the caller owns
 *   the returned series.
 *
 * Returns: (transfer full): series with range == @range (possibly empty), or
 *   NULL with @error set.
 */
StkSeries *stk_nasdaq_parse_historical(const guint8 *data, gsize len, StkRange range,
                                       GError **error);

/**
 * stk_nasdaq_parse_search:
 * @data: autocomplete response body.
 * @len: body length.
 * @error: (nullable): see the error mapping in the file comment.
 *
 * Maps data (null = no hits) to an array of StkSearchHit in response order,
 * at most STK_NASDAQ_MAX_SEARCH_HITS. A hit is skipped unless its "asset" is
 * STOCKS, ETF or MUTUALFUNDS (case-insensitive; selects the StkKind) and its
 * "symbol" satisfies stk_symbol_is_valid(). A hit's name is the item's "name"
 * only if it is non-empty, at most 256 bytes, valid UTF-8 and free of control
 * characters (the StkYahoo rule); otherwise the hit is still returned, with
 * name "".
 *
 * Preconditions:
 *  - @data is NULL only with @len == 0, else it points to @len readable bytes.
 *    @len > STK_NASDAQ_MAX_BODY_BYTES is rejected with STK_ERROR_PARSE.
 *  - @error is NULL or points to a NULL GError* (else critical, NULL).
 * Postconditions:
 *  - On success *@error is untouched. On failure NULL is returned and *@error
 *    (when non-NULL) is set (STK_ERROR_PARSE or _NOT_FOUND).
 * Thread affinity: any thread (pure).
 * Ownership: @data stays with the caller and is not retained; the caller owns
 *   the returned array and, through it, the hits.
 *
 * Returns: (transfer full) (element-type StkSearchHit): possibly empty array
 *   (g_ptr_array_unref() frees it and the hits), or NULL with @error set.
 */
GPtrArray *stk_nasdaq_parse_search(const guint8 *data, gsize len, GError **error);

G_END_DECLS

#endif /* STK_NASDAQ_H */
