/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/* stk-row.c - StkRowItem model, StkRow widget and shared formatting. */
#include "stk-row.h"

#include <math.h>
#include <string.h>

#include "stk-sparkline.h"

/* ------------------------------------------------------------------------ */
/* Formatting                                                               */
/* ------------------------------------------------------------------------ */

#define HUGE_VALUE 1e12

/* Documented in stk-row.h. */
char *
stk_format_price(double value)
{
  if (!isfinite(value))
    return g_strdup(STK_MISSING_TEXT);
  if (fabs(value) >= HUGE_VALUE)
    return g_strdup_printf("%.3e", value);
  return g_strdup_printf("%.2f", value);
}

/**
 * displayed_sign:
 * @value: any double.
 *
 * The sign @value has once rounded to two decimals: -1, 0 or +1 (0 for NAN/inf, which display as
 * the em dash).
 *
 * Preconditions:
 *  - none; every double is accepted. The product value * 100.0 is only formed for
 *    finite |value| < HUGE_VALUE (1e12), so it is finite; no float-to-integer conversion is made.
 * Postconditions:
 *  - nothing is changed.
 * Thread affinity: any thread (no state).
 * Ownership: none (no pointers).
 *
 * Returns: -1 if the displayed value is negative, +1 if positive, 0 otherwise.
 */
static int
displayed_sign(double value)
{
  if (!isfinite(value))
    return 0;
  if (fabs(value) >= HUGE_VALUE)
    return value > 0 ? 1 : -1;
  double r = round(value * 100.0);
  return r > 0 ? 1 : (r < 0 ? -1 : 0);
}

/* Documented in stk-row.h. */
char *
stk_format_change(double value)
{
  if (!isfinite(value))
    return g_strdup(STK_MISSING_TEXT);
  const char *sign = displayed_sign(value) > 0 ? "+" : (displayed_sign(value) < 0 ? "-" : "");
  if (fabs(value) >= HUGE_VALUE)
    return g_strdup_printf("%s%.3e", sign, fabs(value));
  return g_strdup_printf("%s%.2f", sign, fabs(value));
}

/* Documented in stk-row.h. */
char *
stk_format_percent(double value)
{
  if (!isfinite(value))
    return g_strdup(STK_MISSING_TEXT);
  g_autofree char *num = stk_format_change(value);
  return g_strconcat(num, "%", NULL);
}

/* Documented in stk-row.h. */
const char *
stk_change_class(double value)
{
  int s = displayed_sign(value);
  return s > 0 ? "positive" : (s < 0 ? "negative" : "flat");
}

/* Documented in stk-row.h. */
char *
stk_sanitize_text(const char *text)
{
  if (text == NULL)
    return g_strdup("");

  /* Bound the work first: only the first STK_SANITIZE_MAX_BYTES bytes count,
   * cut so that no multi-byte sequence is split (memchr stops at the NUL, so
   * a short string is never read past its end). */
  const char *nul = memchr(text, '\0', STK_SANITIZE_MAX_BYTES + 1);
  gsize len = nul != NULL ? (gsize)(nul - text) : STK_SANITIZE_MAX_BYTES;
  /* memchr() examined bytes 0..STK_SANITIZE_MAX_BYTES, so text[len] with
   * len <= STK_SANITIZE_MAX_BYTES is readable; it is the NUL (no trimming) when
   * the string ended within the bound. */
  for (guint i = 0; i < 3 && len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80; i++)
    len--;

  char *out = g_utf8_make_valid(text, (gssize)len);
  /* One pass, in place over @out (a fresh, valid, NUL-terminated copy of at
   * most 3 * @len bytes without embedded NUL: @text[0..len) holds no NUL).
   * Every replacement is one ASCII byte, never longer than the 1..4 byte UTF-8
   * sequence it replaces, and a kept sequence is copied byte by byte forwards.
   * Invariant: @w <= @r at the top of every iteration (initially equal; each
   * iteration reads the character at @r first, advances @r by its length n >= 1
   * and @w by 1 or n). So nothing is shifted, writes stay below the read
   * position or on bytes already consumed, and the final *w lies inside @out. */
  char *w = out;
  for (const char *r = out; *r != '\0';) {
    const char *next = g_utf8_next_char(r);
    gunichar c = g_utf8_get_char(r);
    GUnicodeType t = g_unichar_type(c);
    if (g_unichar_iscntrl(c) || t == G_UNICODE_LINE_SEPARATOR ||
        t == G_UNICODE_PARAGRAPH_SEPARATOR || t == G_UNICODE_FORMAT) {
      *w++ = ' ';
      r = next;
    } else {
      while (r < next)
        *w++ = *r++;
    }
  }
  *w = '\0';
  return out;
}

