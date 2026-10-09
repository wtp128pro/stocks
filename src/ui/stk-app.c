/* stk-app.c - StkApp: data ownership, refresh execution, persistence. */
#include "stk-app.h"

#include <string.h>

#include "stk-cache.h"
#include "stk-nasdaq.h"
#include "stk-row.h"
#include "stk-scheduler.h"
#include "stk-ui-resources.h"
#include "stk-window.h"

#define CSS_RESOURCE_PATH "/org/stocks/ui/style.css"

typedef struct {
  GWeakRef app; /* weak: a pending request must never keep the application alive */
  guint64 id;
  GCancellable *cancellable;
  char symbol[STK_SCHED_SYMBOL_BYTES];
  StkRange range; /* series requests */
} Request;

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

G_DEFINE_FINAL_TYPE(StkApp, stk_app, GTK_TYPE_APPLICATION)

enum { PROP_0, PROP_OFFLINE, N_PROPS };

static StkSchedTime
sched_now(void)
{
  StkSchedTime t = {g_get_monotonic_time(), g_get_real_time() / 1000};
  return t;
}

static StkRowItem *
lookup_item(StkApp *self, const char *symbol)
{
  return symbol != NULL ? g_hash_table_lookup(self->by_symbol, symbol) : NULL;
}

/* ------------------------------------------------------------------------ */
/* Persistence                                                              */
/* ------------------------------------------------------------------------ */

static gboolean
pending_io(StkApp *self)
{
  return self->loading || self->saving_watchlist || self->saving_cache ||
         g_hash_table_size(self->requests) > 0;
}

/* During the shutdown wait: leave the nested loop once nothing is pending. */
static void
drain_progress(StkApp *self)
{
  if (self->drain_loop != NULL && !pending_io(self))
    g_main_loop_quit(self->drain_loop);
}

static void start_saves(StkApp *self);
static void schedule_save(StkApp *self);

/* An operation (provider request, file save) can still be pending when the
 * application goes away after a forced shutdown (hung network or disk). Its
 * user_data therefore holds no strong reference to the application; the
 * completion callback takes one only while it runs and, when the application
 * is gone, just releases what the operation itself owns. */
static GWeakRef *
weak_app_new(StkApp *self)
{
  GWeakRef *ref = g_new(GWeakRef, 1);
  g_weak_ref_init(ref, self);
  return ref;
}

/* Frees @ref. Returns: (transfer full) (nullable): the application, or NULL if it is gone. */
static StkApp *
weak_app_take(GWeakRef *ref)
{
  StkApp *self = g_weak_ref_get(ref);
  g_weak_ref_clear(ref);
  g_free(ref);
  return self;
}

static void
watchlist_saved(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(StkApp) self = weak_app_take(user_data);
  g_autoptr(GError) error = NULL;
  gboolean saved = stk_watchlist_save_finish(STK_WATCHLIST(source), result, &error);
  if (!saved)
    g_warning("Could not save the watchlist: %s", error->message);
  if (self == NULL)
    return; /* the application is gone */
  self->saving_watchlist = FALSE;
  if (saved && stk_watchlist_is_dirty(self->watchlist)) /* edited while writing */
    self->shutting_down ? start_saves(self) : schedule_save(self);
  drain_progress(self);
}

static void
cache_saved(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(StkApp) self = weak_app_take(user_data);
  g_autoptr(GError) error = NULL;
  gboolean saved = stk_cache_save_finish(STK_CACHE(source), result, &error);
  if (!saved)
    g_warning("Could not save the cache: %s", error->message);
  if (self == NULL)
    return; /* the application is gone */
  self->saving_cache = FALSE;
  if (saved && stk_cache_is_dirty(self->cache))
    self->shutting_down ? start_saves(self) : schedule_save(self);
  drain_progress(self);
}

/* Starts an asynchronous save of whatever is dirty and not already being saved. */
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

static gboolean
save_timeout(gpointer user_data)
{
  StkApp *self = STK_APP(user_data);
  self->save_id = 0;
  start_saves(self);
  return G_SOURCE_REMOVE;
}

static void
schedule_save(StkApp *self)
{
  if (self->save_id == 0 && !self->shutting_down)
    self->save_id = g_timeout_add(STK_APP_SAVE_DELAY_MS, save_timeout, self);
}

/* ------------------------------------------------------------------------ */
/* Items                                                                    */
/* ------------------------------------------------------------------------ */

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

static const char *
store_symbol_at(GListStore *store, guint position)
{
  /* The store keeps the item alive, so the borrowed string outlives the unref. */
  g_autoptr(StkRowItem) item = g_list_model_get_item(G_LIST_MODEL(store), position);
  return stk_row_item_get_symbol(item);
}

/* Brings the item model in line with the watchlist: new symbols get an item
 * (painted from the cache), and the changed middle part of the list is
 * replaced with one splice, so that unchanged items (and a selection on them)
 * survive moves, additions and removals elsewhere. */
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
    g_hash_table_insert(self->by_symbol, g_strdup(e->symbol), item);
  }

  guint prefix = 0;
  while (prefix < n && prefix < old_n &&
         strcmp(stk_watchlist_get_entry(self->watchlist, prefix)->symbol,
                store_symbol_at(self->items, prefix)) == 0)
    prefix++;
  guint suffix = 0;
  while (suffix < n - prefix && suffix < old_n - prefix &&
         strcmp(stk_watchlist_get_entry(self->watchlist, n - 1 - suffix)->symbol,
                store_symbol_at(self->items, old_n - 1 - suffix)) == 0)
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

