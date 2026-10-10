/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/* stk-provider.c - StkProvider interface dispatch. See stk-provider.h. */
#include "stk-provider.h"

G_DEFINE_INTERFACE(StkProvider, stk_provider, G_TYPE_OBJECT)

/**
 * stk_provider_default_init:
 * @iface: (not nullable): the interface vtable being initialised by GObject.
 *
 * GObject interface default initialiser (installed by G_DEFINE_INTERFACE()). The
 * interface has no properties, signals or default implementations, so nothing
 * is done.
 *
 * Preconditions:
 *  - Called once by the GType system when the interface type is first used.
 * Postconditions:
 *  - @iface is untouched; every virtual function stays NULL until an
 *    implementation fills it in.
 * Thread affinity: any thread (called by GObject under its type lock).
 * Ownership: none (@iface is owned by the type system and not retained).
 */
static void
stk_provider_default_init(StkProviderInterface *iface)
{
  (void)iface;
}

/* Documented in stk-provider.h. */
void
stk_search_hit_free(StkSearchHit *hit)
{
  if (hit == NULL)
    return;
  g_free(hit->symbol);
  g_free(hit->name);
  g_free(hit);
}

/* Documented in stk-provider.h. */
void
stk_provider_fetch_quote_async(StkProvider *self, const char *symbol, StkKind kind,
                               GCancellable *cancellable, GAsyncReadyCallback callback,
                               gpointer user_data)
{
  g_return_if_fail(STK_IS_PROVIDER(self));
  g_return_if_fail(symbol != NULL);
  g_return_if_fail(cancellable == NULL || G_IS_CANCELLABLE(cancellable));
  g_return_if_fail(STK_PROVIDER_GET_IFACE(self)->fetch_quote_async != NULL);
  STK_PROVIDER_GET_IFACE(self)->fetch_quote_async(self, symbol, kind, cancellable, callback,
                                                  user_data);
}

/* Documented in stk-provider.h. */
StkQuote *
stk_provider_fetch_quote_finish(StkProvider *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(STK_IS_PROVIDER(self), NULL);
  g_return_val_if_fail(G_IS_ASYNC_RESULT(result), NULL);
  g_return_val_if_fail(STK_PROVIDER_GET_IFACE(self)->fetch_quote_finish != NULL, NULL);
  return STK_PROVIDER_GET_IFACE(self)->fetch_quote_finish(self, result, error);
}

/* Documented in stk-provider.h. */
void
stk_provider_fetch_series_async(StkProvider *self, const char *symbol, StkKind kind,
                                StkRange range, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(STK_IS_PROVIDER(self));
  g_return_if_fail(symbol != NULL);
  g_return_if_fail(cancellable == NULL || G_IS_CANCELLABLE(cancellable));
  g_return_if_fail(STK_PROVIDER_GET_IFACE(self)->fetch_series_async != NULL);
  STK_PROVIDER_GET_IFACE(self)->fetch_series_async(self, symbol, kind, range, cancellable,
                                                   callback, user_data);
}

/* Documented in stk-provider.h. */
StkSeries *
stk_provider_fetch_series_finish(StkProvider *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(STK_IS_PROVIDER(self), NULL);
  g_return_val_if_fail(G_IS_ASYNC_RESULT(result), NULL);
  g_return_val_if_fail(STK_PROVIDER_GET_IFACE(self)->fetch_series_finish != NULL, NULL);
  return STK_PROVIDER_GET_IFACE(self)->fetch_series_finish(self, result, error);
}

/* Documented in stk-provider.h. */
void
stk_provider_search_async(StkProvider *self, const char *text, GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(STK_IS_PROVIDER(self));
  g_return_if_fail(text != NULL);
  g_return_if_fail(cancellable == NULL || G_IS_CANCELLABLE(cancellable));
  g_return_if_fail(STK_PROVIDER_GET_IFACE(self)->search_async != NULL);
  STK_PROVIDER_GET_IFACE(self)->search_async(self, text, cancellable, callback, user_data);
}

/* Documented in stk-provider.h. */
GPtrArray *
stk_provider_search_finish(StkProvider *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(STK_IS_PROVIDER(self), NULL);
  g_return_val_if_fail(G_IS_ASYNC_RESULT(result), NULL);
  g_return_val_if_fail(STK_PROVIDER_GET_IFACE(self)->search_finish != NULL, NULL);
  return STK_PROVIDER_GET_IFACE(self)->search_finish(self, result, error);
}