/* ------------------------------------------------------------------------ */
/* StkRowItem                                                               */
/* ------------------------------------------------------------------------ */

/*
 * G_DEFINE_FINAL_TYPE() below defines stk_row_item_get_type(), the static
 * stk_row_item_parent_class pointer and the static class initialiser that runs
 * stk_row_item_class_init() once; stk_row_item_init() is registered as the
 * instance initialiser. The instance struct is _StkRowItem (StkRowItem).
 */
struct _StkRowItem {
  GObject parent_instance;
  char *symbol;
  char *name;
  StkKind kind;
  StkQuote *quote;
  StkSeries *spark;
  gboolean stale;
  StkRange detail_range;
  StkSeries *detail_series;
};

G_DEFINE_FINAL_TYPE(StkRowItem, stk_row_item, G_TYPE_OBJECT)

enum { ITEM_CHANGED, ITEM_DETAIL_CHANGED, N_ITEM_SIGNALS };
static guint item_signals[N_ITEM_SIGNALS];

/**
 * stk_row_item_finalize:
 * @object: the item being finalized.
 *
 * GObject finalize vfunc: frees the symbol, the name, the quote and drops the
 * series references, then chains up.
 *
 * Preconditions:
 *  - @object is a StkRowItem whose last reference was dropped (called by GObject once).
 * Postconditions:
 *  - every string, the quote and both series references owned by the item are released;
 *    the parent finalize has run.
 * Thread affinity: main thread only.
 * Ownership: @object is owned by GObject; the fields freed here are owned by the item.
 */
static void
stk_row_item_finalize(GObject *object)
{
  StkRowItem *self = STK_ROW_ITEM(object);
  g_free(self->symbol);
  g_free(self->name);
  g_clear_pointer(&self->quote, stk_quote_free);
  g_clear_pointer(&self->spark, stk_series_unref);
  g_clear_pointer(&self->detail_series, stk_series_unref);
  G_OBJECT_CLASS(stk_row_item_parent_class)->finalize(object);
}

/**
 * stk_row_item_class_init:
 * @klass: (not nullable): the class structure being initialised.
 *
 * GObject class initialiser: installs finalize() and registers the "changed" and
 * "detail-changed" signals (no parameters, G_SIGNAL_RUN_LAST).
 *
 * Preconditions:
 *  - called once by GObject while the type is registered.
 * Postconditions:
 *  - item_signals[] holds the two signal ids.
 * Thread affinity: main thread only.
 * Ownership: @klass is owned by GObject.
 */
static void
stk_row_item_class_init(StkRowItemClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = stk_row_item_finalize;
  item_signals[ITEM_CHANGED] =
      g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                   G_TYPE_NONE, 0);
  item_signals[ITEM_DETAIL_CHANGED] =
      g_signal_new("detail-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                   NULL, G_TYPE_NONE, 0);
}

/**
 * stk_row_item_init:
 * @self: (not nullable): the new instance (zero-filled by GObject).
 *
 * GObject instance initialiser: the detail range starts at STK_RANGE_1D; every
 * other field stays zero (no strings, no data).
 *
 * Preconditions:
 *  - called once by GObject for a new instance.
 * Postconditions:
 *  - @self->detail_range is STK_RANGE_1D.
 * Thread affinity: main thread only.
 * Ownership: @self is owned by GObject.
 */
static void
stk_row_item_init(StkRowItem *self)
{
  self->detail_range = STK_RANGE_1D;
}

/* Documented in stk-row.h. */
StkRowItem *
stk_row_item_new(const char *symbol, const char *name, StkKind kind)
{
  g_return_val_if_fail(stk_symbol_is_valid(symbol), NULL);
  StkRowItem *self = g_object_new(STK_TYPE_ROW_ITEM, NULL);
  self->symbol = g_ascii_strup(symbol, -1);
  self->name = stk_sanitize_text(name);
  self->kind = kind;
  return self;
}

/* Documented in stk-row.h. */
const char *
stk_row_item_get_symbol(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), NULL);
  return self->symbol;
}

/* Documented in stk-row.h. */
const char *
stk_row_item_get_name(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), NULL);
  return self->name;
}

/* Documented in stk-row.h. */
StkKind
stk_row_item_get_kind(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), STK_KIND_STOCK);
  return self->kind;
}

/* Documented in stk-row.h. */
const StkQuote *
stk_row_item_get_quote(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), NULL);
  return self->quote;
}

/* Documented in stk-row.h. */
StkSeries *
stk_row_item_get_spark(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), NULL);
  return self->spark;
}

