/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
#include "stk-types.h"

#include <string.h>

/* stk_error_quark(): defined by G_DEFINE_QUARK below. Documented in stk-types.h
 * (no parameters; returns the quark of the STK_ERROR domain; any thread). */
G_DEFINE_QUARK(stk-error-quark, stk_error)

/* Longest symbol (without the NUL) accepted by stk_symbol_is_valid(); every
 * fixed symbol buffer in the project is STK_SYMBOL_MAX_LEN + 1 = 16 bytes. */
#define STK_SYMBOL_MAX_LEN 15

/* ------------------------------------------------------------------------ */
/* Enumerations                                                             */
/* ------------------------------------------------------------------------ */

static const char *const kind_tokens[] = {
  [STK_KIND_STOCK] = "stock",
  [STK_KIND_ETF] = "etf",
  [STK_KIND_FUND] = "fund",
  [STK_KIND_INDEX] = "index",
};

static const char *const range_tokens[] = {
  [STK_RANGE_1D] = "1D",
  [STK_RANGE_1M] = "1M",
  [STK_RANGE_3M] = "3M",
  [STK_RANGE_6M] = "6M",
  [STK_RANGE_1Y] = "1Y",
};

/* Documented in stk-types.h. */
const char *
stk_kind_to_string(StkKind kind)
{
  g_return_val_if_fail((guint)kind < G_N_ELEMENTS(kind_tokens), kind_tokens[STK_KIND_STOCK]);
  return kind_tokens[kind];
}

/* Documented in stk-types.h. */
gboolean
stk_kind_from_string(const char *str, StkKind *out)
{
  g_return_val_if_fail(out != NULL, FALSE);
  if (str == NULL)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS(kind_tokens); i++) {
    if (g_str_equal(str, kind_tokens[i])) {
      *out = (StkKind)i;
      return TRUE;
    }
  }
  return FALSE;
}

/* Documented in stk-types.h. */
const char *
stk_range_to_string(StkRange range)
{
  g_return_val_if_fail((guint)range < G_N_ELEMENTS(range_tokens), range_tokens[STK_RANGE_1D]);
  return range_tokens[range];
}

/* Documented in stk-types.h. */
gboolean
stk_range_from_string(const char *str, StkRange *out)
{
  g_return_val_if_fail(out != NULL, FALSE);
  if (str == NULL)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS(range_tokens); i++) {
    if (g_str_equal(str, range_tokens[i])) {
      *out = (StkRange)i;
      return TRUE;
    }
  }
  return FALSE;
}

/* ------------------------------------------------------------------------ */
/* StkSeries                                                                */
/* ------------------------------------------------------------------------ */

/* Documented in stk-types.h. */
StkSeries *
stk_series_new(StkRange range, double prev_close, gsize capacity_hint)
{
  g_return_val_if_fail((guint)range < G_N_ELEMENTS(range_tokens), NULL);

  StkSeries *s = g_new0(StkSeries, 1);
  s->range = range;
  s->prev_close = isfinite(prev_close) ? prev_close : NAN;
  g_atomic_ref_count_init(&s->ref_count);
  if (capacity_hint > 0) {
    s->points = g_new(StkPoint, capacity_hint);
    s->n_alloc = capacity_hint;
  }
  return s;
}

/* Documented in stk-types.h. */
StkSeries *
stk_series_ref(StkSeries *series)
{
  g_return_val_if_fail(series != NULL, NULL);
  g_atomic_ref_count_inc(&series->ref_count);
  return series;
}

/* Documented in stk-types.h. */
void
stk_series_unref(StkSeries *series)
{
  g_return_if_fail(series != NULL);
  if (g_atomic_ref_count_dec(&series->ref_count)) {
    /* `points` is exposed read-only (const) to readers, but the block is always
     * allocated writable by stk_series_new()/stk_series_append() (g_new/g_renew)
     * and owned solely by the series, so releasing it through a non-const
     * pointer is well defined. */
    g_free((StkPoint *)series->points);
    g_free(series);
  }
}

/* Documented in stk-types.h. */
gboolean
stk_series_append(StkSeries *series, gint64 t_ms, double v)
{
  g_return_val_if_fail(series != NULL, FALSE);

  if (!isfinite(v))
    return FALSE;
  if (series->n_points > 0 && t_ms < series->points[series->n_points - 1].t_ms)
    return FALSE;

  if (series->n_points == series->n_alloc) {
    /* n_alloc * 2 cannot wrap in practice: n_alloc counts allocated StkPoint (16 bytes each), so
     * a value near G_MAXSIZE / 2 would already have made g_renew() abort (allocation overflow)
     * long before. */
    gsize new_alloc = series->n_alloc ? series->n_alloc * 2 : 64;
    series->points = g_renew(StkPoint, (StkPoint *)series->points, new_alloc);
    series->n_alloc = new_alloc;
  }

  StkPoint *pts = (StkPoint *)series->points;
  pts[series->n_points].t_ms = t_ms;
  pts[series->n_points].v = v;
  series->n_points++;
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* StkQuote                                                                 */
/* ------------------------------------------------------------------------ */

/* Documented in stk-types.h. */
gboolean
stk_symbol_is_valid(const char *symbol)
{
  if (symbol == NULL)
    return FALSE;
  /* Bounded length: look for the NUL within the first STK_SYMBOL_MAX_LEN + 1
   * bytes only; no NUL there means longer than 15 bytes (or unterminated). */
  const char *nul = memchr(symbol, '\0', STK_SYMBOL_MAX_LEN + 1);
  if (nul == NULL)
    return FALSE;
  gsize len = (gsize)(nul - symbol);
  if (len < 1)
    return FALSE;
  if (!g_ascii_isalnum(symbol[0]) && symbol[0] != '^')
    return FALSE;
  for (gsize i = 1; i < len; i++) {
    char c = symbol[i];
    if (!g_ascii_isalnum(c) && c != '.' && c != '-' && c != '^' && c != '=')
      return FALSE;
  }
  return TRUE;
}

/* Documented in stk-types.h. */
StkQuote *
stk_quote_new(const char *symbol, const char *name, StkKind kind, const char *currency)
{
  g_return_val_if_fail(stk_symbol_is_valid(symbol), NULL);
  g_return_val_if_fail((guint)kind < G_N_ELEMENTS(kind_tokens), NULL);

  StkQuote *q = g_new0(StkQuote, 1);
  q->symbol = g_ascii_strup(symbol, -1); /* symbol is validated: NUL within 16 bytes */
  q->name = g_strdup(name != NULL ? name : "");
  q->kind = kind;
  q->price = NAN;
  q->change = NAN;
  q->change_pct = NAN;
  q->currency = g_strdup(currency != NULL ? currency : "USD");
  return q;
}

/* Documented in stk-types.h. */
StkQuote *
stk_quote_copy(const StkQuote *quote)
{
  g_return_val_if_fail(quote != NULL, NULL);

  StkQuote *q = g_memdup2(quote, sizeof *q);
  q->symbol = g_strdup(quote->symbol);
  q->name = g_strdup(quote->name);
  q->currency = g_strdup(quote->currency);
  return q;
}

/* Documented in stk-types.h. */
void
stk_quote_free(StkQuote *quote)
{
  if (quote == NULL)
    return;
  g_free(quote->symbol);
  g_free(quote->name);
  g_free(quote->currency);
  g_free(quote);
}
