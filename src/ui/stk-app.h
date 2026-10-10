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
 * "changed" signal that the start-up load emits on the watchlist is emitted
 * on that worker thread: the application connects its own handler only after
 * the load has finished, so none of its handlers runs on the worker. The
 * application itself issues no synchronous file or socket operation on the
 * main thread (the toolkit's and GIO's own initialisation is outside this).
 * Every function below must be called from the main thread, the thread that
 * runs the default main context.
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
 * no wakeup; StkApp itself arms only two other timers: the one-shot save debounce and the
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
 * selected symbol (the only inputs that change the mark of a symbol that keeps its item).
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
/** A save is started this long after the first unsaved change (debounce). */
#define STK_APP_SAVE_DELAY_MS 3000
/** Upper bound of the wait for pending saves and cancelled requests at exit. */
#define STK_APP_SHUTDOWN_WAIT_MS 5000

/*
 * G_DECLARE_FINAL_TYPE() declares: the StkApp typedef (instance struct
 * _StkApp, defined in stk-app.c), the StkAppClass typedef (a struct holding the
 * GtkApplicationClass), GType stk_app_get_type(void), the static inline cast STK_APP()
 * and check STK_IS_APP(), and g_autoptr() support for StkApp and StkAppClass.
 * It does NOT declare STK_APP_CLASS(), STK_IS_APP_CLASS() or STK_APP_GET_CLASS() (those
 * belong to G_DECLARE_DERIVABLE_TYPE()). STK_TYPE_APP below is hand-written.
 * stk_app_get_type() is thread-safe (GType registration is serialised).
 */
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
 * Creates the application object; nothing starts until g_application_run().
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - the object has its provider, watchlist, cache, item model and scheduler
 *    (all empty, nothing read from disk, no request made); no timer exists.
 * Thread affinity: main thread.
 * Ownership: the caller owns the returned reference.
 *
 * Returns: (transfer full): the application (id STK_APP_ID, default flags).
 *   Run it with g_application_run().
 */
StkApp *stk_app_new(void);

/**
 * stk_app_get_items:
 * @self: (not nullable): the application; must satisfy STK_IS_APP().
 *
 * Preconditions:
 *  - @self satisfies STK_IS_APP() (else: critical, NULL returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread.
 * Ownership: the model is owned by @self; no reference is added.
 *
 * Returns: (transfer none): the GListModel of StkRowItem, empty until the
 *   start-up load has finished.
 */
GListModel *stk_app_get_items(StkApp *self);

/**
 * stk_app_get_watchlist:
 * @self: (not nullable): the application; must satisfy STK_IS_APP().
 *
 * Preconditions:
 *  - @self satisfies STK_IS_APP() (else: critical, NULL returned).
 *  - the caller does not use the list before the start-up load has finished
 *    (the load runs on a worker thread that owns the list until then; the
 *    windows, the only callers, are created after it).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread.
 * Ownership: the list is owned by @self; no reference is added.
 *
 * Returns: (transfer none): the watchlist. Edit it only with its own API from
 *   the main thread; the application reacts to "changed".
 */
StkWatchlist *stk_app_get_watchlist(StkApp *self);

/**
 * stk_app_get_provider:
 * @self: (not nullable): the application; must satisfy STK_IS_APP().
 *
 * Preconditions:
 *  - @self satisfies STK_IS_APP() (else: critical, NULL returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread.
 * Ownership: the provider is owned by @self; no reference is added.
 *
 * Returns: (transfer none): the provider.
 */
StkProvider *stk_app_get_provider(StkApp *self);

/**
 * stk_app_get_catalog:
 * @self: (not nullable): the application; must satisfy STK_IS_APP().
 *
 * Loads the compiled-in catalog on first use (memory only, no I/O).
 *
 * Preconditions:
 *  - @self satisfies STK_IS_APP() (else: critical, NULL returned).
 * Postconditions:
 *  - the first call loads the catalog; a failure is logged once with
 *    g_warning() and remembered, so later calls return NULL without retrying.
 * Thread affinity: main thread.
 * Ownership: the catalog is owned by @self (freed in its finalize); the
 *   caller must not free it.
 *
 * Returns: (transfer none) (nullable): the catalog, valid until the
 *   application is finalized; NULL (after a g_warning()) if the compiled-in
 *   resource is unusable.
 */
const StkCatalog *stk_app_get_catalog(StkApp *self);

/**
 * stk_app_get_offline:
 * @self: (not nullable): the application; must satisfy STK_IS_APP().
 *
 * Preconditions:
 *  - @self satisfies STK_IS_APP() (else: critical, FALSE returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread.
 * Ownership: none (no pointers).
 *
 * Returns: TRUE while the network is unavailable (property "offline").
 */
gboolean stk_app_get_offline(StkApp *self);

/**
 * stk_app_set_window_visible:
 * @self: (not nullable): the application; must satisfy STK_IS_APP().
 * @visible: whether the main window is mapped and not minimised or suspended.
 *
 * Tells the scheduler; becoming visible triggers the refresh described in
 * stk-scheduler.h, becoming invisible cancels the requests in flight.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_APP() (else: critical, nothing happens).
 * Postconditions:
 *  - nothing happens once the application is shutting down; otherwise the
 *    scheduler has been told and the actions it produced have been executed
 *    (requests started or cancelled, the refresh timer re-armed).
 * Thread affinity: main thread.
 * Ownership: none (no pointers besides @self).
 */
void stk_app_set_window_visible(StkApp *self, gboolean visible);

/**
 * stk_app_window_focused:
 * @self: (not nullable): the application; must satisfy STK_IS_APP().
 *
 * The main window became the active window (focus regained).
 *
 * Preconditions:
 *  - @self satisfies STK_IS_APP() (else: critical, nothing happens).
 * Postconditions:
 *  - nothing happens once the application is shutting down; otherwise the
 *    scheduler has been told (it starts an event round only while the window
 *    is visible and the network is available) and its actions executed.
 * Thread affinity: main thread.
 * Ownership: none (no pointers besides @self).
 */
void stk_app_window_focused(StkApp *self);

/**
 * stk_app_select:
 * @self: (not nullable): the application; must satisfy STK_IS_APP().
 * @symbol: (nullable): the symbol of an item of stk_app_get_items() (as
 *   returned by stk_row_item_get_symbol()), or NULL for no selection. An
 *   unknown or invalid symbol counts as none.
 * @range: the chart range now shown.
 *
 * Declares which item the detail pane shows. The selected item receives the
 * cached series of @range at once (or none) and the scheduler is told; the
 * previously selected item drops its detail series. The stale flags of both
 * items are re-read from the scheduler. Does nothing once the application
 * is shutting down.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_APP() and @range is STK_RANGE_1D..STK_RANGE_1Y
 *    (else: critical, nothing happens); @symbol is NULL or NUL-terminated.
 * Postconditions:
 *  - the application remembers the item's own symbol (a copy) and @range as
 *    the selection, or no symbol when @symbol matches no item; @symbol is not
 *    retained. Nothing at all happens while shutting down.
 * Thread affinity: main thread.
 * Ownership: @symbol is borrowed for the call only.
 */
void stk_app_select(StkApp *self, const char *symbol, StkRange range);

G_END_DECLS

#endif /* STK_APP_H */