/* Documented in stk-row.h. */
gboolean
stk_row_item_get_stale(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), FALSE);
  return self->stale;
}

/* Documented in stk-row.h. */
StkRange
stk_row_item_get_detail_range(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), STK_RANGE_1D);
  return self->detail_range;
}

/* Documented in stk-row.h. */
StkSeries *
stk_row_item_get_detail_series(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), NULL);
  return self->detail_series;
}

/**
 * same_double:
 * @a: first value.
 * @b: second value.
 *
 * Equality that treats two NANs as equal.
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - nothing is changed.
 * Thread affinity: any thread (no state).
 * Ownership: none (no pointers).
 *
 * Returns: TRUE if @a == @b or both are NAN.
 */
static gboolean
same_double(double a, double b)
{
  return a == b || (isnan(a) && isnan(b));
}

/**
 * same_quote:
 * @a: (nullable): first quote.
 * @b: (nullable): second quote.
 *
 * Compares price, change, change %, timestamp and the realtime flag (the
 * strings are not compared).
 *
 * Preconditions:
 *  - each of @a and @b is NULL or a valid StkQuote.
 * Postconditions:
 *  - nothing is changed.
 * Thread affinity: any thread (reads only its arguments).
 * Ownership: both quotes are borrowed for the call only.
 *
 * Returns: TRUE if both are NULL or all compared fields are equal; FALSE if exactly one is NULL
 *   or a field differs.
 */
static gboolean
same_quote(const StkQuote *a, const StkQuote *b)
{
  if (a == NULL || b == NULL)
    return a == b;
  return same_double(a->price, b->price) && same_double(a->change, b->change) &&
         same_double(a->change_pct, b->change_pct) && a->as_of_ms == b->as_of_ms &&
         a->is_realtime == b->is_realtime;
}

/**
 * same_series:
 * @a: (nullable): first series.
 * @b: (nullable): second series.
 *
 * Compares range, point count, prev_close and every point.
 *
 * Preconditions:
 *  - each of @a and @b is NULL or a valid StkSeries whose points array holds n_points readable
 *    elements. The byte count n_points * sizeof(StkPoint) therefore cannot overflow a gsize: it is
 *    at most the size of the allocation that holds those points.
 *  - memcmp() runs only after n_points was found equal, so it reads n_points elements of both
 *    arrays and no more.
 * Postconditions:
 *  - nothing is changed.
 * Thread affinity: any thread (reads only its arguments).
 * Ownership: both series are borrowed for the call only.
 *
 * Returns: TRUE if @a == @b (same pointer, including both NULL) or the content is equal; FALSE
 *   if exactly one is NULL or the content differs.
 */
static gboolean
same_series(const StkSeries *a, const StkSeries *b)
{
  if (a == b)
    return TRUE;
  if (a == NULL || b == NULL)
    return FALSE;
  if (a->range != b->range || a->n_points != b->n_points || !same_double(a->prev_close, b->prev_close))
    return FALSE;
  /* StkPoint is {gint64, double}: no padding, so memcmp compares exactly the data. */
  return a->n_points == 0 || memcmp(a->points, b->points, a->n_points * sizeof(StkPoint)) == 0;
}

/* Documented in stk-row.h. */
gboolean
stk_row_item_set_quote(StkRowItem *self, const StkQuote *quote)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), FALSE);
  if (same_quote(self->quote, quote))
    return FALSE;
  StkQuote *copy = quote != NULL ? stk_quote_copy(quote) : NULL;
  stk_quote_free(self->quote);
  self->quote = copy;
  g_signal_emit(self, item_signals[ITEM_CHANGED], 0);
  return TRUE;
}

/* Documented in stk-row.h. */
gboolean
stk_row_item_set_spark(StkRowItem *self, StkSeries *series)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), FALSE);
  if (same_series(self->spark, series))
    return FALSE;
  StkSeries *ref = series != NULL ? stk_series_ref(series) : NULL;
  if (self->spark != NULL)
    stk_series_unref(self->spark);
  self->spark = ref;
  g_signal_emit(self, item_signals[ITEM_CHANGED], 0);
  return TRUE;
}

/* Documented in stk-row.h. */
gboolean
stk_row_item_set_stale(StkRowItem *self, gboolean stale)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), FALSE);
  stale = !!stale;
  if (self->stale == stale)
    return FALSE;
  self->stale = stale;
  g_signal_emit(self, item_signals[ITEM_CHANGED], 0);
  return TRUE;
}

