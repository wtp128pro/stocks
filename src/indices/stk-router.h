/*
 * stk-router.h - StkRouter: a StkProvider that sends index symbols to one
 * provider (StkYahoo in the app) and everything else to another (StkNasdaq).
 *
 * Routing is by symbol only, never by guessing the kind:
 *   - fetch_quote / fetch_series: a symbol whose first byte is '^' goes to
 *     the index provider, every other symbol (including "" and invalid ones)
 *     to the primary provider. The kind is passed through unchanged, so the
 *     inner providers do the rejecting: StkYahoo rejects every kind other
 *     than STK_KIND_INDEX and StkNasdaq rejects STK_KIND_INDEX, both with
 *     STK_ERROR_INVALID_ARG and without network traffic ("^GSPC" as a stock
 *     and "AAPL" as an index are therefore errors, not misrouted).
 *   - search: always the primary provider, with the text and cancellable
 *     unchanged. The router has no list of indices and never calls the index
 *     provider's search.
 *
 * Each operation is wrapped in a GTask of the router that uses the caller's
 * cancellable: the callback's source object is the router, the result is
 * finished with the router, cancellation is passed to the inner operation
 * and results are those of the inner provider (same errors, same data,
 * ownership transferred to the caller). The callback is never invoked before
 * the *_async function returned and every operation completes exactly once.
 * Pacing, retries and in-flight caps are the inner providers' business.
 *
 * All functions must be used from the thread that runs the GLib main context
 * (normally the main thread). Operations keep the router and its inner
 * providers alive until they complete.
 */
#ifndef STK_ROUTER_H
#define STK_ROUTER_H

#include <gio/gio.h>

#include "stk-provider.h"

G_BEGIN_DECLS

#define STK_TYPE_ROUTER (stk_router_get_type())
G_DECLARE_FINAL_TYPE(StkRouter, stk_router, STK, ROUTER, GObject)

/**
 * stk_router_new:
 * @primary: a StkProvider for every non-'^' symbol and for search (StkNasdaq
 *   in the app). A reference is taken.
 * @index_provider: a StkProvider for '^' symbols (StkYahoo in the app). A
 *   reference is taken. It may be the same object as @primary.
 *
 * Returns: (transfer full): a StkProvider routing as described above.
 */
StkRouter *stk_router_new(StkProvider *primary, StkProvider *index_provider);

G_END_DECLS

#endif /* STK_ROUTER_H */
