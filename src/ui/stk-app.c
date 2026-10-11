/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/* stk-app.c - StkApp: data ownership, refresh execution, persistence. */
#include "stk-app.h"

#include <string.h>

#include "stk-cache.h"
#include "stk-nasdaq.h"
#include "stk-router.h"
#include "stk-row.h"
#include "stk-scheduler.h"
#include "stk-ui-resources.h"
#include "stk-window.h"
#include "stk-yahoo.h"

#define CSS_RESOURCE_PATH "/io/github/wtp128pro/Stocks/ui/style.css"

/* One in-flight provider request. Ownership: it is allocated by run_action()
 * and belongs to the completion callback of its operation (quote_done() or
 * series_done()), which frees it exactly once with request_free() (directly,
 * or through finish_request()). StkApp.requests only indexes it by id while it
 * is in flight and never frees it. Its GWeakRef keeps it from holding the
 * application alive. */
typedef struct {
  GWeakRef app; /* weak: a pending request must never keep the application alive */
  guint64 id;
  GCancellable *cancellable;
  char symbol[STK_SCHED_SYMBOL_BYTES];
  StkRange range; /* series requests */
} Request;

/* run_action() copies the scheduler's symbol with g_strlcpy() into Request.symbol; the copy is
 * whole because the scheduler's symbol is NUL-terminated within
 * STK_SCHED_SYMBOL_BYTES bytes and the destination is at least that large. */
G_STATIC_ASSERT(sizeof(((Request *)NULL)->symbol) >= STK_SCHED_SYMBOL_BYTES);

struct _StkApp {
  GtkApplication parent_instance;

  StkProvider *provider;
  StkWatchlist *watchlist;
  StkCache *cache;
  StkCatalog *catalog;
  gboolean catalog_failed;

  GListStore *items;      /* StkRowItem, watchlist order */
  GHashTable *by_symbol;  /* char* (owned) -> StkRowItem* (strong) */
  StkScheduler *sched;
  GHashTable *requests;   /* guint64* (== &Request.id) -> Request*, in flight; does not own: a
                           * Request belongs to the callback of its operation */
  guint wake_id;          /* the scheduler's single timer, 0 = none */
  gint64 wake_us;         /* the deadline wake_id was armed for */

  GNetworkMonitor *netmon;
  gulong netmon_id;
  gboolean offline;

  gboolean loading;          /* the start-up load task is running */
  gboolean loaded;
  gboolean activate_pending; /* activate arrived before the load finished */
  gboolean shutting_down;
  gulong watchlist_changed_id;

  char *sel_symbol;
  StkRange sel_range;

  guint save_id;
  gboolean saving_watchlist;
  gboolean saving_cache;

  GMainLoop *drain_loop;
  guint drain_id;
};

/*
 * G_DEFINE_FINAL_TYPE() defines: stk_app_get_type(), the static
 * stk_app_parent_class pointer and the static class initialiser that runs
 * stk_app_class_init() once; stk_app_init() is registered as the instance
 * initialiser; the instance struct is _StkApp above.
 */
G_DEFINE_FINAL_TYPE(StkApp, stk_app, GTK_TYPE_APPLICATION)

enum { PROP_0, PROP_OFFLINE, N_PROPS };

/**
 * sched_now:
 *
 * Reads both clocks for a scheduler call.
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (reads the clocks only); called on the main
 *   thread.
 * Ownership: none (no pointers).
 *
 * Returns: the monotonic time in microseconds and the wall time in
 *   milliseconds since the Unix epoch.
 */
static StkSchedTime
sched_now(void)
{
  StkSchedTime t = {g_get_monotonic_time(), g_get_real_time() / 1000};
  return t;
}

/**
 * lookup_item:
 * @self: (not nullable): the application.
 * @symbol: (nullable): an item symbol, or NULL.
 *
 * Preconditions:
 *  - @self is a valid application; @symbol is NULL or NUL-terminated (every
 *    caller passes a GLib-owned string, a Request.symbol that run_action()
 *    terminated, or the symbol given to stk_app_select()).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread.
 * Ownership: @symbol is borrowed; the item stays owned by @self.
 *
 * Returns: (transfer none) (nullable): the item of that symbol, or NULL when
 *   @symbol is NULL or has no item.
 */
static StkRowItem *
lookup_item(StkApp *self, const char *symbol)
{
  return symbol != NULL ? g_hash_table_lookup(self->by_symbol, symbol) : NULL;
}

/* ------------------------------------------------------------------------ */
/* Persistence                                                              */
/* ------------------------------------------------------------------------ */

/**
 * pending_io:
 * @self: (not nullable): the application.
 *
 * Preconditions:
 *  - @self is a valid application whose request table exists (between init and
 *    finalize).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread.
 * Ownership: none; @self is only read.
 *
 * Returns: TRUE while the start-up load, a save or a request is still pending.
 */
static gboolean
pending_io(StkApp *self)
{
  return self->loading || self->saving_watchlist || self->saving_cache ||
         g_hash_table_size(self->requests) > 0;
}

/**
 * drain_progress:
 * @self: (not nullable): the application.
 *
 * During the shutdown wait: leave the nested loop once nothing is pending.
 *
 * Preconditions:
 *  - @self is a valid application.
 * Postconditions:
 *  - if the shutdown wait loop is running and pending_io() is FALSE, that loop
 *    has been asked to quit; otherwise nothing changes.
 * Thread affinity: main thread.
 * Ownership: none; the loop stays owned by stk_app_shutdown().
 */
static void
drain_progress(StkApp *self)
{
  if (self->drain_loop != NULL && !pending_io(self))
    g_main_loop_quit(self->drain_loop);
}

static void start_saves(StkApp *self);
static void schedule_save(StkApp *self);

/**
 * weak_app_new:
 * @self: (not nullable): the application.
 *
 * An operation (provider request, file save) can still be pending when the
 * application goes away after a forced shutdown (hung network or disk). Its
 * user_data therefore holds no strong reference to the application; the
 * completion callback takes one only while it runs and, when the application
 * is gone, just releases what the operation itself owns.
 *
 * Preconditions:
 *  - @self is a valid application.
 * Postconditions:
 *  - a weak reference to @self was created; @self's reference count is
 *    unchanged.
 * Thread affinity: main thread.
 * Ownership: @self is borrowed; the caller owns the returned box and must give
 *   it to weak_app_take() exactly once (the save callbacks do).
 *
 * Returns: (transfer full): a heap-allocated, initialised GWeakRef.
 */
static GWeakRef *
weak_app_new(StkApp *self)
{
  GWeakRef *ref = g_new(GWeakRef, 1);
  g_weak_ref_init(ref, self);
  return ref;
}

/**
 * weak_app_take:
 * @ref: (transfer full): a box from weak_app_new(), not used afterwards.
 *
 * Frees @ref and returns what it pointed to, if that still exists.
 *
 * Preconditions:
 *  - @ref was returned by weak_app_new() and has not been taken before.
 * Postconditions:
 *  - @ref is cleared and freed.
 * Thread affinity: main thread.
 * Ownership: consumes @ref.
 *
 * Returns: (transfer full) (nullable): a new strong reference to the
 *   application, or NULL if it is gone.
 */
static StkApp *
weak_app_take(GWeakRef *ref)
{
  StkApp *self = g_weak_ref_get(ref);
  g_weak_ref_clear(ref);
  g_free(ref);
  return self;
}

/**
 * watchlist_saved:
 * @source: (not nullable): the StkWatchlist the save ran on (kept alive by the
 *   save task until this callback returns).
 * @result: (not nullable): the result of stk_watchlist_save_async().
 * @user_data: (transfer full): the GWeakRef box made by weak_app_new() in
 *   start_saves().
 *
 * GAsyncReadyCallback of the watchlist save: logs a failure, and when the
 * application still exists, clears its saving flag, re-arms a save if the list
 * was edited while writing, and lets the shutdown wait finish.
 *
 * Preconditions:
 *  - called once, on the main thread, by stk_watchlist_save_async();
 *    @user_data is the box that start_saves() passed (consumed here).
 * Postconditions:
 *  - the box is freed. On a failed save the reason is logged with
 *    g_warning(); if the failure has no GError, the text "unknown error" is
 *    logged instead. If the application is gone nothing else happens;
 *    otherwise saving_watchlist is FALSE and, after a successful save of a
 *    list that was edited meanwhile, a new save is started (while shutting
 *    down) or scheduled (otherwise).
 * Thread affinity: main thread (the callback runs in the main context of the
 *   thread that called stk_watchlist_save_async()).
 * Ownership: the box in @user_data is consumed; @source and @result stay with
 *   the caller.
 */
