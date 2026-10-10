/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-router.c - StkProvider that routes by symbol. See stk-router.h for the
 * behaviour contract.
 *
 * Every operation is a GTask of the router (so the caller's callback sees the
 * router as source object) that forwards to one inner provider with the
 * caller's cancellable and returns the inner result unchanged.
 */
#include "stk-router.h"

struct _StkRouter {
  GObject parent_instance;
  StkProvider *primary;
  StkProvider *index_provider;
};

static void stk_router_provider_init(StkProviderInterface *iface);

/*
 * G_DEFINE_TYPE_WITH_CODE() generates stk_router_get_type() and the *_class_intern_init() /
 * stk_router_parent_class plumbing; it implements the StkProvider interface through
 * stk_router_provider_init(). The GObject boilerplate it generates holds no state of its own.
 * Thread affinity: type registration is thread-safe (GOnce); ownership: none (no pointers
 * are passed by the application).
 */
G_DEFINE_TYPE_WITH_CODE(StkRouter, stk_router, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(STK_TYPE_PROVIDER, stk_router_provider_init))

/* Addresses only (never read): the source tags that tell the three finish functions apart. */
static char tag_quote, tag_series, tag_search;

/**
 * stk_router_init:
 * @self: (not nullable): the new instance
 *
 * GObject instance initialiser: nothing to do; primary and index_provider stay NULL until
 * stk_router_new() sets them.
 *
 * Preconditions:
 *  - @self is a zero-initialised StkRouter instance being constructed by GObject.
 * Postconditions:
 *  - @self is unchanged.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @self stays with GObject; nothing is retained.
 */
static void
stk_router_init(StkRouter *self)
{
  (void)self;
}

/**
 * stk_router_dispose:
 * @object: (not nullable): the StkRouter being disposed
 *
 * GObject dispose: drops the references to the two inner providers. Operations hold a reference
 * to the router (the source object of their task), so none is outstanding here.
 *
 * Preconditions:
 *  - @object is a StkRouter; may run more than once (g_clear_object() makes that safe).
 * Postconditions:
 *  - primary and index_provider are NULL; the parent dispose has run.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the two provider references owned by the router are released; @object stays with the
 *   caller.
 */
static void
stk_router_dispose(GObject *object)
{
  StkRouter *self = STK_ROUTER(object);
  g_clear_object(&self->primary);
  g_clear_object(&self->index_provider);
  G_OBJECT_CLASS(stk_router_parent_class)->dispose(object);
}

/**
 * stk_router_class_init:
 * @klass: (not nullable): the class structure being initialised
 *
 * GObject class initialiser: installs dispose.
 *
 * Preconditions:
 *  - called once by the GType system.
 * Postconditions:
 *  - dispose is overridden; no properties or signals exist.
 * Thread affinity: any thread (called by GObject under its type lock).
 * Ownership: @klass is owned by the type system and not retained.
 */
static void
stk_router_class_init(StkRouterClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = stk_router_dispose;
}

/* Documented in stk-router.h. */
StkRouter *
stk_router_new(StkProvider *primary, StkProvider *index_provider)
{
  g_return_val_if_fail(STK_IS_PROVIDER(primary), NULL);
  g_return_val_if_fail(STK_IS_PROVIDER(index_provider), NULL);
  StkRouter *self = g_object_new(STK_TYPE_ROUTER, NULL);
  self->primary = g_object_ref(primary);
  self->index_provider = g_object_ref(index_provider);
  return self;
}

/**
 * provider_for:
 * @self: (not nullable): the router
 * @symbol: (not nullable): the requested symbol
 *
 * Picks the provider that serves @symbol: '^' symbols are indices.
 *
 * Preconditions:
 *  - @self is a StkRouter with both providers set (stk_router_new()); @symbol is non-NULL and
 *    NUL-terminated: the callers router_fetch_quote_async() and router_fetch_series_async() check
 *    it before calling, so symbol[0] (at worst the terminator) is readable.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @self and @symbol stay with the caller; the returned provider is owned by @self.
 *
 * Returns: (transfer none): index_provider if the first byte of @symbol is '^', else primary.
 */
static StkProvider *
provider_for(StkRouter *self, const char *symbol)
{
  return symbol[0] == '^' ? self->index_provider : self->primary;
}

