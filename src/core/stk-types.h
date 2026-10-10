/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-types.h - core value types shared by every stocks module.
 *
 * Pure data + GLib only: no GTK, no network, no disk I/O, no global state.
 */
#ifndef STK_TYPES_H
#define STK_TYPES_H

#include <glib.h>
#include <math.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* Errors                                                                   */
/* ------------------------------------------------------------------------ */

/** GError domain used by every stocks module. */
#define STK_ERROR (stk_error_quark())

/** Error codes of the STK_ERROR domain. */
typedef enum {
  STK_ERROR_NETWORK,      /**< connection failed / DNS / TLS / I/O error     */
  STK_ERROR_TIMEOUT,      /**< request exceeded its deadline                  */
  STK_ERROR_HTTP,         /**< unexpected HTTP status (not 404 / 429 / Nasdaq 400) */
  STK_ERROR_RATE_LIMITED, /**< HTTP 429 or provider-signalled throttling      */
  STK_ERROR_PARSE,        /**< response/text did not match the expected shape */
  STK_ERROR_NOT_FOUND,    /**< unknown symbol / HTTP 404 / provider error status   */
  STK_ERROR_INVALID_ARG   /**< caller passed an argument the API rejects      */
} StkError;

/**
 * stk_error_quark:
 *
 * Returns the quark of the STK_ERROR domain ("stk-error-quark"). Defined with
 * G_DEFINE_QUARK() in stk-types.c.
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - the same non-zero quark is returned on every call; no state is changed
 *    other than the one-time quark registration.
 * Thread affinity: any thread.
 * Ownership: none (no pointers; returns a quark).
 *
 * Returns: the #GQuark of the STK_ERROR domain.
 */
GQuark stk_error_quark(void);

/* ------------------------------------------------------------------------ */
/* Enumerations                                                             */
/* ------------------------------------------------------------------------ */

/**
 * Instrument class. Selects the Nasdaq provider's `assetclass` (never guessed);
 * indices are served by StkYahoo.
 * Persisted as the tokens of stk_kind_to_string(); new kinds are only ever
 * appended, so existing enumerator values and tokens never change.
 */
typedef enum {
  STK_KIND_STOCK,
  STK_KIND_ETF,
  STK_KIND_FUND, /**< mutual fund: NAV once per day, no intraday data */
  STK_KIND_INDEX /**< market index (e.g. ^GSPC): a level, not a tradable price;
                      behaves like a stock for ranges (1D intraday + daily) */
} StkKind;

/** Chart range. STK_RANGE_1Y is the "12 months" range. */
typedef enum {
  STK_RANGE_1D,
  STK_RANGE_1M,
  STK_RANGE_3M,
  STK_RANGE_6M,
  STK_RANGE_1Y
} StkRange;

/**
 * stk_kind_to_string:
 * @kind: a valid StkKind.
 *
 * Maps an instrument class to its persistence token.
 *
 * Preconditions:
 *  - @kind is one of the StkKind enumerators (an out-of-range value is a
 *    programming error: a critical is logged).
 * Postconditions:
 *  - no state is changed. For an out-of-range @kind a critical is logged and
 *    the "stock" token is returned (checked fallback, never an out-of-bounds
 *    read).
 * Thread affinity: any thread (pure).
 * Ownership: the returned string is static; the caller must not free it.
 *
 * Returns: static, stable persistence token: "stock", "etf", "fund" or
 *   "index".
 *   Never NULL; do not free.
 */
const char *stk_kind_to_string(StkKind kind);

/**
 * stk_kind_from_string:
 * @str: (nullable): token as returned by stk_kind_to_string().
 * @out: (out): receives the kind on success; untouched on failure.
 *
 * Parses a persistence token back into a StkKind.
 *
 * Preconditions:
 *  - @out is non-NULL and writable (NULL is a programming error: critical,
 *    FALSE).
 *  - @str is NULL or a NUL-terminated string.
 * Postconditions:
 *  - on success *@out holds the kind; on failure (NULL or unknown token) *@out
 *    is untouched.
 * Thread affinity: any thread (pure).
 * Ownership: nothing is retained or transferred.
 *
 * Returns: TRUE if @str is exactly one of the tokens (case-sensitive).
 */
gboolean stk_kind_from_string(const char *str, StkKind *out);

/**
 * stk_range_to_string:
 * @range: a valid StkRange.
 *
 * Maps a chart range to its persistence token.
 *
 * Preconditions:
 *  - @range is one of the StkRange enumerators (an out-of-range value is a
 *    programming error: a critical is logged).
 * Postconditions:
 *  - no state is changed. For an out-of-range @range a critical is logged and
 *    the "1D" token is returned (checked fallback).
 * Thread affinity: any thread (pure).
 * Ownership: the returned string is static; the caller must not free it.
 *
 * Returns: static token: "1D", "1M", "3M", "6M" or "1Y". Never NULL.
 */