static void
watchlist_saved(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(StkApp) self = weak_app_take(user_data);
  g_autoptr(GError) error = NULL;
  gboolean saved = stk_watchlist_save_finish(STK_WATCHLIST(source), result, &error);
  if (!saved)
    g_warning("Could not save the watchlist: %s",
              error != NULL ? error->message : "unknown error");
  if (self == NULL)
    return; /* the application is gone */
  self->saving_watchlist = FALSE;
  if (saved && stk_watchlist_is_dirty(self->watchlist)) /* edited while writing */
    self->shutting_down ? start_saves(self) : schedule_save(self);
  drain_progress(self);
}

/**
 * cache_saved:
 * @source: (not nullable): the StkCache the save ran on (kept alive by the
 *   save task until this callback returns).
 * @result: (not nullable): the result of stk_cache_save_async().
 * @user_data: (transfer full): the GWeakRef box made by weak_app_new() in
 *   start_saves().
 *
 * GAsyncReadyCallback of the cache save; same behaviour as watchlist_saved()
 * for the cache.
 *
 * Preconditions:
 *  - called once, on the main thread, by stk_cache_save_async(); @user_data is
 *    the box that start_saves() passed (consumed here).
 * Postconditions:
 *  - the box is freed. On a failed save the reason is logged with
 *    g_warning(); if the failure has no GError, the text "unknown error" is
 *    logged instead. If the application is gone nothing else happens;
 *    otherwise saving_cache is FALSE and, after a successful save of a cache
 *    that was changed meanwhile, a new save is started (while shutting down)
 *    or scheduled (otherwise).
 * Thread affinity: main thread (the callback runs in the main context of the
 *   thread that called stk_cache_save_async()).
 * Ownership: the box in @user_data is consumed; @source and @result stay with
 *   the caller.
 */
static void
cache_saved(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(StkApp) self = weak_app_take(user_data);
  g_autoptr(GError) error = NULL;
  gboolean saved = stk_cache_save_finish(STK_CACHE(source), result, &error);
  if (!saved)
    g_warning("Could not save the cache: %s", error != NULL ? error->message : "unknown error");
  if (self == NULL)
    return; /* the application is gone */
  self->saving_cache = FALSE;
  if (saved && stk_cache_is_dirty(self->cache))
    self->shutting_down ? start_saves(self) : schedule_save(self);
  drain_progress(self);
}

/**
 * start_saves:
 * @self: (not nullable): the application.
 *
 * Starts an asynchronous save of whatever is dirty and not already being saved.
 *
 * Preconditions:
 *  - @self is a valid application; its watchlist and cache are not being loaded
 *    on the worker thread (callers: save_timeout(), the save callbacks and
 *    stk_app_shutdown() only run after the load, which stk_app_shutdown()
 *    guarantees by testing "loaded").
 * Postconditions:
 *  - for each of the watchlist and the cache that is dirty and has no save
 *    running: its saving flag is TRUE and one save was started whose callback
 *    owns a new weak-reference box; nothing is started otherwise.
 * Thread affinity: main thread.
 * Ownership: each box passed as user_data is owned by the save callback.
 */
static void
start_saves(StkApp *self)
{
  if (stk_watchlist_is_dirty(self->watchlist) && !self->saving_watchlist) {
    self->saving_watchlist = TRUE;
    stk_watchlist_save_async(self->watchlist, NULL, watchlist_saved, weak_app_new(self));
  }
  if (stk_cache_is_dirty(self->cache) && !self->saving_cache) {
    self->saving_cache = TRUE;
    stk_cache_save_async(self->cache, NULL, cache_saved, weak_app_new(self));
  }
}

/**
 * save_timeout:
 * @user_data: (not nullable): the StkApp that armed the timer (borrowed raw
 *   pointer).
 *
 * GSourceFunc of the one-shot save debounce timer.
 *
 * Preconditions:
 *  - the source is alive only while @user_data is: stk_app_dispose() removes
 *    save_id before the application can be finalized, so the pointer cannot
 *    dangle.
 * Postconditions:
 *  - save_id is 0 and start_saves() ran; the source is removed.
 * Thread affinity: main thread (default main context).
 * Ownership: @user_data is borrowed.
 *
 * Returns: G_SOURCE_REMOVE always (one-shot).
 */
static gboolean
save_timeout(gpointer user_data)
{
  StkApp *self = STK_APP(user_data);
  self->save_id = 0;
  start_saves(self);
  return G_SOURCE_REMOVE;
}

/**
 * schedule_save:
 * @self: (not nullable): the application.
 *
 * Arms the one-shot save debounce timer unless it is armed or the application
 * is shutting down.
 *
 * Preconditions:
 *  - @self is a valid application.
 * Postconditions:
 *  - if save_id was 0 and the application is not shutting down: save_id is a
 *    live timer of STK_APP_SAVE_DELAY_MS that calls save_timeout() with @self;
 *    otherwise nothing changes.
 * Thread affinity: main thread.
 * Ownership: @self is passed to the timer as a borrowed pointer (see
 *   save_timeout()).
 */
static void
schedule_save(StkApp *self)
{
  if (self->save_id == 0 && !self->shutting_down)
    self->save_id = g_timeout_add(STK_APP_SAVE_DELAY_MS, save_timeout, self);
}

/* ------------------------------------------------------------------------ */
/* Items                                                                    */
/* ------------------------------------------------------------------------ */

/**
 * apply_cache:
 * @self: (not nullable): the application.
 * @item: (not nullable): an item of this application.
 *
 * Paints a new item from the cache: its quote and its sparkline series, when
 * the cache holds them.
 *
 * Preconditions:
 *  - @self and @item are valid; the cache is not being loaded on the worker.
 * Postconditions:
 *  - the item holds the cached quote and 1M series if any; the cache's
 *    recency order changed for a hit (it is not marked dirty).
 * Thread affinity: main thread.
 * Ownership: @item is borrowed; the copies taken from the cache are released.
 */
static void
apply_cache(StkApp *self, StkRowItem *item)
{
  const char *symbol = stk_row_item_get_symbol(item);
  g_autoptr(StkQuote) quote = stk_cache_get_quote(self->cache, symbol, NULL);
  if (quote != NULL)
    stk_row_item_set_quote(item, quote);
  g_autoptr(StkSeries) spark =
      stk_cache_get_series(self->cache, symbol, STK_SCHED_SPARK_RANGE, NULL);
  if (spark != NULL)
    stk_row_item_set_spark(item, spark);
}

/**
 * store_symbol_at:
 * @store: (not nullable): the item store.
 * @position: index of an item of @store.
 *
 * Preconditions:
 *  - @position < the number of items of @store (the only caller, sync_items(),
 *    keeps every index below old_n).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread.
 * Ownership: the item stays owned by @store; the returned string is owned by
 *   that item.
 *
 * Returns: (transfer none): the symbol of the item at @position, valid while
 *   the item stays in the store.
 */
static const char *
store_symbol_at(GListStore *store, guint position)
{
  /* The store keeps the item alive, so the borrowed string outlives the unref. */
  g_autoptr(StkRowItem) item = g_list_model_get_item(G_LIST_MODEL(store), position);
  return stk_row_item_get_symbol(item);
}

