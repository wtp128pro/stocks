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

G_DEFINE_TYPE_WITH_CODE(StkRouter, stk_router, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(STK_TYPE_PROVIDER, stk_router_provider_init))

static char tag_quote, tag_series, tag_search;

static void
stk_router_init(StkRouter *self)
{
  (void)self;
}

static void
stk_router_dispose(GObject *object)
{
  StkRouter *self = STK_ROUTER(object);
  g_clear_object(&self->primary);
  g_clear_object(&self->index_provider);
  G_OBJECT_CLASS(stk_router_parent_class)->dispose(object);
}

static void
stk_router_class_init(StkRouterClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = stk_router_dispose;
}

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

/* The provider that serves @symbol: '^' symbols are indices. */
static StkProvider *
provider_for(StkRouter *self, const char *symbol)
{
  return symbol[0] == '^' ? self->index_provider : self->primary;
}

static void
quote_destroy(gpointer quote)
{
  stk_quote_free(quote);
}

static void
series_destroy(gpointer series)
{
  stk_series_unref(series);
}

static void
hits_destroy(gpointer hits)
{
  g_ptr_array_unref(hits);
}

/* Inner callbacks: @user_data is the router's task (its reference is ours).
 * The source object is the inner provider that was called. */
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

static void
router_fetch_quote_async(StkProvider *provider, const char *symbol, StkKind kind,
                         GCancellable *cancellable, GAsyncReadyCallback callback,
                         gpointer user_data)
{
  StkRouter *self = STK_ROUTER(provider);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, &tag_quote);
  stk_provider_fetch_quote_async(provider_for(self, symbol), symbol, kind, cancellable,
                                 on_inner_quote, task);
}

static StkQuote *
router_fetch_quote_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_quote, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
router_fetch_series_async(StkProvider *provider, const char *symbol, StkKind kind, StkRange range,
                          GCancellable *cancellable, GAsyncReadyCallback callback,
                          gpointer user_data)
{
  StkRouter *self = STK_ROUTER(provider);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, &tag_series);
  stk_provider_fetch_series_async(provider_for(self, symbol), symbol, kind, range, cancellable,
                                  on_inner_series, task);
}

static StkSeries *
router_fetch_series_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_series, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
router_search_async(StkProvider *provider, const char *text, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer user_data)
{
  StkRouter *self = STK_ROUTER(provider);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, &tag_search);
  stk_provider_search_async(self->primary, text, cancellable, on_inner_search, task);
}

static GPtrArray *
router_search_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_search, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

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