const char *stk_range_to_string(StkRange range);

/**
 * stk_range_from_string:
 * @str: (nullable): token as returned by stk_range_to_string().
 * @out: (out): receives the range on success; untouched on failure.
 *
 * Parses a persistence token back into a StkRange.
 *
 * Preconditions:
 *  - @out is non-NULL and writable (NULL is a programming error: critical,
 *    FALSE).
 *  - @str is NULL or a NUL-terminated string.
 * Postconditions:
 *  - on success *@out holds the range; on failure (NULL or unknown token)
 *    *@out is untouched.
 * Thread affinity: any thread (pure).
 * Ownership: nothing is retained or transferred.
 *
 * Returns: TRUE if @str is exactly one of the tokens (case-sensitive).
 */
gboolean stk_range_from_string(const char *str, StkRange *out);

/* ------------------------------------------------------------------------ */
/* StkPoint / StkSeries                                                     */
/* ------------------------------------------------------------------------ */

/** One chart sample. */
typedef struct {
  gint64 t_ms; /**< Unix epoch, milliseconds, UTC */
  double v;    /**< price / NAV; always finite    */
} StkPoint;

/**
 * StkSeries: immutable-by-convention, atomically reference-counted series of
 * samples for one instrument and range. Build it with stk_series_new() +
 * stk_series_append(), then share it (it may be handed across threads once
 * building is finished; appending is not thread-safe).
 *
 * Invariants: every point.v is finite; point.t_ms is non-decreasing.
 */
typedef struct {
  StkRange range;
  /** Previous session's close; NAN when unknown. Test with isnan(). */
  double prev_close;
  /**
   * Samples, ascending in t_ms; read-only. Valid for indices
   * [0, n_points). May be non-NULL even when n_points == 0 (storage can be
   * pre-allocated by the capacity hint); n_points is the only emptiness test.
   * The allocated capacity is internal.
   */
  const StkPoint *points;
  gsize n_points;

  /*< private >*/
  gatomicrefcount ref_count;
  gsize n_alloc;
} StkSeries;

/**
 * stk_series_new:
 * @range: the range this series covers.
 * @prev_close: previous close, or NAN when unknown. +/-INFINITY is stored as
 *   NAN.
 * @capacity_hint: expected number of points (0 = unknown); pre-allocates
 *   storage, so `points` may be non-NULL while the series is still empty.
 *
 * Creates an empty series.
 *
 * Preconditions:
 *  - @range is one of the StkRange enumerators.
 *  - @capacity_hint is bounded by the caller: the core applies no cap and
 *    requests capacity_hint * sizeof(StkPoint) (16) bytes at once (g_new
 *    aborts if that cannot be allocated). The real callers pass a count that
 *    is already bounded: the cache passes n <= STK_CACHE_MAX_SERIES_POINTS
 *    (2000, checked before the call); the Yahoo and Nasdaq parsers pass the
 *    number of elements of a JSON array or of points collected from one,
 *    inside a response body capped at STK_YAHOO_MAX_BODY_BYTES /
 *    STK_NASDAQ_MAX_BODY_BYTES (2 MiB each), so it cannot exceed the body
 *    size.
 * Postconditions:
 *  - on success: n_points == 0, ref_count == 1, prev_close is @prev_close if
 *    finite else NAN, and storage for @capacity_hint points is pre-allocated
 *    when @capacity_hint > 0 (an allocation failure aborts, g_new).
 *  - on invalid @range: a critical is logged and NULL is returned; nothing is
 *    allocated.
 * Thread affinity: any thread (the new series is private to the caller).
 * Ownership: the caller owns the returned reference.
 *
 * Returns: (transfer full): new empty series (n_points == 0) with one
 *   reference, or NULL (with a critical) if @range is not a valid StkRange.
 *   Release with stk_series_unref() (or g_autoptr(StkSeries)).
 */
StkSeries *stk_series_new(StkRange range, double prev_close, gsize capacity_hint);

/**
 * stk_series_ref:
 * @series: non-NULL series.
 *
 * Takes an additional (atomic) reference.
 *
 * Preconditions:
 *  - @series is non-NULL, was created by stk_series_new() and still holds at
 *    least one reference owned by the caller (NULL: critical, NULL returned).
 * Postconditions:
 *  - the reference count is incremented by one; nothing else changes.
 * Thread affinity: any thread (atomic reference count).
 * Ownership: the caller owns the new reference and must stk_series_unref() it.
 *
 * Returns: (transfer full): @series with its reference count incremented.
 */