/* Documented in stk-row.h. */
gboolean
stk_row_item_set_detail(StkRowItem *self, StkRange range, StkSeries *series)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), FALSE);
  g_return_val_if_fail(range >= STK_RANGE_1D && range <= STK_RANGE_1Y, FALSE);
  if (self->detail_range == range && same_series(self->detail_series, series))
    return FALSE;
  StkSeries *ref = series != NULL ? stk_series_ref(series) : NULL;
  if (self->detail_series != NULL)
    stk_series_unref(self->detail_series);
  self->detail_series = ref;
  self->detail_range = range;
  g_signal_emit(self, item_signals[ITEM_DETAIL_CHANGED], 0);
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* StkRow                                                                   */
/* ------------------------------------------------------------------------ */

struct _StkRow {
  GtkBox parent_instance;
  GtkWidget *symbol_label;
  GtkWidget *name_label;
  GtkWidget *spark;
  GtkWidget *price_label;
  GtkWidget *change_label;
  GtkWidget *menu_button;

  StkRowItem *item; /* strong while bound */
  gulong changed_id;
  StkSeries *shown_spark;  /* strong: what the sparkline currently draws */
  const char *shown_class; /* static string of the change label's colour class */
};

/*
 * G_DEFINE_FINAL_TYPE() below defines stk_row_get_type(), the static
 * stk_row_parent_class pointer and the static class initialiser that runs
 * stk_row_class_init() once; stk_row_init() is registered as the instance
 * initialiser. The instance struct is _StkRow (StkRow).
 */
G_DEFINE_FINAL_TYPE(StkRow, stk_row, GTK_TYPE_BOX)

enum { ROW_REMOVE_REQUESTED, ROW_MOVE_REQUESTED, ROW_REORDER_REQUESTED, N_ROW_SIGNALS };
static guint row_signals[N_ROW_SIGNALS];

/**
 * set_change_class:
 * @self: (not nullable): the row.
 * @cls: (nullable): colour class for the change label, or NULL for none.
 *
 * Swaps the colour CSS class of the change label.
 *
 * Preconditions:
 *  - @self is a constructed StkRow.
 *  - @cls is NULL or one of the static strings returned by stk_change_class() (the previous class
 *    is recognised by pointer comparison and removed by name).
 * Postconditions:
 *  - the label carries exactly @cls (or none) from the colour classes and @self->shown_class is
 *    @cls; nothing happens when it already was.
 * Thread affinity: main thread only.
 * Ownership: @cls is a static string, not copied or freed.
 */
static void
set_change_class(StkRow *self, const char *cls)
{
  if (self->shown_class == cls)
    return;
  if (self->shown_class != NULL)
    gtk_widget_remove_css_class(self->change_label, self->shown_class);
  self->shown_class = cls;
  if (cls != NULL)
    gtk_widget_add_css_class(self->change_label, cls);
}

/**
 * set_spark:
 * @self: (not nullable): the row.
 * @series: (nullable): the series to draw, or NULL to clear the sparkline.
 *
 * Makes the sparkline draw @series and keeps one reference on it for the row.
 *
 * Preconditions:
 *  - @self is a constructed StkRow; @series is NULL or a valid StkSeries.
 * Postconditions:
 *  - @self->shown_spark is @series (the row holds one reference of its own, the previous one is
 *    released) and the sparkline was given @series; nothing happens when it already was.
 * Thread affinity: main thread only.
 * Ownership: @series stays with the caller; the row takes its own reference.
 */
static void
set_spark(StkRow *self, StkSeries *series)
{
  if (self->shown_spark == series)
    return;
  StkSeries *ref = series != NULL ? stk_series_ref(series) : NULL;
  if (self->shown_spark != NULL)
    stk_series_unref(self->shown_spark);
  self->shown_spark = ref;
  stk_sparkline_set_series(STK_SPARKLINE(self->spark), series);
}

/**
 * refresh:
 * @self: (not nullable): the row.
 *
 * Redraws the whole row from the bound item (blank when none): texts, change colour class,
 * sparkline and the "stale" class. Also the callback of the bound item's "changed" signal
 * (connected with g_signal_connect_swapped(), so the row is its first argument).
 *
 * Preconditions:
 *  - @self is a constructed StkRow; @self->item is NULL or a live StkRowItem (strongly held).
 * Postconditions:
 *  - the labels, classes and sparkline reflect the item's current data; a missing quote shows
 *    the em dash. The item is not modified.
 * Thread affinity: main thread only.
 * Ownership: none transferred; all strings are borrowed or freed locally.
 */
