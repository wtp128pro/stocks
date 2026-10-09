#include "stk-types.h"

#include <string.h>

G_DEFINE_QUARK(stk-error-quark, stk_error)

/* ------------------------------------------------------------------------ */
/* Enumerations                                                             */
/* ------------------------------------------------------------------------ */

static const char *const kind_tokens[] = {
  [STK_KIND_STOCK] = "stock",
  [STK_KIND_ETF] = "etf",
  [STK_KIND_FUND] = "fund",
};

static const char *const range_tokens[] = {
  [STK_RANGE_1D] = "1D",
  [STK_RANGE_1M] = "1M",
  [STK_RANGE_3M] = "3M",
  [STK_RANGE_6M] = "6M",
  [STK_RANGE_1Y] = "1Y",
};

const char *
stk_kind_to_string(StkKind kind)
{
  g_return_val_if_fail((guint)kind < G_N_ELEMENTS(kind_tokens), kind_tokens[STK_KIND_STOCK]);
  return kind_tokens[kind];
}

gboolean
stk_kind_from_string(const char *str, StkKind *out)
{
  g_return_val_if_fail(out != NULL, FALSE);
  if (str == NULL)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS(kind_tokens); i++) {
    if (strcmp(str, kind_tokens[i]) == 0) {
      *out = (StkKind)i;
      return TRUE;
    }
  }
  return FALSE;
}

const char *
stk_range_to_string(StkRange range)
{
  g_return_val_if_fail((guint)range < G_N_ELEMENTS(range_tokens), range_tokens[STK_RANGE_1D]);
  return range_tokens[range];
}

gboolean
stk_range_from_string(const char *str, StkRange *out)
{
  g_return_val_if_fail(out != NULL, FALSE);
  if (str == NULL)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS(range_tokens); i++) {
    if (strcmp(str, range_tokens[i]) == 0) {
      *out = (StkRange)i;
      return TRUE;
    }
  }
  return FALSE;
}

/* ------------------------------------------------------------------------ */
/* StkSeries                                                                */
/* ------------------------------------------------------------------------ */

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

StkSeries *
stk_series_ref(StkSeries *series)
{
  g_return_val_if_fail(series != NULL, NULL);
  g_atomic_ref_count_inc(&series->ref_count);
  return series;
}

void
stk_series_unref(StkSeries *series)
{
  g_return_if_fail(series != NULL);
  if (g_atomic_ref_count_dec(&series->ref_count)) {
    g_free((StkPoint *)series->points);
    g_free(series);
  }
}

gboolean
stk_series_append(StkSeries *series, gint64 t_ms, double v)
{
  g_return_val_if_fail(series != NULL, FALSE);

  if (!isfinite(v))
    return FALSE;
  if (series->n_points > 0 && t_ms < series->points[series->n_points - 1].t_ms)
    return FALSE;

  if (series->n_points == series->n_alloc) {
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

gboolean
stk_symbol_is_valid(const char *symbol)
{
  if (symbol == NULL)
    return FALSE;
  gsize len = strlen(symbol);
  if (len < 1 || len > 15)
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

StkQuote *
stk_quote_new(const char *symbol, const char *name, StkKind kind, const char *currency)
{
  g_return_val_if_fail(stk_symbol_is_valid(symbol), NULL);
  g_return_val_if_fail((guint)kind < G_N_ELEMENTS(kind_tokens), NULL);

  StkQuote *q = g_new0(StkQuote, 1);
  q->symbol = g_ascii_strup(symbol, -1);
  q->name = g_strdup(name != NULL ? name : "");
  q->kind = kind;
  q->price = NAN;
  q->change = NAN;
  q->change_pct = NAN;
  q->currency = g_strdup(currency != NULL ? currency : "USD");
  return q;
}

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