/**
 * on_item_view_changed:
 * @self: (not nullable): the application (the swapped-signal instance).
 * @item: (not nullable): the item that changed.
 *
 * Handler of every item's "changed" signal: when the item's chart range or points/percent choice
 * differs from what the watchlist holds for its symbol, stores it there and schedules a save.
 * Quote, sparkline and stale changes find the values equal and do nothing.
 *
 * Preconditions:
 *  - @self is a live application with a watchlist owned by the calling thread; @item is a live
 *    StkRowItem. The connection is made by sync_items() with g_signal_connect_object() on @self.
 * Postconditions:
 *  - the watchlist entry of the item's symbol (if it still exists) holds the item's view range
 *    and points flag; a save is scheduled iff that changed them.
 * Thread affinity: main thread.
 * Ownership: all parameters are borrowed for the call.
 */
static void
on_item_view_changed(StkApp *self, StkRowItem *item)
{
  if (stk_watchlist_set_view(self->watchlist, stk_row_item_get_symbol(item),
                             stk_row_item_get_view_range(item),
                             stk_row_item_get_show_points(item)))
    schedule_save(self);
}

/**
 * sync_items:
 * @self: (not nullable): the application.
 *
 * Brings the item model in line with the watchlist: new symbols get an item
 * (painted from the cache), and the changed middle part of the list is
 * replaced with one splice, so that unchanged items (and a selection on them)
 * survive moves, additions and removals elsewhere.
 *
 * Preconditions:
 *  - @self is a valid application and the watchlist is owned by the calling
 *    thread (the start-up load has finished); its entries have symbols that
 *    satisfy stk_symbol_is_valid() and that are NUL-terminated (the watchlist
 *    guarantees it), so stk_row_item_new() succeeds and the strings below are
 *    compared as C strings.
 *  - every index below is in range: entries are read only for indices below
 *    the entry count n, store items only below the store size old_n; the
 *    unsigned differences n - prefix and old_n - prefix cannot wrap because
 *    prefix <= min(n, old_n) after the first loop, and n - 1 - suffix and
 *    old_n - 1 - suffix cannot wrap because suffix < min(n, old_n) - prefix at
 *    each use.
 * Postconditions:
 *  - the store holds one item per entry in watchlist order; the table
 *    by_symbol holds exactly the symbols of the watchlist (items of removed
 *    symbols are released from it); the store changed with at most one
 *    items-changed emission.
 * Thread affinity: main thread.
 * Ownership: items are owned by by_symbol (strong) and the store (one
 *   reference each); watchlist entries are borrowed.
 */
static void
sync_items(StkApp *self)
{
  guint n = stk_watchlist_get_n_entries(self->watchlist);
  guint old_n = g_list_model_get_n_items(G_LIST_MODEL(self->items));

  for (guint i = 0; i < n; i++) {
    const StkWatchlistEntry *e = stk_watchlist_get_entry(self->watchlist, i);
    if (g_hash_table_contains(self->by_symbol, e->symbol))
      continue;
    StkRowItem *item = stk_row_item_new(e->symbol, e->name, e->kind);
    apply_cache(self, item);
    /* Restore the saved view before the handler below exists, so nothing is written back. */
    stk_row_item_set_view_range(item, e->range);
    stk_row_item_set_show_points(item, e->show_points);
    g_signal_connect_object(item, "changed", G_CALLBACK(on_item_view_changed), self,
                            G_CONNECT_SWAPPED);
    g_hash_table_insert(self->by_symbol, g_strdup(e->symbol), item);
  }

  guint prefix = 0;
  while (prefix < n && prefix < old_n &&
         g_str_equal(stk_watchlist_get_entry(self->watchlist, prefix)->symbol,
                     store_symbol_at(self->items, prefix)))
    prefix++;
  guint suffix = 0;
  while (suffix < n - prefix && suffix < old_n - prefix &&
         g_str_equal(stk_watchlist_get_entry(self->watchlist, n - 1 - suffix)->symbol,
                     store_symbol_at(self->items, old_n - 1 - suffix)))
    suffix++;

  guint n_removed = old_n - prefix - suffix;
  guint n_added = n - prefix - suffix;
  if (n_removed > 0 || n_added > 0) {
    gpointer *added = g_new(gpointer, n_added);
    for (guint i = 0; i < n_added; i++)
      added[i] = g_hash_table_lookup(
          self->by_symbol, stk_watchlist_get_entry(self->watchlist, prefix + i)->symbol);
    g_list_store_splice(self->items, prefix, n_removed, added, n_added);
    g_free(added);
  }

  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, self->by_symbol);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    if (!stk_watchlist_contains(self->watchlist, key))
      g_hash_table_iter_remove(&iter);
  }
}

/* ------------------------------------------------------------------------ */
/* Scheduler glue                                                           */
/* ------------------------------------------------------------------------ */

static void pump_actions(StkApp *self);

/**
 * refresh_stale:
 * @self: (not nullable): the application.
 * @symbol: (nullable): a symbol, or NULL.
 *
 * Makes the stale flag of the item of @symbol mirror the scheduler.
 *
 * Preconditions:
 *  - @self is a valid application; @symbol is NULL or NUL-terminated.
 * Postconditions:
 *  - if @symbol has an item, its stale flag equals stk_scheduler_is_stale();
 *    otherwise nothing changes.
 * Thread affinity: main thread.
 * Ownership: @symbol is borrowed.
 */
static void
refresh_stale(StkApp *self, const char *symbol)
{
  StkRowItem *item = lookup_item(self, symbol);
  if (item != NULL)
    stk_row_item_set_stale(item, stk_scheduler_is_stale(self->sched, symbol));
}

/**
 * request_free:
 * @r: (transfer full): a request that no longer is in the request table or
 *   whose table has been discarded.
 *
 * Preconditions:
 *  - @r was allocated by run_action() and is freed exactly once; nothing
 *    refers to it afterwards (finish_request() removes it from the table
 *    first).
 * Postconditions:
 *  - the weak reference is cleared, the cancellable released and @r freed.
 * Thread affinity: main thread.
 * Ownership: consumes @r.
 */
static void
request_free(Request *r)
{
  g_weak_ref_clear(&r->app);
  g_object_unref(r->cancellable);
  g_free(r);
}

/**
 * outcome:
 * @ok: whether the operation succeeded.
 * @error: (nullable): the error of a failed operation, may be NULL.
 * @r: (not nullable): the request, only used for the log line.
 * @what: (not nullable): "quote" or "series", for the log line.
 *
 * Maps an operation result to the scheduler's result. A failure is logged
 * with g_debug().
 *
 * Preconditions:
 *  - @r is a live request whose symbol is NUL-terminated; @what is
 *    NUL-terminated.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread.
 * Ownership: all pointers are borrowed.
 *
 * Returns: STK_SCHED_RESULT_OK if @ok; STK_SCHED_RESULT_CANCELLED if @error is
 *   G_IO_ERROR_CANCELLED; otherwise STK_SCHED_RESULT_FAILED (also when @error
 *   is NULL; the log then says "unknown error").
 */
static StkSchedResult
outcome(gboolean ok, const GError *error, const Request *r, const char *what)
{
  if (ok)
    return STK_SCHED_RESULT_OK;
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return STK_SCHED_RESULT_CANCELLED;
  g_debug("%s %s failed: %s", what, r->symbol, error != NULL ? error->message : "unknown error");
  return STK_SCHED_RESULT_FAILED;
}

/**
 * finish_request:
 * @self: (not nullable): the application.
 * @r: (transfer full): the request that ended.
 * @result: how it ended.
 *
 * Ends request @r (taken): hands its result to the scheduler, re-reads the
 * stale mark of its symbol and frees it. The order is load-bearing: @r leaves
 * the table before the scheduler may start a new request.
 *
 * Preconditions:
 *  - @self is a valid application; @r is a request in the table (its id, which
 *    the scheduler issued once and never reuses, is the key) that no one else
 *    frees.
 * Postconditions:
 *  - @r is removed from the table and freed; the scheduler knows the request is
 *    done; its actions were executed and the shutdown wait re-checked.
 * Thread affinity: main thread.
 * Ownership: consumes @r; @self is borrowed.
 */
