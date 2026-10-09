/*
 * stk-provider.h - StkProvider: the only network surface of the stocks app.
 *
 * A GObject interface with three asynchronous GIO-style operations (quote,
 * chart series, symbol search). Every other module obtains market data
 * exclusively through this interface; StkNasdaq (stk-nasdaq.h) is its only
 * implementation, so the unofficial data source can be swapped by writing
 * another implementation.
 *
 * Contract shared by all operations
 * ---------------------------------
 *  - Call the *_async functions from the thread that runs the GLib main
 *    context (normally the main thread). They never block: all I/O is
 *    asynchronous and the callback runs in the thread-default main context
 *    that was current at the call.
 *  - The callback is never invoked before the *_async function has returned,
 *    even when the arguments are rejected immediately.
 *  - Invalid arguments are reported through the callback (not as a
 *    programming-error critical) with STK_ERROR_INVALID_ARG, without any
 *    network traffic. Symbols may come from disk or the network, so they are
 *    validated with stk_symbol_is_valid() by the implementation.
 *  - Failures are reported in the STK_ERROR domain (STK_ERROR_NETWORK,
 *    _TIMEOUT, _HTTP, _RATE_LIMITED, _PARSE, _NOT_FOUND, _INVALID_ARG).
 *    Cancellation is the only exception: it is reported as
 *    G_IO_ERROR / G_IO_ERROR_CANCELLED. If @cancellable is cancelled before
 *    the *_finish function is called, *_finish reports G_IO_ERROR_CANCELLED
 *    even if the operation had already succeeded. An implementation may
 *    refuse new work while too many operations are outstanding: it then
 *    reports STK_ERROR_RATE_LIMITED through the callback without network
 *    traffic (StkNasdaq: STK_NASDAQ_MAX_INFLIGHT requests).
 *  - The caller owns the results (transfer full) and the provider keeps no
 *    reference to them.
 *
 * Semantics of the returned data (implementation independent)
 * -----------------------------------------------------------
 *  - StkQuote: price/change/change_pct describe the latest REGULAR-session
 *    price (the last official price for funds); unknown numeric fields are
 *    NAN. quote->kind equals the requested kind and quote->symbol equals the
 *    requested symbol (upper-cased), never a string taken from the response.
 *  - StkSeries for STK_RANGE_1D (stocks and ETFs only): one point per
 *    provider sample inside the regular session of the trading day the
 *    provider reports, t_ms being the true UTC instant; the session window
 *    is half-open [open, close) because the provider labels each minute
 *    sample by its START time (the sample labelled with the closing time is
 *    the first after-hours minute and is not returned). prev_close is the
 *    previous session's close, derived from the same provider's quote as
 *    quote price - quote change (see stk_nasdaq.h for why); NAN when that is
 *    unavailable. A failing quote lookup never fails the series.
 *  - StkSeries for STK_RANGE_1M/3M/6M/1Y: one point per trading day, valid
 *    for every kind; t_ms is 00:00 UTC of the trading day's calendar date
 *    (America/New_York date) and is NOT the close instant; prev_close is NAN.
 *    The window ends on today's New York date and starts 1, 3, 6 or 12
 *    calendar months earlier.
 *  - A series with zero points is a valid ("no data") result.
 */
#ifndef STK_PROVIDER_H
#define STK_PROVIDER_H

#include <gio/gio.h>

#include "stk-types.h"

G_BEGIN_DECLS

/**
 * StkSearchHit: one symbol-search result. Owns its strings.
 *
 * @symbol satisfies stk_symbol_is_valid() and is ASCII upper-case; @name is
 * the display name ("" when unknown); @kind is the instrument class the
 * provider reported for the hit (hits of any other class are not returned).
 */
typedef struct {
  char *symbol;
  char *name;
  StkKind kind;
} StkSearchHit;

/** Frees @hit and its strings. NULL is allowed (no-op). */
void stk_search_hit_free(StkSearchHit *hit);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StkSearchHit, stk_search_hit_free)

#define STK_TYPE_PROVIDER (stk_provider_get_type())
G_DECLARE_INTERFACE(StkProvider, stk_provider, STK, PROVIDER, GObject)

/**
 * StkProviderInterface:
 * @fetch_quote_async: see stk_provider_fetch_quote_async().
 * @fetch_quote_finish: see stk_provider_fetch_quote_finish().
 * @fetch_series_async: see stk_provider_fetch_series_async().
 * @fetch_series_finish: see stk_provider_fetch_series_finish().
 * @search_async: see stk_provider_search_async().
 * @search_finish: see stk_provider_search_finish().
 *
 * Every virtual function is mandatory.
 */
struct _StkProviderInterface {
  GTypeInterface parent_iface;