/**
 * quote_destroy:
 * @quote: (not nullable) (transfer full): a #StkQuote, passed as gpointer
 *
 * GDestroyNotify of a quote task result that was never propagated.
 *
 * Preconditions:
 *  - @quote is a quote owned by the task result.
 * Postconditions:
 *  - the quote is freed with stk_quote_free().
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @quote.
 */
static void
quote_destroy(gpointer quote)
{
  stk_quote_free(quote);
}

/**
 * series_destroy:
 * @series: (not nullable) (transfer full): a #StkSeries, passed as gpointer
 *
 * GDestroyNotify of a series task result that was never propagated.
 *
 * Preconditions:
 *  - @series is a series reference owned by the task result.
 * Postconditions:
 *  - the reference is dropped with stk_series_unref().
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of one reference to @series.
 */
static void
series_destroy(gpointer series)
{
  stk_series_unref(series);
}

/**
 * hits_destroy:
 * @hits: (not nullable) (transfer full): a #GPtrArray of #StkSearchHit, passed as gpointer
 *
 * GDestroyNotify of a search task result that was never propagated.
 *
 * Preconditions:
 *  - @hits is an array reference owned by the task result.
 * Postconditions:
 *  - the reference is dropped; the array and its hits are freed with it.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of one reference to @hits.
 */
static void
hits_destroy(gpointer hits)
{
  g_ptr_array_unref(hits);
}

/**
 * on_inner_quote:
 * @source: (not nullable): the inner provider that was called, passed as GObject
 * @result: (not nullable): the inner provider's result
 * @user_data: (transfer full): the router's GTask for this operation, whose reference is ours
 *
 * GAsyncReadyCallback of the inner quote operation: moves its result (quote or error) into the
 * router's task.
 *
 * Preconditions:
 *  - @source is a StkProvider and @result its pending result; @user_data is the task created in
 *    router_fetch_quote_async(). The inner provider follows the StkProvider contract: a NULL quote
 *    comes with @error set (g_task_return_error() requires a non-NULL error).
 * Postconditions:
 *  - the task is completed exactly once, with the quote (owned by the task result until the
 *    caller propagates it) or the inner error. The task reference is released on return; the
 *    caller's callback may run before that.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes the task reference in @user_data (released through the g_autoptr); the quote
 *   and the error are handed to the task; @source and @result stay with the caller.
 */
static void
on_inner_quote(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  StkQuote *quote = stk_provider_fetch_quote_finish(STK_PROVIDER(source), result, &error);
  if (quote != NULL)
    g_task_return_pointer(task, quote, quote_destroy);
  else
    g_task_return_error(task, error);
}

/**
 * on_inner_series:
 * @source: (not nullable): the inner provider that was called, passed as GObject
 * @result: (not nullable): the inner provider's result
 * @user_data: (transfer full): the router's GTask for this operation, whose reference is ours
 *
 * GAsyncReadyCallback of the inner series operation: moves its result (series or error) into the
 * router's task.
 *
 * Preconditions:
 *  - @source is a StkProvider and @result its pending result; @user_data is the task created in
 *    router_fetch_series_async(). The inner provider follows the StkProvider contract: a NULL
 *    series comes with @error set (g_task_return_error() requires a non-NULL error).
 * Postconditions:
 *  - the task is completed exactly once, with the series (one reference, owned by the task result
 *    until the caller propagates it) or the inner error. The task reference is released on
 *    return; the caller's callback may run before that.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes the task reference in @user_data (released through the g_autoptr); the series
 *   and the error are handed to the task; @source and @result stay with the caller.
 */
static void
on_inner_series(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  StkSeries *series = stk_provider_fetch_series_finish(STK_PROVIDER(source), result, &error);
  if (series != NULL)
    g_task_return_pointer(task, series, series_destroy);
  else
    g_task_return_error(task, error);
}