static void
finish_request(StkApp *self, Request *r, StkSchedResult result)
{
  g_hash_table_remove(self->requests, &r->id);
  stk_scheduler_request_done(self->sched, sched_now(), r->id, result);
  refresh_stale(self, r->symbol);
  pump_actions(self);
  drain_progress(self);
  request_free(r);
}

/**
 * apply_quote:
 * @self: (not nullable): the application.
 * @r: (not nullable): the finished quote request.
 * @quote: (not nullable): the quote it delivered.
 *
 * Shows the quote on its item and stores it in the cache, but only if it
 * changed what the item holds.
 *
 * Preconditions:
 *  - @self, @r and @quote are valid; the cache stores the quote under
 *    @quote->symbol, the item is looked up by @r->symbol.
 * Postconditions:
 *  - if the item still exists and the quote differs: the item holds it and,
 *    if the cache accepts it, a save is scheduled; otherwise nothing changes.
 * Thread affinity: main thread.
 * Ownership: all pointers are borrowed; the item and the cache copy the quote.
 */
static void
apply_quote(StkApp *self, const Request *r, const StkQuote *quote)
{
  StkRowItem *item = lookup_item(self, r->symbol);
  if (item == NULL || !stk_row_item_set_quote(item, quote))
    return; /* removed meanwhile, or nothing new: nothing to show or to store */
  if (!stk_cache_put_quote(self->cache, quote, g_get_real_time() / 1000)) {
    g_debug("quote of %s rejected by the cache", r->symbol);
    return;
  }
  schedule_save(self);
}

/**
 * keeps_last_session:
 * @self: (not nullable): the application.
 * @r: (not nullable): the request that delivered @series.
 * @item: (not nullable): the item of @r's symbol.
 * @series: (not nullable): the delivered series.
 *
 * A successful 1D refresh with no point (Nasdaq before the open has none of
 * the new regular session yet) must not replace the last session's series,
 * neither on screen nor in the cache; see stk_sched_series_is_hollow().
 *
 * Preconditions:
 *  - all pointers are valid; @item belongs to @r->symbol.
 * Postconditions:
 *  - no state is changed apart from the cache's recency order.
 * Thread affinity: main thread.
 * Ownership: all pointers are borrowed; the cached series reference taken
 *   here is released.
 *
 * Returns: TRUE iff @series is a 1D series without a point and the item or the
 *   cache holds a non-empty 1D series (so @series must be dropped).
 */
static gboolean
keeps_last_session(StkApp *self, const Request *r, StkRowItem *item, const StkSeries *series)
{
  if (series->range != STK_RANGE_1D || series->n_points > 0)
    return FALSE;
  if (stk_sched_series_is_hollow(series, stk_row_item_get_detail_series(item)))
    return TRUE;
  g_autoptr(StkSeries) cached = stk_cache_get_series(self->cache, r->symbol, STK_RANGE_1D, NULL);
  return stk_sched_series_is_hollow(series, cached);
}

/**
 * apply_series:
 * @self: (not nullable): the application.
 * @r: (not nullable): the finished series request.
 * @series: (not nullable): the series it delivered.
 *
 * Shows the series on the sparkline and/or the detail pane of its item as the
 * range and the selection demand and stores it in the cache.
 *
 * Preconditions:
 *  - @self, @r and @series are valid. The symbols compared below are
 *    NUL-terminated: sel_symbol is a g_strdup() of an item symbol and
 *    @r->symbol was terminated by run_action().
 * Postconditions:
 *  - nothing changes if the item is gone, if @series->range differs from the
 *    requested range, or if keeps_last_session() holds. Otherwise the item
 *    shows it where it matches (sparkline: 1M range; detail: selected symbol
 *    and range) and the cache stores it (and a save is scheduled) unless a
 *    matching display already held an equal series.
 * Thread affinity: main thread.
 * Ownership: all pointers are borrowed; the item and the cache take their own
 *   references on @series.
 */
static void
apply_series(StkApp *self, const Request *r, StkSeries *series)
{
  StkRowItem *item = lookup_item(self, r->symbol);
  if (item == NULL)
    return;
  if (series->range != r->range) { /* the provider contract forbids it; never mix ranges up */
    g_debug("series of %s: asked for %s, got %s", r->symbol, stk_range_to_string(r->range),
            stk_range_to_string(series->range));
    return;
  }
  if (keeps_last_session(self, r, item, series)) {
    g_debug("series %s/1D has no point: keeping the last session", r->symbol);
    return;
  }
  gboolean matched = FALSE;
  gboolean changed = FALSE;
  if (series->range == STK_SCHED_SPARK_RANGE) {
    matched = TRUE;
    changed |= stk_row_item_set_spark(item, series);
  }
  if (self->sel_symbol != NULL && g_str_equal(self->sel_symbol, r->symbol) &&
      series->range == self->sel_range) {
    matched = TRUE;
    changed |= stk_row_item_set_detail(item, series->range, series);
  }
  /* A series nobody shows (its range was deselected meanwhile) is still worth
   * caching; one that equals what is shown is not worth a write. */
  if (matched && !changed)
    return;
  if (!stk_cache_put_series(self->cache, r->symbol, series, g_get_real_time() / 1000)) {
    g_debug("series %s/%s rejected by the cache", r->symbol, stk_range_to_string(series->range));
    return;
  }
  schedule_save(self);
}

/**
 * quote_done:
 * @source: (not nullable): the StkProvider that ran the request.
 * @result: (not nullable): the result of stk_provider_fetch_quote_async().
 * @user_data: (transfer full): the Request made by run_action().
 *
 * GAsyncReadyCallback of a quote request. When the application still exists
 * the result is applied (unless shutting down) and the request is finished;
 * when it is gone only the request is released.
 *
 * Preconditions:
 *  - called once, on the main thread, by the provider; @user_data is a Request
 *    that run_action() created for this operation and nothing else frees.
 * Postconditions:
 *  - the request is freed exactly once (through finish_request() or
 *    request_free()); the quote, if any, is freed.
 * Thread affinity: main thread.
 * Ownership: @user_data is consumed; @source and @result stay with the caller;
 *   a strong reference on the application is held only while the callback
 *   runs.
 */
static void
quote_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  Request *r = user_data;
  g_autoptr(StkApp) self = g_weak_ref_get(&r->app);
  g_autoptr(GError) error = NULL;
  StkQuote *quote = stk_provider_fetch_quote_finish(STK_PROVIDER(source), result, &error);
  StkSchedResult res = outcome(quote != NULL, error, r, "quote");
  if (self == NULL) { /* the application is gone (forced shutdown): only release the request */
    stk_quote_free(quote);
    request_free(r);
    return;
  }
  if (quote != NULL && !self->shutting_down)
    apply_quote(self, r, quote);
  stk_quote_free(quote);
  finish_request(self, r, res);
}

/**
 * series_done:
 * @source: (not nullable): the StkProvider that ran the request.
 * @result: (not nullable): the result of stk_provider_fetch_series_async().
 * @user_data: (transfer full): the Request made by run_action().
 *
 * GAsyncReadyCallback of a series request; same lifecycle as quote_done().
 *
 * Preconditions:
 *  - called once, on the main thread, by the provider; @user_data is a Request
 *    that run_action() created for this operation and nothing else frees.
 * Postconditions:
 *  - the request is freed exactly once (through finish_request() or
 *    request_free()); the series reference, if any, is released.
 * Thread affinity: main thread.
 * Ownership: @user_data is consumed; @source and @result stay with the caller;
 *   a strong reference on the application is held only while the callback
 *   runs.
 */
static void
series_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  Request *r = user_data;
  g_autoptr(StkApp) self = g_weak_ref_get(&r->app);
  g_autoptr(GError) error = NULL;
  StkSeries *series = stk_provider_fetch_series_finish(STK_PROVIDER(source), result, &error);
  StkSchedResult res = outcome(series != NULL, error, r, "series");
  if (self != NULL && series != NULL && !self->shutting_down)
    apply_series(self, r, series);
  if (series != NULL)
    stk_series_unref(series);
  if (self == NULL) { /* the application is gone (forced shutdown): only release the request */
    request_free(r);
    return;
  }
  finish_request(self, r, res);
}