StkSeries *stk_series_ref(StkSeries *series);

/**
 * stk_series_unref:
 * @series: non-NULL series.
 *
 * Drops one reference; frees the series when the last one goes away.
 *
 * Preconditions:
 *  - @series is non-NULL and the caller owns one reference to it (NULL:
 *    critical, nothing happens).
 * Postconditions:
 *  - the reference count is decremented; when it reaches zero the point
 *    storage and the series are freed and @series (and `points`) must no
 *    longer be used by anyone.
 * Thread affinity: any thread (atomic reference count).
 * Ownership: consumes the caller's reference.
 */
void stk_series_unref(StkSeries *series);

/**
 * stk_series_append:
 * @series: non-NULL series that is not shared with other threads.
 * @t_ms: sample time, ms since epoch (UTC); must be >= the last sample's.
 * @v: sample value; must be finite.
 *
 * Appends a sample. Invalid data is rejected (not a programming error, so no
 * critical is logged).
 *
 * Preconditions:
 *  - @series is non-NULL (NULL: critical, FALSE), was created by
 *    stk_series_new(), is used by one thread only and has not yet been shared.
 *  - the number of appended points is bounded by the caller: the core imposes
 *    no maximum, so repeated appends grow the storage without limit. The real
 *    callers bound it: the cache deserialiser rejects n >
 *    STK_CACHE_MAX_SERIES_POINTS (2000) before appending and
 *    stk_cache_put_series() rejects longer series; the Yahoo and Nasdaq
 *    parsers append at most the points collected from a response body capped
 *    at 2 MiB (STK_YAHOO_MAX_BODY_BYTES / STK_NASDAQ_MAX_BODY_BYTES).
 * Postconditions:
 *  - on TRUE: n_points grew by one and the new last point is (@t_ms, @v);
 *    storage may have been reallocated, so earlier `points` pointers are
 *    invalidated.
 *  - on FALSE: the series is unchanged (non-finite @v, or @t_ms before the last
 *    sample).
 *  - capacity growth: when n_points equals the allocated capacity, the capacity
 *    becomes twice its previous value, or 64 points when it was 0 (no capacity
 *    hint, or a hint of 0); otherwise it is unchanged. An allocation failure
 *    aborts (g_renew).
 * Thread affinity: the thread that owns the series during building (not
 *   thread-safe).
 * Ownership: nothing is retained; @series stays owned by the caller.
 *
 * Returns: TRUE if appended; FALSE if @v is not finite or @t_ms precedes the
 *   last sample (the series is unchanged).
 */
gboolean stk_series_append(StkSeries *series, gint64 t_ms, double v);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StkSeries, stk_series_unref)

/* ------------------------------------------------------------------------ */
/* StkQuote                                                                 */
/* ------------------------------------------------------------------------ */

/**
 * StkQuote: snapshot of an instrument's current price. Owns its strings.
 * Numeric fields that are unknown are NAN (test with isnan()).
 */
typedef struct {
  char *symbol;       /**< upper-case ticker (ASCII-uppercased by stk_quote_new), non-empty */
  char *name;         /**< display name, "" when unknown                      */
  StkKind kind;
  double price;       /**< last price (NAV for funds), NAN when unknown       */
  double change;      /**< absolute change vs previous close, may be negative */
  double change_pct;  /**< percent change (1.5 == 1.5%), may be negative      */
  gint64 as_of_ms;    /**< quote timestamp, ms since epoch UTC; 0 = unknown   */
  gboolean is_realtime; /**< FALSE for delayed data and fund NAV              */
  gboolean market_open; /**< provider says the regular session is open        */
  char *currency;     /**< ISO 4217 code, e.g. "USD"                          */
} StkQuote;

/**
 * stk_symbol_is_valid:
 * @symbol: (nullable): candidate ticker.
 *
 * Validates a ticker symbol. Only the first 16 bytes of @symbol are ever
 * examined (memchr bound), so an over-long or unterminated candidate is
 * rejected without being scanned further.
 *
 * Preconditions:
 *  - @symbol is NULL or points to a NUL-terminated string, or to at least 16
 *    readable bytes.
 * Postconditions:
 *  - no state is changed. A TRUE result proves that @symbol holds 1..15
 *    validated bytes followed by a NUL, i.e. it fits a char[16] buffer.
 * Thread affinity: any thread (pure).
 * Ownership: nothing is retained.
 *
 * Returns: TRUE iff @symbol is non-NULL, 1..15 bytes long, consists only of
 *   ASCII letters, digits and '.', '-', '^', '=' (e.g. BRK.B, BF-B, ^GSPC,
 *   EURUSD=X), and starts with an ASCII letter, digit or '^'. Anything else
 *   (path separators, whitespace, '%', '?', '#', '&', control or non-ASCII
 *   bytes, "." / ".." forms) is rejected. Case is not significant.
 *
 * Every consumer that receives a symbol from an external source (network,
 * disk, user input) MUST call this first. A valid symbol must still be
 * percent-encoded when placed in a URL and MUST NEVER be used as a raw path
 * component.
 */