  void (*fetch_quote_async)(StkProvider *self, const char *symbol, StkKind kind,
                            GCancellable *cancellable, GAsyncReadyCallback callback,
                            gpointer user_data);
  StkQuote *(*fetch_quote_finish)(StkProvider *self, GAsyncResult *result, GError **error);

  void (*fetch_series_async)(StkProvider *self, const char *symbol, StkKind kind,
                             StkRange range, GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data);
  StkSeries *(*fetch_series_finish)(StkProvider *self, GAsyncResult *result, GError **error);

  void (*search_async)(StkProvider *self, const char *text, GCancellable *cancellable,
                       GAsyncReadyCallback callback, gpointer user_data);
  GPtrArray *(*search_finish)(StkProvider *self, GAsyncResult *result, GError **error);
};

/**
 * stk_provider_fetch_quote_async:
 * @self: a StkProvider.
 * @symbol: non-NULL ticker; it must satisfy stk_symbol_is_valid() (else
 *   STK_ERROR_INVALID_ARG is reported). Copied.
 * @kind: the instrument's stored class. It alone selects the provider's
 *   asset class; it is never guessed. An invalid value is reported as
 *   STK_ERROR_INVALID_ARG.
 * @cancellable: (nullable): cancels the operation.
 * @callback: invoked once, in the thread-default main context of the caller.
 * @user_data: passed to @callback.
 *
 * Starts fetching the current quote of @symbol.
 */
void stk_provider_fetch_quote_async(StkProvider *self, const char *symbol, StkKind kind,
                                    GCancellable *cancellable, GAsyncReadyCallback callback,
                                    gpointer user_data);

/**
 * stk_provider_fetch_quote_finish:
 * @self: the provider passed to the async call.
 * @result: the GAsyncResult given to the callback.
 * @error: (nullable): return location for a GError.
 *
 * Returns: (transfer full): the quote (release with stk_quote_free()), or
 *   NULL with @error set.
 */
StkQuote *stk_provider_fetch_quote_finish(StkProvider *self, GAsyncResult *result,
                                          GError **error);

/**
 * stk_provider_fetch_series_async:
 * @self: a StkProvider.
 * @symbol: non-NULL ticker; must satisfy stk_symbol_is_valid().
 * @kind: the instrument's stored class (see stk_provider_fetch_quote_async()).
 * @range: chart range. STK_RANGE_1D is only valid for STK_KIND_STOCK and
 *   STK_KIND_ETF: STK_KIND_FUND with STK_RANGE_1D, and an invalid range
 *   value, are reported as STK_ERROR_INVALID_ARG without network traffic.
 * @cancellable: (nullable): cancels the operation.
 * @callback: invoked once, in the thread-default main context of the caller.
 * @user_data: passed to @callback.
 *
 * Starts fetching the chart series of @symbol for @range.
 */
void stk_provider_fetch_series_async(StkProvider *self, const char *symbol, StkKind kind,
                                     StkRange range, GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer user_data);

/**
 * stk_provider_fetch_series_finish:
 * @self: the provider passed to the async call.
 * @result: the GAsyncResult given to the callback.
 * @error: (nullable): return location for a GError.
 *
 * Returns: (transfer full): the series (release with stk_series_unref());
 *   series->range equals the requested range. NULL with @error set on
 *   failure.
 */
StkSeries *stk_provider_fetch_series_finish(StkProvider *self, GAsyncResult *result,
                                            GError **error);

/**
 * stk_provider_search_async:
 * @self: a StkProvider.
 * @text: non-NULL free text typed by the user. Leading/trailing ASCII
 *   whitespace is ignored; after that it must be 1..64 bytes of valid UTF-8
 *   without control characters, else STK_ERROR_INVALID_ARG is reported.
 *   Copied.
 * @cancellable: (nullable): cancels the operation.
 * @callback: invoked once, in the thread-default main context of the caller.
 * @user_data: passed to @callback.
 *
 * Starts a symbol/name search.
 */
void stk_provider_search_async(StkProvider *self, const char *text, GCancellable *cancellable,
                               GAsyncReadyCallback callback, gpointer user_data);

/**
 * stk_provider_search_finish:
 * @self: the provider passed to the async call.
 * @result: the GAsyncResult given to the callback.
 * @error: (nullable): return location for a GError.
 *
 * Returns: (transfer full) (element-type StkSearchHit): array of hits in the
 *   provider's order (possibly empty), each freed by the array
 *   (g_ptr_array_unref()); NULL with @error set on failure. Only hits whose
 *   class is stock, ETF or mutual fund and whose symbol satisfies
 *   stk_symbol_is_valid() are included.
 */
GPtrArray *stk_provider_search_finish(StkProvider *self, GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* STK_PROVIDER_H */