/**
 * run_action:
 * @self: (not nullable): the application.
 * @a: (not nullable): one action of the scheduler, valid for the call.
 *
 * Executes a scheduler action: a CANCEL cancels the request with that id, a
 * FETCH starts the request with the provider (nothing while shutting down).
 *
 * Preconditions:
 *  - @self and @a are valid; for a FETCH, @a->symbol is NUL-terminated within
 *    STK_SCHED_SYMBOL_BYTES bytes and @a->id is a fresh id (the scheduler
 *    starts at 1, only increments and never reuses an id), so the id is not
 *    in the request table yet and the table key &r->id stays unique and valid
 *    for as long as r is in the table.
 * Postconditions:
 *  - CANCEL: the cancellable of the request (if it is still in flight) was
 *    cancelled; a strong reference on it is held meanwhile because the
 *    cancellation may complete the operation synchronously and free the
 *    Request, which is not touched afterwards.
 *    FETCH (not shutting down): a Request exists in the table and one provider
 *    call is running; the symbol copy g_strlcpy()'d into the Request is not
 *    truncated (see the static assertion above). The completion callback owns
 *    the Request.
 * Thread affinity: main thread.
 * Ownership: @a is borrowed; the new Request is owned by the operation's
 *   completion callback, the table does not own it.
 */
static void
run_action(StkApp *self, const StkSchedAction *a)
{
  if (a->type == STK_SCHED_CANCEL) {
    g_debug("cancel #%" G_GUINT64_FORMAT, a->id);
    Request *r = g_hash_table_lookup(self->requests, &a->id);
    if (r != NULL) {
      /* Cancelling may complete the request synchronously and free it. */
      g_autoptr(GCancellable) cancellable = g_object_ref(r->cancellable);
      g_cancellable_cancel(cancellable);
    }
    return;
  }
  if (self->shutting_down)
    return;

  Request *r = g_new0(Request, 1);
  g_weak_ref_init(&r->app, self);
  r->id = a->id;
  r->cancellable = g_cancellable_new();
  g_strlcpy(r->symbol, a->symbol, sizeof r->symbol);
  r->range = a->range;
  g_debug("request #%" G_GUINT64_FORMAT " %s %s%s%s", a->id,
          a->type == STK_SCHED_FETCH_QUOTE ? "quote" : "series", a->symbol,
          a->type == STK_SCHED_FETCH_SERIES ? " " : "",
          a->type == STK_SCHED_FETCH_SERIES ? stk_range_to_string(a->range) : "");
  g_hash_table_insert(self->requests, &r->id, r);
  if (a->type == STK_SCHED_FETCH_QUOTE)
    stk_provider_fetch_quote_async(self->provider, a->symbol, a->kind, r->cancellable, quote_done,
                                   r);
  else
    stk_provider_fetch_series_async(self->provider, a->symbol, a->kind, a->range, r->cancellable,
                                    series_done, r);
}

/**
 * wake_timeout:
 * @user_data: (not nullable): the StkApp that armed the timer (borrowed raw
 *   pointer).
 *
 * GSourceFunc of the scheduler's single timer.
 *
 * Preconditions:
 *  - the source is alive only while @user_data is: stk_app_dispose() (and
 *    stk_app_shutdown()) remove wake_id before the application can be
 *    finalized.
 * Postconditions:
 *  - wake_id is 0 and wake_us -1; the scheduler was ticked and its actions
 *    executed, which may arm a new timer; the source is removed.
 * Thread affinity: main thread (default main context).
 * Ownership: @user_data is borrowed.
 *
 * Returns: G_SOURCE_REMOVE always (a new timer is made by rearm_timer()).
 */
static gboolean
wake_timeout(gpointer user_data)
{
  StkApp *self = STK_APP(user_data);
  self->wake_id = 0;
  self->wake_us = -1;
  stk_scheduler_tick(self->sched, sched_now());
  pump_actions(self);
  return G_SOURCE_REMOVE;
}

/**
 * rearm_timer:
 * @self: (not nullable): the application.
 *
 * Keeps exactly the timer the scheduler asks for: none, or one at its deadline.
 *
 * Preconditions:
 *  - @self is a valid application. The scheduler derives every deadline from
 *    a g_get_monotonic_time() reading plus at most STK_SCHED_PERIOD_US, so
 *    the difference to the current monotonic time is small and
 *    "delay_us + 999" cannot overflow; the delay is clamped to G_MAXINT32 ms
 *    and is never negative before the unsigned cast.
 * Postconditions:
 *  - without a deadline no timer exists (wake_id 0, wake_us -1); with one
 *    there is exactly one timer armed for it (kept if it already was).
 * Thread affinity: main thread.
 * Ownership: the timer holds @self as a borrowed raw pointer (see
 *   wake_timeout()).
 */
static void
rearm_timer(StkApp *self)
{
  gint64 wake = stk_scheduler_next_wakeup_us(self->sched);
  if (wake < 0) {
    g_clear_handle_id(&self->wake_id, g_source_remove);
    self->wake_us = -1;
    return;
  }
  if (self->wake_id != 0 && self->wake_us == wake)
    return;
  g_clear_handle_id(&self->wake_id, g_source_remove);
  gint64 delay_us = wake - g_get_monotonic_time();
  gint64 delay_ms = delay_us <= 0 ? 0 : (delay_us + 999) / 1000;
  self->wake_us = wake;
  self->wake_id = g_timeout_add((guint)MIN(delay_ms, (gint64)G_MAXINT32), wake_timeout, self);
}

/**
 * pump_actions:
 * @self: (not nullable): the application.
 *
 * Executes the actions the scheduler has queued and re-arms the timer.
 *
 * Preconditions:
 *  - @self is a valid application.
 * Postconditions:
 *  - every pending scheduler action was passed (as a copy) to run_action() in
 *    order, and the timer follows the scheduler's wakeup.
 * Thread affinity: main thread.
 * Ownership: the action array taken from the scheduler is released here.
 */
static void
pump_actions(StkApp *self)
{
  g_autoptr(GArray) actions = stk_scheduler_take_actions(self->sched);
  if (actions != NULL) {
    for (guint i = 0; i < actions->len; i++) {
      StkSchedAction a = g_array_index(actions, StkSchedAction, i);
      run_action(self, &a);
    }
  }
  rearm_timer(self);
}

/**
 * sync_scheduler_symbols:
 * @self: (not nullable): the application.
 *
 * Feeds the watchlist to the scheduler and executes the resulting actions.
 *
 * Preconditions:
 *  - @self is a valid application; the watchlist is owned by the calling
 *    thread (the start-up load has finished). Its entries (indices below its
 *    entry count, so never NULL) have symbols satisfying stk_symbol_is_valid().
 * Postconditions:
 *  - the scheduler tracks the watchlist's symbols and kinds; the temporary
 *    array of borrowed symbol pointers is freed before the scheduler's
 *    actions are executed.
 * Thread affinity: main thread.
 * Ownership: the symbol pointers are borrowed from the watchlist for the call.
 */
static void
sync_scheduler_symbols(StkApp *self)
{
  guint n = stk_watchlist_get_n_entries(self->watchlist);
  StkSchedSymbol *symbols = g_new(StkSchedSymbol, n > 0 ? n : 1);
  for (guint i = 0; i < n; i++) {
    const StkWatchlistEntry *e = stk_watchlist_get_entry(self->watchlist, i);
    symbols[i].symbol = e->symbol;
    symbols[i].kind = e->kind;
  }
  stk_scheduler_set_symbols(self->sched, sched_now(), symbols, n);
  g_free(symbols);
  pump_actions(self);
}

/**
 * on_watchlist_changed:
 * @self: (not nullable): the application (the swapped-signal instance).
 *
 * Handler of the watchlist's "changed" signal (connected with
 * g_signal_connect_swapped() after the start-up load).
 *
 * Preconditions:
 *  - emitted on the main thread (the handler is connected only after the load
 *    has finished, so it never runs on the load worker); @self is alive:
 *    stk_app_dispose() disconnects the handler.
 * Postconditions:
 *  - the items and the scheduler follow the watchlist and a save is scheduled.
 * Thread affinity: main thread.
 * Ownership: @self is borrowed.
 */