/**
 * on_inner_search:
 * @source: (not nullable): the inner provider that was called, passed as GObject
 * @result: (not nullable): the inner provider's result
 * @user_data: (transfer full): the router's GTask for this operation, whose reference is ours
 *
 * GAsyncReadyCallback of the inner search operation: moves its result (hit array or error) into
 * the router's task.
 *
 * Preconditions:
 *  - @source is a StkProvider and @result its pending result; @user_data is the task created in
 *    router_search_async(). The inner provider follows the StkProvider contract: a NULL array
 *    comes with @error set (g_task_return_error() requires a non-NULL error).
 * Postconditions:
 *  - the task is completed exactly once, with the hit array (owned by the task result until the
 *    caller propagates it) or the inner error. The task reference is released on return; the
 *    caller's callback may run before that.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes the task reference in @user_data (released through the g_autoptr); the array
 *   and the error are handed to the task; @source and @result stay with the caller.
 */
static void
on_inner_search(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  GPtrArray *hits = stk_provider_search_finish(STK_PROVIDER(source), result, &error);
  if (hits != NULL)
    g_task_return_pointer(task, hits, hits_destroy);
  else
    g_task_return_error(task, error);
}

/**
 * router_fetch_quote_async:
 * @provider: (not nullable): the StkRouter
 * @symbol: (not nullable): the requested symbol
 * @kind: the requested kind, passed through unchanged
 * @cancellable: (nullable): the caller's cancellable, passed to the inner operation
 * @callback: (scope async): called once with the result
 * @user_data: passed to @callback
 *
 * StkProviderInterface.fetch_quote_async: forwards the request to the index provider for a symbol
 * whose first byte is '^', else to the primary provider (see stk-router.h).
 *
 * Preconditions:
 *  - @provider is a StkRouter (checked by the interface dispatcher); @symbol is non-NULL and
 *    NUL-terminated (checked by the dispatcher and, since provider_for() reads it, again here:
 *    a NULL symbol is a g_return_if_fail() critical that starts nothing).
 * Postconditions:
 *  - a task of the router (source tag tag_quote) is created and the inner provider is called with
 *    @symbol, @kind and @cancellable; @callback is called once, with the inner result, by the
 *    task. Validation and every error are the inner provider's.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the task takes references on @provider and @cancellable and a reference to the task
 *   is passed to the inner operation; @symbol is only passed on, not retained here; @user_data is
 *   only passed on.
 */
static void
router_fetch_quote_async(StkProvider *provider, const char *symbol, StkKind kind,
                         GCancellable *cancellable, GAsyncReadyCallback callback,
                         gpointer user_data)
{
  StkRouter *self = STK_ROUTER(provider);
  g_return_if_fail(symbol != NULL);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, &tag_quote);
  stk_provider_fetch_quote_async(provider_for(self, symbol), symbol, kind, cancellable,
                                 on_inner_quote, task);
}

/**
 * router_fetch_quote_finish:
 * @provider: (not nullable): the StkRouter the request was started on
 * @result: (not nullable): the result passed to the callback
 * @error: (nullable): return location for a #GError
 *
 * StkProviderInterface.fetch_quote_finish.
 *
 * Preconditions:
 *  - @result is a GTask created by router_fetch_quote_async() for @provider and not yet finished
 *    (checked with g_return_val_if_fail()).
 * Postconditions:
 *  - on failure NULL is returned and *@error is set; the task result is consumed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the quote is returned to the caller; @provider and @result stay with the caller.
 *
 * Returns: (transfer full) (nullable): the quote, or NULL with @error set.
 */
static StkQuote *
router_fetch_quote_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_quote, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/**
 * router_fetch_series_async:
 * @provider: (not nullable): the StkRouter
 * @symbol: (not nullable): the requested symbol
 * @kind: the requested kind, passed through unchanged
 * @range: the requested range, passed through unchanged
 * @cancellable: (nullable): the caller's cancellable, passed to the inner operation
 * @callback: (scope async): called once with the result
 * @user_data: passed to @callback
 *
 * StkProviderInterface.fetch_series_async: forwards the request to the index provider for a
 * symbol whose first byte is '^', else to the primary provider (see stk-router.h).
 *
 * Preconditions:
 *  - @provider is a StkRouter (checked by the interface dispatcher); @symbol is non-NULL and
 *    NUL-terminated (checked by the dispatcher and, since provider_for() reads it, again here:
 *    a NULL symbol is a g_return_if_fail() critical that starts nothing).
 * Postconditions:
 *  - a task of the router (source tag tag_series) is created and the inner provider is called with
 *    @symbol, @kind, @range and @cancellable; @callback is called once, with the inner result, by
 *    the task. Validation and every error are the inner provider's.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the task takes references on @provider and @cancellable and a reference to the task
 *   is passed to the inner operation; @symbol is only passed on, not retained here; @user_data is
 *   only passed on.
 */