static void
refresh(StkRow *self)
{
  if (self->item == NULL) {
    gtk_label_set_text(GTK_LABEL(self->symbol_label), "");
    gtk_label_set_text(GTK_LABEL(self->name_label), "");
    gtk_label_set_text(GTK_LABEL(self->price_label), "");
    gtk_label_set_text(GTK_LABEL(self->change_label), "");
    set_change_class(self, NULL);
    set_spark(self, NULL);
    gtk_widget_remove_css_class(GTK_WIDGET(self), "stale");
    return;
  }

  const StkQuote *q = stk_row_item_get_quote(self->item);
  double price = q != NULL ? q->price : NAN;
  double pct = q != NULL ? q->change_pct : NAN;

  gtk_label_set_text(GTK_LABEL(self->symbol_label), stk_row_item_get_symbol(self->item));
  gtk_label_set_text(GTK_LABEL(self->name_label), stk_row_item_get_name(self->item));
  g_autofree char *price_text = stk_format_price(price);
  gtk_label_set_text(GTK_LABEL(self->price_label), price_text);
  g_autofree char *pct_text = stk_format_percent(pct);
  gtk_label_set_text(GTK_LABEL(self->change_label), pct_text);
  set_change_class(self, stk_change_class(pct));
  set_spark(self, stk_row_item_get_spark(self->item));
  if (stk_row_item_get_stale(self->item))
    gtk_widget_add_css_class(GTK_WIDGET(self), "stale");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(self), "stale");
}

/* Documented in stk-row.h. */
void
stk_row_bind(StkRow *self, StkRowItem *item)
{
  g_return_if_fail(STK_IS_ROW(self));
  g_return_if_fail(item == NULL || STK_IS_ROW_ITEM(item));
  if (self->item != NULL) {
    g_signal_handler_disconnect(self->item, self->changed_id);
    self->changed_id = 0;
    g_clear_object(&self->item);
  }
  if (item != NULL) {
    self->item = g_object_ref(item);
    self->changed_id = g_signal_connect_swapped(item, "changed", G_CALLBACK(refresh), self);
  }
  refresh(self);
}

/**
 * emit_with_symbol:
 * @self: (not nullable): the row.
 * @signal_id: the id of "remove-requested" or "move-requested" (row_signals[]).
 * @delta: the second signal argument; only used when @has_delta is TRUE.
 * @has_delta: TRUE for "move-requested" (arguments symbol, delta), FALSE for
 *   "remove-requested" (argument symbol).
 *
 * Emits the signal with the bound item's symbol.
 *
 * Preconditions:
 *  - @self is a constructed StkRow; @signal_id and @has_delta agree on the signal's arity.
 * Postconditions:
 *  - nothing happens when the row is unbound. Otherwise the signal was emitted with a private
 *    copy of the symbol, while the row was kept alive by a temporary reference, so a handler
 *    that unbinds, disposes or destroys the row cannot invalidate the arguments.
 * Thread affinity: main thread only.
 * Ownership: the symbol copy and the temporary reference are released before returning.
 */
static void
emit_with_symbol(StkRow *self, guint signal_id, int delta, gboolean has_delta)
{
  if (self->item == NULL)
    return;
  /* The handler may unbind or destroy the row: keep it and the symbol alive. */
  g_autoptr(StkRow) keep = g_object_ref(self);
  g_autofree char *symbol = g_strdup(stk_row_item_get_symbol(self->item));
  if (has_delta)
    g_signal_emit(keep, signal_id, 0, symbol, delta);
  else
    g_signal_emit(keep, signal_id, 0, symbol);
}

/**
 * on_remove:
 * @action: the activated "row.remove" action (unused).
 * @parameter: (nullable): the activation parameter (unused; the action has none).
 * @user_data: (not nullable): the StkRow, passed as user data by g_signal_connect_object() in
 *   stk_row_init(), which disconnects the handler when the row is finalized.
 *
 * "activate" handler of the "remove" action: emits "remove-requested".
 *
 * Preconditions:
 *  - @user_data is a StkRow (it was connected with the row as the object).
 * Postconditions:
 *  - "remove-requested" was emitted with the bound item's symbol; nothing when unbound.
 * Thread affinity: main thread only.
 * Ownership: all parameters are borrowed for the call.
 */
static void
on_remove(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void)action;
  (void)parameter;
  emit_with_symbol(STK_ROW(user_data), row_signals[ROW_REMOVE_REQUESTED], 0, FALSE);
}

/**
 * on_move_up:
 * @action: the activated "row.move-up" action (unused).
 * @parameter: (nullable): the activation parameter (unused; the action has none).
 * @user_data: (not nullable): the StkRow, see on_remove().
 *
 * "activate" handler of the "move-up" action: emits "move-requested" with delta -1.
 *
 * Preconditions:
 *  - @user_data is a StkRow.
 * Postconditions:
 *  - "move-requested" was emitted with the bound item's symbol and -1; nothing when unbound.
 * Thread affinity: main thread only.
 * Ownership: all parameters are borrowed for the call.
 */