static void
on_watchlist_changed(StkApp *self)
{
  sync_items(self);
  sync_scheduler_symbols(self);
  schedule_save(self);
}

/**
 * set_offline:
 * @self: (not nullable): the application.
 * @offline: the new network state.
 *
 * Preconditions:
 *  - @self is a valid application.
 * Postconditions:
 *  - when the state changes: the "offline" property is notified, the
 *    scheduler told and its actions executed; otherwise nothing happens.
 * Thread affinity: main thread.
 * Ownership: none (no pointers besides @self).
 */
static void
set_offline(StkApp *self, gboolean offline)
{
  if (self->offline == offline)
    return;
  self->offline = offline;
  g_object_notify(G_OBJECT(self), "offline");
  stk_scheduler_set_online(self->sched, sched_now(), !offline);
  pump_actions(self);
}

/**
 * on_network_changed:
 * @monitor: (not nullable): the network monitor.
 * @available: the monitor's reported availability (unused: the monitor is
 *   asked again).
 * @user_data: (not nullable): the StkApp the handler was connected for.
 *
 * Handler of the monitor's "network-changed" signal.
 *
 * Preconditions:
 *  - emitted on the main thread; @user_data is alive: stk_app_dispose()
 *    disconnects the handler.
 * Postconditions:
 *  - set_offline() was called with the monitor's current availability.
 * Thread affinity: main thread.
 * Ownership: all pointers are borrowed.
 */