static void
router_fetch_series_async(StkProvider *provider, const char *symbol, StkKind kind, StkRange range,
                          GCancellable *cancellable, GAsyncReadyCallback callback,
                          gpointer user_data)
{
  StkRouter *self = STK_ROUTER(provider);
  g_return_if_fail(symbol != NULL);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, &tag_series);
  stk_provider_fetch_series_async(provider_for(self, symbol), symbol, kind, range, cancellable,
                                  on_inner_series, task);
}

/**
 * router_fetch_series_finish:
 * @provider: (not nullable): the StkRouter the request was started on
 * @result: (not nullable): the result passed to the callback
 * @error: (nullable): return location for a #GError
 *
 * StkProviderInterface.fetch_series_finish.
 *
 * Preconditions:
 *  - @result is a GTask created by router_fetch_series_async() for @provider and not yet finished
 *    (checked with g_return_val_if_fail()).
 * Postconditions:
 *  - on failure NULL is returned and *@error is set; the task result is consumed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the series is returned to the caller; @provider and @result stay with the caller.
 *
 * Returns: (transfer full) (nullable): the series, or NULL with @error set.
 */
static StkSeries *
router_fetch_series_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_series, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/**
 * router_search_async:
 * @provider: (not nullable): the StkRouter
 * @text: (not nullable): the search text, passed through unchanged
 * @cancellable: (nullable): the caller's cancellable, passed to the inner operation
 * @callback: (scope async): called once with the result
 * @user_data: passed to @callback
 *
 * StkProviderInterface.search_async: forwards the search to the primary provider only.
 *
 * Preconditions:
 *  - @provider is a StkRouter and @text is non-NULL and NUL-terminated (both checked by the
 *    interface dispatcher, which the inner provider's own dispatcher repeats).
 * Postconditions:
 *  - a task of the router (source tag tag_search) is created and the primary provider is called
 *    with @text and @cancellable; @callback is called once, with the inner result, by the task.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the task takes references on @provider and @cancellable and a reference to the task
 *   is passed to the inner operation; @text is only passed on, not retained here; @user_data is
 *   only passed on.
 */
static void
router_search_async(StkProvider *provider, const char *text, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer user_data)
{
  StkRouter *self = STK_ROUTER(provider);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, &tag_search);
  stk_provider_search_async(self->primary, text, cancellable, on_inner_search, task);
}

/**
 * router_search_finish:
 * @provider: (not nullable): the StkRouter the request was started on
 * @result: (not nullable): the result passed to the callback
 * @error: (nullable): return location for a #GError
 *
 * StkProviderInterface.search_finish.
 *
 * Preconditions:
 *  - @result is a GTask created by router_search_async() for @provider and not yet finished
 *    (checked with g_return_val_if_fail()).
 * Postconditions:
 *  - on failure NULL is returned and *@error is set; the task result is consumed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the array is returned to the caller; @provider and @result stay with the caller.
 *
 * Returns: (transfer full) (nullable) (element-type StkSearchHit): the hits, or NULL with @error
 *   set.
 */
static GPtrArray *
router_search_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_search, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/**
 * stk_router_provider_init:
 * @iface: (not nullable): the interface vtable of StkRouter
 *
 * GObject interface initialiser: wires the six StkProviderInterface virtual functions.
 *
 * Preconditions:
 *  - called once by the GType system; @iface is writable.
 * Postconditions:
 *  - all six virtual functions are set.
 * Thread affinity: any thread (called by GObject under its type lock).
 * Ownership: @iface is owned by the type system and not retained.
 */
static void
stk_router_provider_init(StkProviderInterface *iface)
{
  iface->fetch_quote_async = router_fetch_quote_async;
  iface->fetch_quote_finish = router_fetch_quote_finish;
  iface->fetch_series_async = router_fetch_series_async;
  iface->fetch_series_finish = router_fetch_series_finish;
  iface->search_async = router_search_async;
  iface->search_finish = router_search_finish;
}