static void
refresh_stale(StkApp *self, const char *symbol)
{
  StkRowItem *item = lookup_item(self, symbol);
  if (item != NULL)
    stk_row_item_set_stale(item, stk_scheduler_is_stale(self->sched, symbol));
}

static void
request_free(Request *r)
{
  g_weak_ref_clear(&r->app);
  g_object_unref(r->cancellable);
  g_free(r);
}

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

/* Ends request @r (taken): hands its result to the scheduler, re-reads the
 * stale mark of its symbol and frees it. */
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
  gboolean matched = FALSE;
  gboolean changed = FALSE;
  if (series->range == STK_SCHED_SPARK_RANGE) {
    matched = TRUE;
    changed |= stk_row_item_set_spark(item, series);
  }
  if (self->sel_symbol != NULL && strcmp(self->sel_symbol, r->symbol) == 0 &&
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

/* Keeps exactly the timer the scheduler asks for: none, or one at its deadline. */
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

static void
on_watchlist_changed(StkApp *self)
{
  sync_items(self);
  sync_scheduler_symbols(self);
  schedule_save(self);
}

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

static void
on_network_changed(GNetworkMonitor *monitor, gboolean available, gpointer user_data)
{
  (void)available;
  set_offline(STK_APP(user_data), !g_network_monitor_get_network_available(monitor));
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

StkApp *
stk_app_new(void)
{
  return g_object_new(STK_TYPE_APP, "application-id", STK_APP_ID, "flags", G_APPLICATION_FLAGS_NONE,
                      NULL);
}

GListModel *
stk_app_get_items(StkApp *self)
{
  g_return_val_if_fail(STK_IS_APP(self), NULL);
  return G_LIST_MODEL(self->items);
}

StkWatchlist *
stk_app_get_watchlist(StkApp *self)
{
  g_return_val_if_fail(STK_IS_APP(self), NULL);
  return self->watchlist;
}

StkProvider *
stk_app_get_provider(StkApp *self)
{
  g_return_val_if_fail(STK_IS_APP(self), NULL);
  return self->provider;
}

const StkCatalog *
stk_app_get_catalog(StkApp *self)
{
  g_return_val_if_fail(STK_IS_APP(self), NULL);
  if (self->catalog == NULL && !self->catalog_failed) {
    g_autoptr(GError) error = NULL;
    self->catalog = stk_catalog_load_default(&error);
    if (self->catalog == NULL) {
      self->catalog_failed = TRUE;
      g_warning("Cannot load the Fidelity catalog: %s", error->message);
    }
  }
  return self->catalog;
}

gboolean
stk_app_get_offline(StkApp *self)
{
  g_return_val_if_fail(STK_IS_APP(self), FALSE);
  return self->offline;
}

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

static void
load_result_free(LoadResult *r)
{
  g_free(r->watchlist_error);
  g_free(r->cache_error);
  g_free(r);
}

/* Runs on a worker thread. The watchlist and the cache are touched by nobody
 * else until the task has completed on the main thread, which is the hand-off
 * that makes it correct for these main-thread-owned objects. A missing file
 * is not an error. */
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
    r->watchlist_error = g_strdup(wl_error->message);

  g_autoptr(GError) cache_error = NULL;
  if (!stk_cache_load(self->cache, &cache_error) &&
      !g_error_matches(cache_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    r->cache_error = g_strdup(cache_error->message);

  g_task_return_pointer(task, r, (GDestroyNotify)load_result_free);
}

static void present_window(StkApp *self);

static void
load_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)user_data;
  StkApp *self = STK_APP(source);
  LoadResult *r = g_task_propagate_pointer(G_TASK(result), NULL);
  self->loading = FALSE;

  if (r->watchlist_error != NULL)
    g_warning("Could not read the watchlist (starting empty): %s", r->watchlist_error);
  if (r->n_dropped > 0)
    g_warning("%u invalid watchlist entries were dropped", r->n_dropped);
  if (r->cache_error != NULL)
    g_warning("Could not read the cache (starting empty): %s", r->cache_error);
  load_result_free(r);

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
  g_autofree char *text = g_strndup(data, len);
  g_autoptr(GtkCssProvider) provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, text);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                             GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

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

static void
present_window(StkApp *self)
{
  GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(self));
  if (window == NULL)
    window = GTK_WINDOW(stk_window_new(self));
  gtk_window_present(window);
}

static void
stk_app_activate(GApplication *application)
{
  StkApp *self = STK_APP(application);
  if (self->loaded)
    present_window(self);
  else
    self->activate_pending = TRUE;
}

static gboolean
drain_timeout(gpointer user_data)
{
  StkApp *self = STK_APP(user_data);
  self->drain_id = 0;
  g_warning("Gave up waiting for pending saves/requests after %d ms", STK_APP_SHUTDOWN_WAIT_MS);
  g_main_loop_quit(self->drain_loop);
  return G_SOURCE_REMOVE;
}

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

static void
stk_app_init(StkApp *self)
{
  self->provider = STK_PROVIDER(stk_nasdaq_new());
  self->watchlist = stk_watchlist_new(NULL);
  self->cache = stk_cache_new(NULL);
  self->items = g_list_store_new(STK_TYPE_ROW_ITEM);
  self->by_symbol = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->sched = stk_scheduler_new();
  self->requests = g_hash_table_new(g_int64_hash, g_int64_equal);
  self->sel_range = STK_RANGE_1D;
  self->wake_us = -1;
}