static void
on_move_up(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void)action;
  (void)parameter;
  emit_with_symbol(STK_ROW(user_data), row_signals[ROW_MOVE_REQUESTED], -1, TRUE);
}

/**
 * on_move_down:
 * @action: the activated "row.move-down" action (unused).
 * @parameter: (nullable): the activation parameter (unused; the action has none).
 * @user_data: (not nullable): the StkRow, see on_remove().
 *
 * "activate" handler of the "move-down" action: emits "move-requested" with delta +1.
 *
 * Preconditions:
 *  - @user_data is a StkRow.
 * Postconditions:
 *  - "move-requested" was emitted with the bound item's symbol and +1; nothing when unbound.
 * Thread affinity: main thread only.
 * Ownership: all parameters are borrowed for the call.
 */
static void
on_move_down(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void)action;
  (void)parameter;
  emit_with_symbol(STK_ROW(user_data), row_signals[ROW_MOVE_REQUESTED], 1, TRUE);
}

/**
 * row_menu:
 *
 * Returns the menu model shared by all rows ("Move up", "Move down", "Remove"), created on
 * first use. The GMenu is created once per process and intentionally never freed (it lives for
 * the whole process; every menu button takes its own reference). g_once_init_enter() makes the
 * creation safe even if it were ever called from two threads.
 *
 * Preconditions:
 *  - GLib only; the caller is the main thread (create_popup()).
 * Postconditions:
 *  - the menu exists and is unchanged afterwards.
 * Thread affinity: main thread only.
 * Ownership: the model is owned by this function's static; the caller must not free it.
 *
 * Returns: (transfer none): the shared menu model, never NULL.
 */
static GMenuModel *
row_menu(void)
{
  static gsize once = 0;
  static GMenu *menu;
  if (g_once_init_enter(&once)) {
    menu = g_menu_new();
    g_menu_append(menu, "Move up", "row.move-up");
    g_menu_append(menu, "Move down", "row.move-down");
    g_menu_append(menu, "Remove", "row.remove");
    g_once_init_leave(&once, 1);
  }
  return G_MENU_MODEL(menu);
}

/**
 * create_popup:
 * @button: the row's menu button.
 * @user_data: unused (NULL: no data was registered).
 *
 * GtkMenuButtonCreatePopupFunc: the popover (and its widgets) is only built when the menu is
 * first opened, by giving the button its menu model.
 *
 * Preconditions:
 *  - @button is a GtkMenuButton.
 * Postconditions:
 *  - @button has a menu model (row_menu()) afterwards; an existing one is kept.
 * Thread affinity: main thread only.
 * Ownership: @button is borrowed; the button takes its own reference on the shared menu.
 */
static void
create_popup(GtkMenuButton *button, gpointer user_data)
{
  (void)user_data;
  if (gtk_menu_button_get_menu_model(button) == NULL)
    gtk_menu_button_set_menu_model(button, row_menu());
}

/**
 * on_drag_prepare:
 * @source: the drag source (unused).
 * @x: pointer x (unused).
 * @y: pointer y (unused).
 * @user_data: (not nullable): the StkRow, passed as user data by g_signal_connect_object().
 *
 * GtkDragSource "prepare" handler: the dragged payload is the bound item's symbol as text.
 *
 * Preconditions:
 *  - @user_data is a StkRow.
 * Postconditions:
 *  - no state is changed; the provider holds its own copy of the symbol.
 * Thread affinity: main thread only.
 * Ownership: the returned provider is owned by the caller (GTK); the symbol is copied.
 *
 * Returns: (transfer full) (nullable): a G_TYPE_STRING content provider, or NULL (no drag) when
 *   the row is unbound.
 */
static GdkContentProvider *
on_drag_prepare(GtkDragSource *source, double x, double y, gpointer user_data)
{
  (void)source;
  (void)x;
  (void)y;
  StkRow *self = STK_ROW(user_data);
  if (self->item == NULL)
    return NULL;
  return gdk_content_provider_new_typed(G_TYPE_STRING, stk_row_item_get_symbol(self->item));
}

/**
 * on_drag_begin:
 * @source: the drag source.
 * @drag: the new drag (unused).
 * @user_data: (not nullable): the StkRow, passed as user data by g_signal_connect_object().
 *
 * GtkDragSource "drag-begin" handler: uses a snapshot of the row as the drag icon.
 *
 * Preconditions:
 *  - @user_data is a StkRow (a GtkWidget).
 * Postconditions:
 *  - @source has the row's paintable as its icon (hotspot 0, 0); the local reference on the
 *    paintable is released.
 * Thread affinity: main thread only.
 * Ownership: all parameters are borrowed for the call.
 */
