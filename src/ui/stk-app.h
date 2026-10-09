/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-app.h - StkApp: the GtkApplication of the stocks app and the owner of
 * its data: the StkProvider (network), the StkWatchlist and StkCache
 * (persistence), the StkCatalog, the list of StkRowItem shown by the window
 * and the StkScheduler that decides when to fetch.
 *
 * Threading: the application's own state is main-thread only. Worker threads
 * exist for file I/O only: a GTask worker reads the two files once at
 * start-up (until it has finished, see "Start-up", nothing else touches the
 * watchlist and the cache), and every later save runs on a worker thread as
 * well: stk_watchlist_save_async() / stk_cache_save_async() take a snapshot on
 * the main thread and serialise and write it on a worker (at most one save of
 * each at a time; the completion callback runs on the main thread). The
 * application itself issues no synchronous file or socket operation on the
 * main thread (the toolkit's and GIO's own initialisation is outside this).
 *
 * Start-up
 * --------
 * startup: the CSS is installed and the watchlist and the cache are loaded on
 * a worker thread (the application holds itself alive meanwhile). activate:
 * the main window is created and presented as soon as the load finished, so
 * its first frame already shows the cached rows (quotes and sparklines from
 * the cache); no network access precedes it. A file that is missing is
 * normal (first run); a file that cannot be read or parsed is logged with
 * g_warning() and treated as empty.
 *
 * Refreshing
 * ----------
 * The scheduler (stk-scheduler.h) is fed with the watchlist, the window's
 * visibility and focus, the selection and the network state
 * (GNetworkMonitor); its actions are executed here: a quote or series request
 * to the provider with a GCancellable per request, cancelled on the
 * scheduler's CANCEL actions. The refresh timer is at most one source, armed
 * for the scheduler's next wakeup (a periodic round while the market is open,
 * or the one final round after the close) and absent while the scheduler has
 * no wakeup; the only other timers are the one-shot save debounce and the
 * shutdown wait. A result updates its
 * StkRowItem only if the data differs from what the item holds, and is stored
 * in the cache only in that case (a series that nothing shows, e.g. of a range
 * deselected meanwhile, is always stored). One exception: a 1D series without
 * a point (the provider has none of the new session before the open) never
 * replaces a non-empty 1D series, neither on screen nor in the cache, and does
 * not mark the symbol stale (see stk_sched_series_is_hollow()): the last
 * session stays on screen and on disk; only when no non-empty 1D series is
 * held does the empty one replace it ("No data"). The detail header
 * caption comes from the quote, not from the series, so it keeps naming the
 * date of the quote's last trade. A symbol removed while a request is
 * in flight has the late result dropped. A failed request keeps the cached
 * data and marks the item stale. The stale flag of every item mirrors
 * stk_scheduler_is_stale(): it is re-read for the symbol of each finished
 * request and, after a selection change, for the previously and the newly
 * selected symbol (the only inputs that change a mark).
 *
 * Persistence
 * -----------
 * Edits of the watchlist (by the picker or the row actions) and cache updates
 * are saved asynchronously at most once per STK_APP_SAVE_DELAY_MS (a
 * one-shot timer armed by the first change). At shutdown everything dirty is
 * saved and the application waits (running a nested main loop, at most
 * STK_APP_SHUTDOWN_WAIT_MS) for the saves and the cancelled requests to
 * finish before it returns from "shutdown". Requests and saves that have not
 * finished by then (a hung network or disk) are abandoned: they hold no
 * reference to the application, so it is finalized normally, and their late
 * completion only frees them.
 *
 * Items
 * -----
 * stk_app_get_items() is a GListModel of StkRowItem in watchlist order. It
 * follows the watchlist: whatever changes it (picker, row actions) is applied
 * to the model with a single items-changed emission that keeps the items of
 * unchanged symbols (and with them the list view's selection) alive.
 */
#ifndef STK_APP_H
#define STK_APP_H

#include <gtk/gtk.h>

#include "stk-catalog.h"
#include "stk-provider.h"
#include "stk-types.h"
#include "stk-watchlist.h"

G_BEGIN_DECLS

#define STK_APP_ID "io.github.wtp128pro.Stocks"
/** Changes are written at most this long after the first unsaved change. */
#define STK_APP_SAVE_DELAY_MS 3000
/** Upper bound of the wait for pending saves and cancelled requests at exit. */
#define STK_APP_SHUTDOWN_WAIT_MS 5000

#define STK_TYPE_APP (stk_app_get_type())
G_DECLARE_FINAL_TYPE(StkApp, stk_app, STK, APP, GtkApplication)

/**
 * StkApp:offline:
 *
 * (readable, notify) TRUE while the network monitor reports no network.
 */

/**
 * stk_app_new:
 *
 * Returns: (transfer full): the application (id STK_APP_ID, default flags).
 *   Run it with g_application_run().
 */
StkApp *stk_app_new(void);

/**
 * stk_app_get_items:
 * @self: the application.
 *
 * Returns: (transfer none): the GListModel of StkRowItem, empty until the
 *   start-up load has finished.
 */
GListModel *stk_app_get_items(StkApp *self);

/**
 * stk_app_get_watchlist:
 * @self: the application.
 *
 * Returns: (transfer none): the watchlist. Edit it only with its own API from
 *   the main thread; the application reacts to "changed".
 */
StkWatchlist *stk_app_get_watchlist(StkApp *self);

/** Returns: (transfer none): the provider. */
StkProvider *stk_app_get_provider(StkApp *self);

/**
 * stk_app_get_catalog:
 * @self: the application.
 *
 * Loads the compiled-in catalog on first use (memory only, no I/O).
 *
 * Returns: (transfer none) (nullable): the catalog, valid until the
 *   application is finalized; NULL (after a g_warning()) if the compiled-in
 *   resource is unusable.
 */
const StkCatalog *stk_app_get_catalog(StkApp *self);

/** Returns: TRUE while the network is unavailable (property "offline"). */
gboolean stk_app_get_offline(StkApp *self);

/**
 * stk_app_set_window_visible:
 * @self: the application.
 * @visible: whether the main window is mapped and not minimised or suspended.
 *
 * Tells the scheduler; becoming visible triggers the refresh described in
 * stk-scheduler.h, becoming invisible cancels the requests in flight.
 */
void stk_app_set_window_visible(StkApp *self, gboolean visible);

/** The main window became the active window (focus regained). */
void stk_app_window_focused(StkApp *self);

/**
 * stk_app_select:
 * @self: the application.
 * @symbol: (nullable): the symbol of an item of stk_app_get_items() (as
 *   returned by stk_row_item_get_symbol()), or NULL for no selection. An
 *   unknown or invalid symbol counts as none.
 * @range: the chart range now shown.
 *
 * Declares which item the detail pane shows. The selected item receives the
 * cached series of @range at once (or none) and a fetch is scheduled; the
 * previously selected item drops its detail series. The stale flags of both
 * items are re-read from the scheduler. Does nothing once the application
 * is shutting down.
 */
void stk_app_select(StkApp *self, const char *symbol, StkRange range);

G_END_DECLS

#endif /* STK_APP_H */