gboolean stk_symbol_is_valid(const char *symbol);

/**
 * stk_quote_new:
 * @symbol: ticker satisfying stk_symbol_is_valid() (copied, ASCII-uppercased).
 *   Validated BEFORE upper-casing; an invalid symbol is a programming error
 *   (critical) and yields NULL.
 * @name: (nullable): display name (copied); NULL becomes "".
 * @kind: instrument class; an invalid value yields NULL with a critical.
 * @currency: (nullable): ISO 4217 code (copied); NULL becomes "USD".
 *
 * price, change and change_pct start as NAN; as_of_ms as 0; is_realtime and
 * market_open as FALSE.
 *
 * Preconditions:
 *  - @symbol satisfies stk_symbol_is_valid() (checked: critical and NULL
 *    otherwise).
 *  - @kind is a valid StkKind (checked: critical and NULL otherwise).
 *  - @name and @currency are NULL or NUL-terminated strings (GLib-owned or
 *    otherwise validated text; NOT a raw network buffer) whose length is
 *    bounded by the caller. The core does not cap, truncate or validate them:
 *    it duplicates them with g_strdup(), whose cost is proportional to the
 *    length and which reads up to the NUL. The real callers: the Yahoo parser
 *    passes a name only if name_is_usable() (valid UTF-8, no control
 *    characters, at most 256 bytes) and a currency only if it is exactly 3
 *    upper-case ASCII letters; the Nasdaq parser passes a currency of exactly
 *    3 upper-case ASCII letters and the "companyName" string only if it is
 *    non-empty, at most 256 bytes, valid UTF-8 and free of control characters
 *    (otherwise it passes NULL); the cache deserialiser passes a name of at most
 *    STK_CACHE_MAX_NAME_BYTES (256) valid UTF-8 bytes and a currency of 1..8
 *    ASCII letters/digits. Both strings come from json-glib, which
 *    NUL-terminates them.
 * Postconditions:
 *  - on success: all three strings are private copies owned by the quote; the
 *    symbol is ASCII-upper-cased; the numeric fields hold the defaults above.
 *  - on failure: nothing is allocated.
 * Thread affinity: any thread (the new quote is private to the caller).
 * Ownership: the caller owns the returned quote; the arguments are copied and
 *   stay owned by the caller.
 *
 * Returns: (transfer full): new quote, or NULL on invalid input; release with
 *   stk_quote_free().
 */
StkQuote *stk_quote_new(const char *symbol, const char *name, StkKind kind,
                        const char *currency);

/**
 * stk_quote_copy:
 * @quote: non-NULL quote.
 *
 * Deep-copies a quote.
 *
 * Preconditions:
 *  - @quote is non-NULL (checked: critical and NULL). Its `kind` is copied
 *    verbatim (any value, not validated); its string fields are NULL or
 *    NUL-terminated (a NULL field is copied as NULL).
 * Postconditions:
 *  - the copy has equal numeric fields and its own heap copies of symbol, name
 *    and currency; @quote is unchanged.
 * Thread affinity: any thread (reads @quote; the caller must not mutate it
 *   concurrently).
 * Ownership: the caller owns the returned quote; @quote stays with its owner.
 *
 * Returns: (transfer full): deep copy, release with stk_quote_free(); NULL if @quote is NULL.
 */
StkQuote *stk_quote_copy(const StkQuote *quote);

/**
 * stk_quote_free:
 * @quote: (nullable) (transfer full): quote to free, or NULL.
 *
 * Frees @quote and its strings. NULL is allowed (no-op).
 *
 * Preconditions:
 *  - @quote is NULL, or a quote whose strings and struct were heap-allocated
 *    by stk_quote_new()/stk_quote_copy() (or by equivalent g_malloc/g_strdup
 *    code) and that nobody else will use afterwards.
 * Postconditions:
 *  - the struct and its symbol, name and currency strings are freed; @quote
 *    is dangling.
 * Thread affinity: any thread.
 * Ownership: consumes @quote.
 */
void stk_quote_free(StkQuote *quote);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StkQuote, stk_quote_free)

G_END_DECLS

#endif /* STK_TYPES_H */