static void
on_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer user_data)
{
  (void)drag;
  g_autoptr(GdkPaintable) icon = gtk_widget_paintable_new(GTK_WIDGET(user_data));
  gtk_drag_source_set_icon(source, icon, 0, 0);
}

/**
 * on_drop:
 * @target: the drop target (unused).
 * @value: the dropped value; the target only accepts G_TYPE_STRING.
 * @x: drop x (unused).
 * @y: drop y (unused).
 * @user_data: (not nullable): the StkRow, passed as user data by g_signal_connect_object().
 *
 * GtkDropTarget "drop" handler: asks the owner to move the dropped instrument to this row's
 * position by emitting "reorder-requested". The payload may come from another application, so it
 * is UNTRUSTED text: it is only copied here (a GValue string is NUL-terminated) and handed on;
 * it is never interpreted, indexed or stored by the row. The receiver validates it
 * (on_row_reorder() in stk-window.c: stk_watchlist_index_of() looks at no more than the first 16
 * bytes and rejects anything that is not a valid symbol).
 *
 * Preconditions:
 *  - @user_data is a StkRow; @value is a valid, initialised GValue.
 * Postconditions:
 *  - when the row is bound and @value holds a non-NULL string, "reorder-requested" was emitted
 *    with a copy of that string (not length limited here) and the bound symbol, and TRUE is
 *    returned (also if the receiver then ignores the text); otherwise nothing happens and FALSE
 *    is returned. Row and copies stay alive while the handlers run.
 * Thread affinity: main thread only.
 * Ownership: @value is borrowed for the call; the copies are freed before returning.
 *
 * Returns: TRUE if the drop was accepted (signal emitted), FALSE otherwise.
 */
static gboolean
on_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data)
{
  (void)target;
  (void)x;
  (void)y;
  StkRow *self = STK_ROW(user_data);
  if (self->item == NULL || !G_VALUE_HOLDS_STRING(value))
    return FALSE;
  const char *source = g_value_get_string(value);
  if (source == NULL)
    return FALSE;
  g_autoptr(StkRow) keep = g_object_ref(self);
  g_autofree char *src = g_strdup(source);
  g_autofree char *dst = g_strdup(stk_row_item_get_symbol(self->item));
  g_signal_emit(keep, row_signals[ROW_REORDER_REQUESTED], 0, src, dst);
  return TRUE;
}

/**
 * stk_row_dispose:
 * @object: the row being disposed.
 *
 * GObject dispose vfunc: disconnects from the bound item and drops the references on the item
 * and the shown series, then chains up. May run more than once.
 *
 * Preconditions:
 *  - @object is a StkRow; called by GObject (possibly repeatedly).
 * Postconditions:
 *  - the row is unbound (no handler of the item points at it any more), item and shown_spark are
 *    NULL; the parent dispose has run. A second call finds both NULL and only chains up.
 * Thread affinity: main thread only.
 * Ownership: the item reference and the series reference held by the row are released.
 */
static void
stk_row_dispose(GObject *object)
{
  StkRow *self = STK_ROW(object);
  if (self->item != NULL) {
    g_signal_handler_disconnect(self->item, self->changed_id);
    self->changed_id = 0;
    g_clear_object(&self->item);
  }
  g_clear_pointer(&self->shown_spark, stk_series_unref);
  G_OBJECT_CLASS(stk_row_parent_class)->dispose(object);
}

/**
 * stk_row_class_init:
 * @klass: (not nullable): the class structure being initialised.
 *
 * GObject class initialiser: installs dispose() and registers the signals "remove-requested"
 * (string), "move-requested" (string, int) and "reorder-requested" (string, string), all
 * G_SIGNAL_RUN_LAST.
 *
 * Preconditions:
 *  - called once by GObject while the type is registered.
 * Postconditions:
 *  - row_signals[] holds the three signal ids.
 * Thread affinity: main thread only.
 * Ownership: @klass is owned by GObject.
 */