static void
on_network_changed(GNetworkMonitor *monitor, gboolean available, gpointer user_data)
{
  (void)available;
  set_offline(STK_APP(user_data), !g_network_monitor_get_network_available(monitor));
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

/* Documented in stk-app.h. */
StkApp *
stk_app_new(void)
{
  return g_object_new(STK_TYPE_APP, "application-id", STK_APP_ID, "flags", G_APPLICATION_FLAGS_NONE,
                      NULL);
}

/* Documented in stk-app.h. */
GListModel *
stk_app_get_items(StkApp *self)
{
  g_return_val_if_fail(STK_IS_APP(self), NULL);
  return G_LIST_MODEL(self->items);
}

/* Documented in stk-app.h. */
StkWatchlist *
stk_app_get_watchlist(StkApp *self)
{
  g_return_val_if_fail(STK_IS_APP(self), NULL);
  return self->watchlist;
}

/* Documented in stk-app.h. */
StkProvider *
stk_app_get_provider(StkApp *self)
{
  g_return_val_if_fail(STK_IS_APP(self), NULL);
  return self->provider;
}

/* Documented in stk-app.h. */
const StkCatalog *
stk_app_get_catalog(StkApp *self)
{
  g_return_val_if_fail(STK_IS_APP(self), NULL);
  if (self->catalog == NULL && !self->catalog_failed) {
    g_autoptr(GError) error = NULL;
    self->catalog = stk_catalog_load_default(&error);
    if (self->catalog == NULL) {
      self->catalog_failed = TRUE;
      g_warning("Cannot load the Fidelity catalog: %s",
                error != NULL ? error->message : "unknown error");
    }
  }
  return self->catalog;
}

/* Documented in stk-app.h. */
gboolean
stk_app_get_offline(StkApp *self)
{
  g_return_val_if_fail(STK_IS_APP(self), FALSE);
  return self->offline;
}

/* Documented in stk-app.h. */
void
stk_app_set_window_visible(StkApp *self, gboolean visible)
{
  g_return_if_fail(STK_IS_APP(self));
  if (self->shutting_down)
    return;
  g_debug("window visible=%d", visible);
  stk_scheduler_set_visible(self->sched, sched_now(), visible);
  pump_actions(self);
}

/* Documented in stk-app.h. */
void
stk_app_window_focused(StkApp *self)
{
  g_return_if_fail(STK_IS_APP(self));
  if (self->shutting_down)
    return;
  g_debug("window focused");
  stk_scheduler_focus_regained(self->sched, sched_now());
  pump_actions(self);
}

/* Documented in stk-app.h. */
void
stk_app_select(StkApp *self, const char *symbol, StkRange range)
{
  g_return_if_fail(STK_IS_APP(self));
  g_return_if_fail(range >= STK_RANGE_1D && range <= STK_RANGE_1Y);
  if (self->shutting_down)
    return; /* the windows are being torn down: their signals no longer mean anything */

  StkRowItem *item = lookup_item(self, symbol);
  StkRowItem *old = lookup_item(self, self->sel_symbol);
  if (old != NULL && old != item)
    stk_row_item_set_detail(old, stk_row_item_get_detail_range(old), NULL);

  const char *new_symbol = item != NULL ? stk_row_item_get_symbol(item) : NULL;
  g_autofree char *previous = self->sel_symbol; /* the previously selected symbol, or NULL */
  self->sel_symbol = g_strdup(new_symbol);
  self->sel_range = range;

  if (item != NULL) {
    g_autoptr(StkSeries) cached = stk_cache_get_series(self->cache, new_symbol, range, NULL);
    stk_row_item_set_detail(item, range, cached);
  }
  g_debug("select %s %s", new_symbol != NULL ? new_symbol : "(none)", stk_range_to_string(range));
  stk_scheduler_set_selection(self->sched, sched_now(), new_symbol, range);
  /* set_selection() forgets the failure of the DETAIL job of the previous and of the new
   * selection (see "Stale marks" in stk-scheduler.h): both items must follow. */
  refresh_stale(self, previous);
  refresh_stale(self, new_symbol);
  pump_actions(self);
}

/* ------------------------------------------------------------------------ */
/* Start-up load (worker thread)                                            */
/* ------------------------------------------------------------------------ */

typedef struct {
  guint n_dropped;
  char *watchlist_error;
  char *cache_error;
} LoadResult;

/**
 * load_result_free:
 * @r: (transfer full): a LoadResult.
 *
 * GDestroyNotify of the load task's result.
 *
 * Preconditions:
 *  - @r is non-NULL and heap-allocated (g_new0()) with NULL or g_strdup()'d
 *    strings.
 * Postconditions:
 *  - the strings and @r are freed.
 * Thread affinity: the thread that releases the result (main thread, from
 *   load_done(); the task would release it on whichever thread drops the
 *   last reference).
 * Ownership: consumes @r.
 */
static void
load_result_free(LoadResult *r)
{
  g_free(r->watchlist_error);
  g_free(r->cache_error);
  g_free(r);
}

/**
 * load_thread:
 * @task: (not nullable): the load task.
 * @source: (not nullable): the StkApp the task was created for.
 * @task_data: unused (NULL).
 * @cancellable: unused (the task has none).
 *
 * Runs on a worker thread. The watchlist and the cache are touched by nobody
 * else until the task has completed on the main thread, which is the hand-off
 * that makes it correct for these main-thread-owned objects. A missing file
 * is not an error. The watchlist's "changed" signal emitted by the load runs on
 * this thread; the application has no handler connected yet.
 *
 * Preconditions:
 *  - @source is the application, kept alive by the task; the watchlist and the
 *    cache of @source exist and no other thread uses them (stk_app_startup()
 *    creates the task before any window exists and the shutdown path does not
 *    touch them while "loaded" is FALSE).
 * Postconditions:
 *  - the task returns a LoadResult with the number of dropped entries and the
 *    message of any load error other than "file does not exist"; a failing
 *    load without a GError is reported as "unknown error".
 * Thread affinity: worker thread only (GTask thread pool).
 * Ownership: the LoadResult is handed to the task (destroy notify
 *   load_result_free()); @source, @task_data and @cancellable are borrowed.
 */
static void
load_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  (void)task_data;
  (void)cancellable;
  StkApp *self = STK_APP(source);
  LoadResult *r = g_new0(LoadResult, 1);

  g_autoptr(GError) wl_error = NULL;
  if (!stk_watchlist_load(self->watchlist, &r->n_dropped, &wl_error) &&
      !g_error_matches(wl_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    r->watchlist_error = g_strdup(wl_error != NULL ? wl_error->message : "unknown error");

  g_autoptr(GError) cache_error = NULL;
  if (!stk_cache_load(self->cache, &cache_error) &&
      !g_error_matches(cache_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    r->cache_error = g_strdup(cache_error != NULL ? cache_error->message : "unknown error");

  g_task_return_pointer(task, r, (GDestroyNotify)load_result_free);
}

static void present_window(StkApp *self);

/**
 * load_done:
 * @source: (not nullable): the StkApp that started the load.
 * @result: (not nullable): the finished GTask of load_thread().
 * @user_data: unused (NULL).
 *
 * GAsyncReadyCallback of the start-up load: logs what the load reported, hooks
 * the application up to the watchlist, fills the items and the scheduler, and
 * presents the window if activate already came; always drops the start-up hold.
 *
 * Preconditions:
 *  - called once, on the main thread, after load_thread() returned; the hold
 *    taken by stk_app_startup() is still in place.
 * Postconditions:
 *  - loading is FALSE; a missing result (NULL from the task) is treated like a
 *    load without messages. If the application is shutting down the shutdown
 *    wait is re-checked; otherwise loaded is TRUE, the "changed" handler is
 *    connected, items and scheduler hold the watchlist and the window is
 *    presented if activation was pending. The hold is released.
 * Thread affinity: main thread.
 * Ownership: the LoadResult obtained from the task is freed here; @source and
 *   @result stay with the caller.
 */
static void
load_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)user_data;
  StkApp *self = STK_APP(source);
  LoadResult *r = g_task_propagate_pointer(G_TASK(result), NULL);
  self->loading = FALSE;

  if (r != NULL) {
    if (r->watchlist_error != NULL)
      g_warning("Could not read the watchlist (starting empty): %s", r->watchlist_error);
    if (r->n_dropped > 0)
      g_warning("%u invalid watchlist entries were dropped", r->n_dropped);
    if (r->cache_error != NULL)
      g_warning("Could not read the cache (starting empty): %s", r->cache_error);
    load_result_free(r);
  }

  if (self->shutting_down) {
    drain_progress(self);
  } else {
    g_debug("loaded %u watchlist entries", stk_watchlist_get_n_entries(self->watchlist));
    self->loaded = TRUE;
    self->watchlist_changed_id =
        g_signal_connect_swapped(self->watchlist, "changed", G_CALLBACK(on_watchlist_changed), self);
    sync_items(self);
    sync_scheduler_symbols(self);
    if (self->activate_pending)
      present_window(self);
  }
  g_application_release(G_APPLICATION(self));
}

/* ------------------------------------------------------------------------ */
/* GApplication                                                             */
/* ------------------------------------------------------------------------ */

/**
 * install_css:
 *
 * Installs the compiled-in style sheet for the default display.
 *
 * Preconditions:
 *  - GTK is initialised (called from stk_app_startup() after the parent
 *    startup). The resource lookup reports a failure through its GError, which
 *    GLib sets whenever it returns NULL.
 * Postconditions:
 *  - the CSS provider is installed at application priority; nothing is
 *    installed (with a g_warning()) if the resource is missing or empty or
 *    if there is no default display.
 * Thread affinity: main thread.
 * Ownership: the bytes and the provider are released here (the display holds
 *   its own reference on the provider).
 */
static void
install_css(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = g_resource_lookup_data(stk_ui_get_resource(), CSS_RESOURCE_PATH,
                                                   G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
  if (bytes == NULL) {
    g_warning("Missing style resource: %s", error->message);
    return;
  }
  gsize len = 0;
  const char *data = g_bytes_get_data(bytes, &len);
  if (data == NULL || len == 0) {
    g_warning("Empty style resource");
    return;
  }
  GdkDisplay *display = gdk_display_get_default();
  if (display == NULL) {
    g_warning("No display: the style resource is not installed");
    return;
  }
  g_autofree char *text = g_strndup(data, len);
  g_autoptr(GtkCssProvider) provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, text);
  gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/**
 * stk_app_startup:
 * @application: (not nullable): the StkApp (GApplication::startup vfunc).
 *
 * Chains up, installs the CSS, hooks up the network monitor and starts the
 * load of the watchlist and the cache on a worker thread, holding the
 * application until it finished.
 *
 * Preconditions:
 *  - called once by GApplication on the main thread, before activate.
 * Postconditions:
 *  - the network monitor is referenced and its handler connected; the
 *    scheduler knows the network state; loading is TRUE; the application is
 *    held (released by load_done()); one worker task owns the right to touch
 *    the watchlist and the cache until load_done().
 * Thread affinity: main thread.
 * Ownership: the task holds its own reference on the application; the monitor
 *   reference is released in stk_app_dispose().
 */
static void
stk_app_startup(GApplication *application)
{
  StkApp *self = STK_APP(application);
  G_APPLICATION_CLASS(stk_app_parent_class)->startup(application);
  install_css();

  self->netmon = g_object_ref(g_network_monitor_get_default());
  self->offline = !g_network_monitor_get_network_available(self->netmon);
  self->netmon_id =
      g_signal_connect(self->netmon, "network-changed", G_CALLBACK(on_network_changed), self);
  stk_scheduler_set_online(self->sched, sched_now(), !self->offline);

  /* Alive until the window exists (released in load_done). */
  g_application_hold(application);
  self->loading = TRUE;
  g_autoptr(GTask) task = g_task_new(self, NULL, load_done, NULL);
  g_task_run_in_thread(task, load_thread);
}

/**
 * present_window:
 * @self: (not nullable): the application.
 *
 * Shows the main window, creating it on first use.
 *
 * Preconditions:
 *  - @self is a valid application, loaded and not shutting down (callers:
 *    stk_app_activate() and load_done()).
 * Postconditions:
 *  - the active window (existing or new) was presented.
 * Thread affinity: main thread.
 * Ownership: a new window is owned by the application (GtkApplication tracks
 *   it); nothing is returned.
 */
static void
present_window(StkApp *self)
{
  GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(self));
  if (window == NULL)
    window = GTK_WINDOW(stk_window_new(self));
  gtk_window_present(window);
}

/**
 * stk_app_activate:
 * @application: (not nullable): the StkApp (GApplication::activate vfunc).
 *
 * Presents the window now if the load finished, otherwise as soon as it does.
 *
 * Preconditions:
 *  - called by GApplication on the main thread after startup.
 * Postconditions:
 *  - the window is presented when loaded; otherwise activate_pending is TRUE.
 * Thread affinity: main thread.
 * Ownership: none (no pointers besides @application).
 */
static void
stk_app_activate(GApplication *application)
{
  StkApp *self = STK_APP(application);
  if (self->loaded)
    present_window(self);
  else
    self->activate_pending = TRUE;
}

/**
 * drain_timeout:
 * @user_data: (not nullable): the StkApp that armed the timer (borrowed raw
 *   pointer).
 *
 * GSourceFunc of the shutdown wait limit.
 *
 * Preconditions:
 *  - the source is alive only while @user_data is and while the wait loop
 *    exists: stk_app_shutdown() removes drain_id right after the loop ran and
 *    stk_app_dispose() removes it too.
 * Postconditions:
 *  - drain_id is 0, a warning was logged and the wait loop was asked to quit;
 *    the source is removed.
 * Thread affinity: main thread (default main context).
 * Ownership: @user_data is borrowed.
 *
 * Returns: G_SOURCE_REMOVE always (one-shot).
 */
static gboolean
drain_timeout(gpointer user_data)
{
  StkApp *self = STK_APP(user_data);
  self->drain_id = 0;
  g_warning("Gave up waiting for pending saves/requests after %d ms", STK_APP_SHUTDOWN_WAIT_MS);
  g_main_loop_quit(self->drain_loop);
  return G_SOURCE_REMOVE;
}

/**
 * stk_app_shutdown:
 * @application: (not nullable): the StkApp (GApplication::shutdown vfunc).
 *
 * Cancels every request, lets the windows go, saves what is dirty and waits,
 * in a nested main loop of at most STK_APP_SHUTDOWN_WAIT_MS, for the start-up
 * load, the saves and the cancelled requests. Pending operations that have not
 * finished by then are abandoned (they hold only weak references).
 *
 * Preconditions:
 *  - called once by GApplication on the main thread after the main loop ended.
 * Postconditions:
 *  - shutting_down is TRUE, the save and refresh timers are removed, the
 *    scheduler was told to hide; if loaded, saves of dirty data were started.
 *    On return nothing is pending or the wait limit elapsed (a warning was
 *    logged); the wait loop and its timer no longer exist.
 * Thread affinity: main thread.
 * Ownership: the wait loop is created and released here; the Requests and
 *   save boxes still pending belong to their callbacks.
 */
static void
stk_app_shutdown(GApplication *application)
{
  StkApp *self = STK_APP(application);
  self->shutting_down = TRUE;
  g_debug("shutdown: saving and waiting for %u request(s)", g_hash_table_size(self->requests));
  g_clear_handle_id(&self->save_id, g_source_remove);

  /* Cancel everything in flight, then let the windows go. */
  stk_scheduler_set_visible(self->sched, sched_now(), FALSE);
  pump_actions(self);
  g_clear_handle_id(&self->wake_id, g_source_remove);
  G_APPLICATION_CLASS(stk_app_parent_class)->shutdown(application);

  if (self->loaded)
    start_saves(self);
  if (pending_io(self)) {
    self->drain_loop = g_main_loop_new(NULL, FALSE);
    self->drain_id = g_timeout_add(STK_APP_SHUTDOWN_WAIT_MS, drain_timeout, self);
    g_main_loop_run(self->drain_loop);
    g_clear_handle_id(&self->drain_id, g_source_remove);
    g_clear_pointer(&self->drain_loop, g_main_loop_unref);
  }
}

/**
 * stk_app_get_property:
 * @object: (not nullable): the StkApp.
 * @prop_id: the property id (PROP_OFFLINE is the only one).
 * @value: (not nullable): the GValue to fill.
 * @pspec: (not nullable): the property's GParamSpec.
 *
 * GObject get_property vfunc.
 *
 * Preconditions:
 *  - called by GObject with @value initialised to the property's type.
 * Postconditions:
 *  - PROP_OFFLINE: @value holds the offline flag; any other id logs the
 *    standard invalid-property warning and leaves @value untouched.
 * Thread affinity: main thread.
 * Ownership: all pointers are borrowed; @value is filled for the caller.
 */
static void
stk_app_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  StkApp *self = STK_APP(object);
  switch (prop_id) {
  case PROP_OFFLINE:
    g_value_set_boolean(value, self->offline);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

/**
 * stk_app_dispose:
 * @object: (not nullable): the StkApp (GObject dispose vfunc).
 *
 * Removes every source and signal handler that holds a raw pointer to the
 * application (save, refresh and shutdown-wait timers, the network and
 * watchlist handlers), releases the network monitor and chains up. It may run
 * more than once.
 *
 * Preconditions:
 *  - called by GObject on the main thread. No completion callback of a pending
 *    operation can run during it (one thread); once the last reference is
 *    gone GObject clears every GWeakRef to the application, so a callback that
 *    runs later sees the application as gone.
 * Postconditions:
 *  - save_id, wake_id and drain_id are 0, the handler ids are 0, netmon is
 *    NULL.
 * Thread affinity: the thread that drops the last reference (main thread).
 * Ownership: the monitor reference is released; the handlers and sources are
 *   removed.
 */
static void
stk_app_dispose(GObject *object)
{
  StkApp *self = STK_APP(object);
  g_clear_handle_id(&self->save_id, g_source_remove);
  g_clear_handle_id(&self->wake_id, g_source_remove);
  g_clear_handle_id(&self->drain_id, g_source_remove);
  if (self->netmon != NULL && self->netmon_id != 0)
    g_signal_handler_disconnect(self->netmon, self->netmon_id);
  self->netmon_id = 0;
  g_clear_object(&self->netmon);
  if (self->watchlist != NULL && self->watchlist_changed_id != 0)
    g_signal_handler_disconnect(self->watchlist, self->watchlist_changed_id);
  self->watchlist_changed_id = 0;
  G_OBJECT_CLASS(stk_app_parent_class)->dispose(object);
}

/**
 * stk_app_finalize:
 * @object: (not nullable): the StkApp (GObject finalize vfunc).
 *
 * Frees what the application owns and chains up.
 *
 * Preconditions:
 *  - called once by GObject after dispose; no timer or handler refers to the
 *    application any more (see stk_app_dispose()).
 * Postconditions:
 *  - the scheduler, the request table (without the Requests, which belong to
 *    their callbacks), the symbol table, the item store, the provider, the
 *    watchlist, the cache, the catalog and the selected symbol are released.
 * Thread affinity: the thread that drops the last reference (main thread).
 * Ownership: everything the application owns is released.
 */
static void
stk_app_finalize(GObject *object)
{
  StkApp *self = STK_APP(object);
  g_clear_pointer(&self->sched, stk_scheduler_free);
  g_clear_pointer(&self->requests, g_hash_table_unref);
  g_clear_pointer(&self->by_symbol, g_hash_table_unref);
  g_clear_object(&self->items);
  g_clear_object(&self->provider);
  g_clear_object(&self->watchlist);
  g_clear_object(&self->cache);
  g_clear_pointer(&self->catalog, stk_catalog_free);
  g_free(self->sel_symbol);
  G_OBJECT_CLASS(stk_app_parent_class)->finalize(object);
}

/**
 * stk_app_class_init:
 * @klass: (not nullable): the class structure being initialised.
 *
 * Installs the GApplication startup / activate / shutdown vfuncs, the GObject
 * get_property / dispose / finalize vfuncs and the read-only "offline"
 * property.
 *
 * Preconditions:
 *  - called once by the GType system.
 * Postconditions:
 *  - the vfuncs and the property are installed.
 * Thread affinity: the thread that first uses the type (GType serialises it).
 * Ownership: none (the class is owned by GType).
 */
static void
stk_app_class_init(StkAppClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  GApplicationClass *ac = G_APPLICATION_CLASS(klass);
  oc->get_property = stk_app_get_property;
  oc->dispose = stk_app_dispose;
  oc->finalize = stk_app_finalize;
  ac->startup = stk_app_startup;
  ac->activate = stk_app_activate;
  ac->shutdown = stk_app_shutdown;
  g_object_class_install_property(
      oc, PROP_OFFLINE,
      g_param_spec_boolean("offline", NULL, NULL, FALSE,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY));
}

/**
 * stk_app_init:
 * @self: (not nullable): the instance being initialised.
 *
 * Creates the provider (a router over Nasdaq and Yahoo), the watchlist and the
 * cache (default paths, nothing read), the item store, the symbol table, the
 * scheduler and the request table.
 *
 * Preconditions:
 *  - @self is a zero-initialised new instance.
 * Postconditions:
 *  - all members above exist; no timer, handler or request exists; the
 *    selection is none with range STK_RANGE_1D and wake_us is -1.
 * Thread affinity: the thread that creates the instance (main thread).
 * Ownership: the instance owns everything created; the two inner providers
 *   are also referenced by the router, so the local references are released.
 */
static void
stk_app_init(StkApp *self)
{
  g_autoptr(StkNasdaq) nasdaq = stk_nasdaq_new();
  g_autoptr(StkYahoo) yahoo = stk_yahoo_new();
  self->provider = STK_PROVIDER(stk_router_new(STK_PROVIDER(nasdaq), STK_PROVIDER(yahoo)));
  self->watchlist = stk_watchlist_new(NULL);
  self->cache = stk_cache_new(NULL);
  self->items = g_list_store_new(STK_TYPE_ROW_ITEM);
  self->by_symbol = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->sched = stk_scheduler_new();
  self->requests = g_hash_table_new(g_int64_hash, g_int64_equal);
  self->sel_range = STK_RANGE_1D;
  self->wake_us = -1;
}
