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
  STK_ERROR_HTTP,         /**< unexpected HTTP status (not 404 / 429)         */
  STK_ERROR_RATE_LIMITED, /**< HTTP 429 or provider-signalled throttling      */
  STK_ERROR_PARSE,        /**< response/text did not match the expected shape */
  STK_ERROR_NOT_FOUND,    /**< unknown symbol / HTTP 404                      */
  STK_ERROR_INVALID_ARG   /**< caller passed an argument the API rejects      */
} StkError;

/** Returns the quark of the STK_ERROR domain ("stk-error-quark"). */
GQuark stk_error_quark(void);

/* ------------------------------------------------------------------------ */
/* Enumerations                                                             */
/* ------------------------------------------------------------------------ */

/**
 * Instrument class. Selects the provider's `assetclass` (never guessed).
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
 * Returns: TRUE if @str is exactly one of the tokens (case-sensitive).
 */
gboolean stk_kind_from_string(const char *str, StkKind *out);

/**
 * stk_range_to_string:
 * @range: a valid StkRange.
 *
 * Returns: static token: "1D", "1M", "3M", "6M" or "1Y". Never NULL.
 */
const char *stk_range_to_string(StkRange range);

/**
 * stk_range_from_string:
 * @str: (nullable): token as returned by stk_range_to_string().
 * @out: (out): receives the range on success; untouched on failure.
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
 * Returns: (transfer full): new empty series (n_points == 0) with one
 *   reference, or NULL (with a critical) if @range is not a valid StkRange.
 *   Release with stk_series_unref() (or g_autoptr(StkSeries)).
 */
StkSeries *stk_series_new(StkRange range, double prev_close, gsize capacity_hint);

/**
 * stk_series_ref:
 * @series: non-NULL series.
 *
 * Returns: (transfer full): @series with its reference count incremented.
 */
StkSeries *stk_series_ref(StkSeries *series);

/**
 * stk_series_unref:
 * @series: non-NULL series.
 *
 * Drops one reference; frees the series when the last one goes away.
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
 * Returns: (transfer full): new quote, or NULL on invalid input; release with
 *   stk_quote_free().
 */
StkQuote *stk_quote_new(const char *symbol, const char *name, StkKind kind,
                        const char *currency);

/**
 * stk_quote_copy:
 * @quote: non-NULL quote.
 *
 * Returns: (transfer full): deep copy, release with stk_quote_free().
 */
StkQuote *stk_quote_copy(const StkQuote *quote);

/** Frees @quote and its strings. NULL is allowed (no-op). */
void stk_quote_free(StkQuote *quote);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StkQuote, stk_quote_free)

G_END_DECLS

#endif /* STK_TYPES_H */