static void
stk_row_class_init(StkRowClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = stk_row_dispose;
  row_signals[ROW_REMOVE_REQUESTED] =
      g_signal_new("remove-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                   NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  row_signals[ROW_MOVE_REQUESTED] =
      g_signal_new("move-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                   NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);
  row_signals[ROW_REORDER_REQUESTED] =
      g_signal_new("reorder-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                   NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
}

/**
 * make_label:
 * @css_class: (not nullable): style class added to the label.
 * @xalign: horizontal alignment, 0.0 (left) .. 1.0 (right).
 *
 * Creates a single-line label that ellipsizes at the end.
 *
 * Preconditions:
 *  - GTK is initialised; @css_class is a NUL-terminated string; @xalign is in 0.0 .. 1.0.
 * Postconditions:
 *  - a new floating label is returned.
 * Thread affinity: main thread only.
 * Ownership: @css_class is borrowed (GTK copies it).
 *
 * Returns: (transfer floating): the new label.
 */
static GtkWidget *
make_label(const char *css_class, float xalign)
{
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), xalign);
  gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(label, css_class);
  return label;
}

/**
 * stk_row_init:
 * @self: (not nullable): the new row (zero-filled by GObject).
 *
 * GObject instance initialiser: builds the child widgets (symbol/name, sparkline, price, change,
 * menu button), the "row" action group (remove, move-up, move-down) and the drag source and drop
 * target. The action and controller handlers are connected with g_signal_connect_object() and
 * the row as the object, so they never outlive it. The row starts unbound.
 *
 * Preconditions:
 *  - called once by GObject for a new instance; GTK is initialised.
 * Postconditions:
 *  - all child pointers of @self are set, item is NULL; the widget owns the children, the action
 *    group and the controllers.
 * Thread affinity: main thread only.
 * Ownership: @self is owned by GObject; local references on the action group and actions are
 *   released after they were handed to the widget / the group.
 */
static void
stk_row_init(StkRow *self)
{
  GtkWidget *box = GTK_WIDGET(self);
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing(GTK_BOX(self), 10);
  gtk_widget_add_css_class(box, "stk-row");

  GtkWidget *names = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(names, TRUE);
  gtk_widget_set_valign(names, GTK_ALIGN_CENTER);
  self->symbol_label = make_label("stk-symbol", 0.0f);
  self->name_label = make_label("stk-name", 0.0f);
  gtk_label_set_width_chars(GTK_LABEL(self->symbol_label), 6);
  gtk_label_set_width_chars(GTK_LABEL(self->name_label), 8);
  gtk_label_set_max_width_chars(GTK_LABEL(self->name_label), 24);
  gtk_box_append(GTK_BOX(names), self->symbol_label);
  gtk_box_append(GTK_BOX(names), self->name_label);
  gtk_box_append(GTK_BOX(self), names);

  self->spark = stk_sparkline_new();
  gtk_widget_set_valign(self->spark, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(self), self->spark);

  self->price_label = make_label("stk-price", 1.0f);
  gtk_label_set_width_chars(GTK_LABEL(self->price_label), 7);
  gtk_widget_set_valign(self->price_label, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(self), self->price_label);

  self->change_label = make_label("stk-change", 1.0f);
  gtk_label_set_width_chars(GTK_LABEL(self->change_label), 7);
  gtk_widget_set_valign(self->change_label, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(self), self->change_label);

  self->menu_button = gtk_menu_button_new();
  gtk_menu_button_set_child(GTK_MENU_BUTTON(self->menu_button), gtk_label_new("\xe2\x80\xa6"));
  gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(self->menu_button), create_popup, NULL,
                                        NULL);
  gtk_widget_add_css_class(self->menu_button, "flat");
  gtk_widget_set_valign(self->menu_button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(self->menu_button, "Row actions");
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->menu_button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Row actions", -1);
  gtk_box_append(GTK_BOX(self), self->menu_button);

  GSimpleActionGroup *group = g_simple_action_group_new();
  static const struct {
    const char *name;
    GCallback cb;
  } actions[] = {{"remove", G_CALLBACK(on_remove)},
                 {"move-up", G_CALLBACK(on_move_up)},
                 {"move-down", G_CALLBACK(on_move_down)}};
  for (guint i = 0; i < G_N_ELEMENTS(actions); i++) {
    GSimpleAction *action = g_simple_action_new(actions[i].name, NULL);
    g_signal_connect_object(action, "activate", actions[i].cb, self, 0);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);
  }
  gtk_widget_insert_action_group(box, "row", G_ACTION_GROUP(group));
  g_object_unref(group);

  GtkDragSource *drag = gtk_drag_source_new();
  gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
  g_signal_connect_object(drag, "prepare", G_CALLBACK(on_drag_prepare), self, 0);
  g_signal_connect_object(drag, "drag-begin", G_CALLBACK(on_drag_begin), self, 0);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drag));

  GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
  g_signal_connect_object(drop, "drop", G_CALLBACK(on_drop), self, 0);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drop));
}

/* Documented in stk-row.h. */
GtkWidget *
stk_row_new(void)
{
  return g_object_new(STK_TYPE_ROW, NULL);
}
