/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/* stk-detail.c - StkDetail: price, change, range switcher and chart. */
#include "stk-detail.h"

#include <math.h>

#include "stk-chart.h"
#include "stk-market.h"

#define N_RANGES 5

/* range_buttons[] and range_labels[] are indexed by StkRange. */
G_STATIC_ASSERT(N_RANGES == STK_RANGE_1Y + 1);

struct _StkDetail {
  GtkBox parent_instance;
  GtkWidget *stack;
  GtkWidget *symbol_label;
  GtkWidget *name_label;
  GtkWidget *badge;
  GtkWidget *price_label;
  GtkWidget *change_label;
  GtkWidget *caption_label;
  GtkWidget *range_buttons[N_RANGES]; /* indexed by StkRange */
  GtkWidget *chart;

  StkRowItem *item; /* strong */
  gulong changed_id;
  gulong detail_id;
  StkRange range;
  gboolean offline;
  gboolean updating_range; /* ignore "toggled" while we flip buttons ourselves */
  const char *shown_class; /* static colour class of the change label */
  StkSeries *shown_series; /* strong: what the chart currently draws */
};

/*
 * G_DEFINE_FINAL_TYPE() below defines stk_detail_get_type(), the static
 * stk_detail_parent_class pointer and the static class initialiser that runs
 * stk_detail_class_init() once; stk_detail_init() is registered as the
 * instance initialiser. The instance struct is _StkDetail (StkDetail).
 */
G_DEFINE_FINAL_TYPE(StkDetail, stk_detail, GTK_TYPE_BOX)

enum { SIGNAL_RANGE_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static const char *const range_labels[N_RANGES] = {"1D", "1M", "3M", "6M", "1Y"};

/* ------------------------------------------------------------------------ */
/* Rendering                                                                */
/* ------------------------------------------------------------------------ */

/**
 * format_caption:
 * @q: (nullable): the quote, or NULL when there is none.
 * @kind: the instrument class (funds get the "NAV" prefix).
 *
 * Builds the caption under the change: the New York date of the quote's timestamp.
 *
 * Preconditions:
 *  - @q is NULL or a valid StkQuote.
 * Postconditions:
 *  - nothing is changed.
 * Thread affinity: main thread only.
 * Ownership: @q is borrowed for the call; the result is owned by the caller.
 *
 * Returns: (transfer full): "NAV · <date>" (funds) or "As of <date>" (others) when @q has a
 *   positive timestamp whose New York date could be computed and formatted ("%b %d, %Y"); otherwise
 *   "NAV" for funds and "" for the others. Never NULL.
 */
static char *
format_caption(const StkQuote *q, StkKind kind)
{
  gboolean fund = kind == STK_KIND_FUND;
  GDate date;
  g_date_clear(&date, 1);
  if (q != NULL && q->as_of_ms > 0 && stk_market_local_date(q->as_of_ms, &date)) {
    g_autoptr(GDateTime) dt = g_date_time_new_utc(g_date_get_year(&date), g_date_get_month(&date),
                                                  g_date_get_day(&date), 12, 0, 0);
    if (dt != NULL) {
      g_autofree char *text = g_date_time_format(dt, "%b %d, %Y");
      if (text != NULL)
        return fund ? g_strdup_printf("NAV \xc2\xb7 %s", text)
                    : g_strdup_printf("As of %s", text);
    }
  }
  return g_strdup(fund ? "NAV" : "");
}

/**
 * set_change_class:
 * @self: (not nullable): the pane.
 * @cls: (not nullable): colour class for the change label.
 *
 * Swaps the colour CSS class of the change label.
 *
 * Preconditions:
 *  - @self is a constructed StkDetail; @cls is one of the static strings returned by
 *    stk_change_class() (never NULL; the previous class is recognised by pointer comparison).
 * Postconditions:
 *  - the label carries @cls and no other colour class; self->shown_class is @cls; nothing happens
 *    when it already was.
 * Thread affinity: main thread only.
 * Ownership: @cls is a static string, not copied or freed.
 */
static void
set_change_class(StkDetail *self, const char *cls)
{
  if (self->shown_class == cls)
    return;
  if (self->shown_class != NULL)
    gtk_widget_remove_css_class(self->change_label, self->shown_class);
  self->shown_class = cls;
  gtk_widget_add_css_class(self->change_label, cls);
}

/**
 * refresh_status:
 * @self: (not nullable): the pane.
 *
 * Updates the "Offline"/"Stale" badge (offline takes precedence) and the chart's stale marker.
 *
 * Preconditions:
 *  - @self is a constructed, not disposed StkDetail; self->item is NULL or a live item.
 * Postconditions:
 *  - the badge is visible with "Offline" or "Stale", or hidden when neither applies; the chart is
 *    marked stale iff offline or the item is stale.
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides @self).
 */
static void
refresh_status(StkDetail *self)
{
  gboolean stale = self->item != NULL && stk_row_item_get_stale(self->item);
  const char *text = self->offline ? "Offline" : (stale ? "Stale" : NULL);
  if (text != NULL)
    gtk_label_set_text(GTK_LABEL(self->badge), text);
  gtk_widget_set_visible(self->badge, text != NULL);
  stk_chart_set_stale(STK_CHART(self->chart), self->offline || stale);
}

/**
 * refresh_chart:
 * @self: (not nullable): the pane.
 *
 * Gives the chart the item's detail series while the item's detail range equals the selected
 * range, otherwise no series.
 *
 * Preconditions:
 *  - @self is a constructed, not disposed StkDetail; self->item is NULL or a live item.
 * Postconditions:
 *  - self->shown_series is that series with one reference of the pane's own (the previous one is
 *    released) and the chart was given it; nothing happens when the series is already shown.
 * Thread affinity: main thread only.
 * Ownership: the series stays with the item; the pane takes its own reference.
 */
static void
refresh_chart(StkDetail *self)
{
  StkSeries *series = NULL;
  if (self->item != NULL && stk_row_item_get_detail_range(self->item) == self->range)
    series = stk_row_item_get_detail_series(self->item);
  if (series == self->shown_series)
    return;
  StkSeries *ref = series != NULL ? stk_series_ref(series) : NULL;
  if (self->shown_series != NULL)
    stk_series_unref(self->shown_series);
  self->shown_series = ref;
  stk_chart_set_series(STK_CHART(self->chart), series);
}

/**
 * refresh_quote:
 * @self: (not nullable): the pane.
 *
 * Re-renders the price, the change text, its colour class and the caption from the item's quote
 * (a missing quote shows the em dash).
 *
 * Preconditions:
 *  - @self is a constructed, not disposed StkDetail.
 * Postconditions:
 *  - nothing happens when there is no item; otherwise the three labels reflect the quote. The
 *    caption label is hidden when the caption is empty.
 * Thread affinity: main thread only.
 * Ownership: the quote is borrowed from the item; all strings are freed locally.
 */
static void
refresh_quote(StkDetail *self)
{
  if (self->item == NULL)
    return;
  const StkQuote *q = stk_row_item_get_quote(self->item);
  double price = q != NULL ? q->price : NAN;
  double change = q != NULL ? q->change : NAN;
  double pct = q != NULL ? q->change_pct : NAN;

  g_autofree char *price_text = stk_format_price(price);
  gtk_label_set_text(GTK_LABEL(self->price_label), price_text);

  g_autofree char *change_text = stk_format_change(change);
  g_autofree char *pct_text = stk_format_percent(pct);
  g_autofree char *both = g_strdup_printf("%s (%s)", change_text, pct_text);
  gtk_label_set_text(GTK_LABEL(self->change_label), both);
  /* The percent change is the colour source: it is what the list shows too. */
  set_change_class(self, stk_change_class(isfinite(pct) ? pct : change));

  g_autofree char *caption = format_caption(q, stk_row_item_get_kind(self->item));
  gtk_label_set_text(GTK_LABEL(self->caption_label), caption);
  gtk_widget_set_visible(self->caption_label, caption[0] != '\0');
}

/**
 * set_range_buttons:
 * @self: (not nullable): the pane.
 * @range: the range to select.
 *
 * Selects the range button without emitting "range-changed" (the "toggled" handler is muted).
 *
 * Preconditions:
 *  - @self is a constructed StkDetail that is not disposed (dispose clears range_buttons[]) and
 *    @range is STK_RANGE_1D..STK_RANGE_1Y. A violation is a programming error: a critical is
 *    logged and nothing is changed.
 * Postconditions:
 *  - the button of @range is active and self->range is @range.
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides @self).
 */
static void
set_range_buttons(StkDetail *self, StkRange range)
{
  /* Only stk_detail_set_item() calls this, with a constant range; the check keeps the index and
   * the button pointer valid should that ever change or the pane be disposed. */
  g_return_if_fail((guint)range < N_RANGES && self->range_buttons[range] != NULL);
  self->updating_range = TRUE;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->range_buttons[range]), TRUE);
  self->updating_range = FALSE;
  self->range = range;
}

/**
 * on_item_changed:
 * @self: (not nullable): the pane (receiver of the swapped "changed" connection to the item; the
 *   item argument the signal also passes is not used).
 *
 * Refreshes the texts, badge and stale marker after the item's quote or stale flag changed.
 *
 * Preconditions:
 *  - @self is not disposed: the connection is removed by disconnect_item() when the item is
 *    replaced and by stk_detail_dispose().
 * Postconditions:
 *  - refresh_quote() and refresh_status() ran.
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides @self).
 */
static void
on_item_changed(StkDetail *self)
{
  refresh_quote(self);
  refresh_status(self);
}

/**
 * on_item_detail_changed:
 * @self: (not nullable): the pane (receiver of the swapped "detail-changed" connection to the
 *   item; the item argument the signal also passes is not used).
 *
 * Refreshes the chart after the item's detail series or range changed.
 *
 * Preconditions:
 *  - @self is not disposed (see on_item_changed()).
 * Postconditions:
 *  - refresh_chart() ran.
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides @self).
 */
static void
on_item_detail_changed(StkDetail *self)
{
  refresh_chart(self);
}

/**
 * disconnect_item:
 * @self: (not nullable): the pane.
 *
 * Disconnects both item handlers and drops the pane's reference on the item.
 *
 * Preconditions:
 *  - @self is a constructed StkDetail; when self->item is not NULL, changed_id and detail_id are
 *    the live handler ids on it (set together by stk_detail_set_item()).
 * Postconditions:
 *  - self->item is NULL and both ids are 0; no handler of the item points at the pane. Calling it
 *    again does nothing.
 * Thread affinity: main thread only.
 * Ownership: the pane's reference on the item is released.
 */
static void
disconnect_item(StkDetail *self)
{
  if (self->item == NULL)
    return;
  g_signal_handler_disconnect(self->item, self->changed_id);
  g_signal_handler_disconnect(self->item, self->detail_id);
  self->changed_id = self->detail_id = 0;
  g_clear_object(&self->item);
}

/* Documented in stk-detail.h. */
void
stk_detail_set_item(StkDetail *self, StkRowItem *item)
{
  g_return_if_fail(STK_IS_DETAIL(self));
  g_return_if_fail(item == NULL || STK_IS_ROW_ITEM(item));
  if (item == self->item)
    return;
  disconnect_item(self);

  if (item == NULL) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "empty");
    refresh_chart(self);
    refresh_status(self);
    return;
  }

  self->item = g_object_ref(item);
  self->changed_id = g_signal_connect_swapped(item, "changed", G_CALLBACK(on_item_changed), self);
  self->detail_id =
      g_signal_connect_swapped(item, "detail-changed", G_CALLBACK(on_item_detail_changed), self);

  gboolean fund = stk_row_item_get_kind(item) == STK_KIND_FUND;
  gtk_widget_set_visible(self->range_buttons[STK_RANGE_1D], !fund);
  if (fund && self->range == STK_RANGE_1D)
    set_range_buttons(self, STK_RANGE_1M);

  gtk_label_set_text(GTK_LABEL(self->symbol_label), stk_row_item_get_symbol(item));
  gtk_label_set_text(GTK_LABEL(self->name_label), stk_row_item_get_name(item));
  refresh_quote(self);
  refresh_status(self);
  refresh_chart(self);
  gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "content");
}

/* Documented in stk-detail.h. */
StkRange
stk_detail_get_range(StkDetail *self)
{
  g_return_val_if_fail(STK_IS_DETAIL(self), STK_RANGE_1D);
  return self->range;
}

/* Documented in stk-detail.h. */
void
stk_detail_set_offline(StkDetail *self, gboolean offline)
{
  g_return_if_fail(STK_IS_DETAIL(self));
  offline = !!offline;
  if (offline == self->offline)
    return;
  self->offline = offline;
  refresh_status(self);
}

/* ------------------------------------------------------------------------ */
/* Widget                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * on_range_toggled:
 * @button: the toggle button that changed.
 * @user_data: (not nullable): the StkDetail, passed as user data by g_signal_connect_object() in
 *   stk_detail_init(); disconnected by stk_detail_dispose().
 *
 * "toggled" handler of a range button: when the button became active and is not the current
 * range, makes it the range, refreshes the chart and emits "range-changed" with its index. The
 * index is that of the button in range_buttons[] (0 .. N_RANGES - 1), never external data.
 *
 * Preconditions:
 *  - @user_data is a StkDetail that is not disposed (the handlers are removed by dispose first).
 * Postconditions:
 *  - nothing happens while the pane flips the buttons itself (updating_range), when the button
 *    became inactive, or when it is already the current range; otherwise self->range is the
 *    button's range and "range-changed" was emitted.
 * Thread affinity: main thread only.
 * Ownership: all parameters are borrowed for the call.
 */
static void
on_range_toggled(GtkToggleButton *button, gpointer user_data)
{
  StkDetail *self = STK_DETAIL(user_data);
  if (self->updating_range || !gtk_toggle_button_get_active(button))
    return;
  for (guint i = 0; i < N_RANGES; i++) {
    if (self->range_buttons[i] != GTK_WIDGET(button))
      continue;
    if ((StkRange)i == self->range)
      return;
    self->range = (StkRange)i;
    refresh_chart(self);
    g_signal_emit(self, signals[SIGNAL_RANGE_CHANGED], 0, (guint)i);
    return;
  }
}

/**
 * stk_detail_dispose:
 * @object: the pane being disposed.
 *
 * GObject dispose vfunc: disconnects the range buttons' handlers (and clears the pointers to the
 * buttons), disconnects from the item and drops the pane's reference on it and on the shown
 * series, then chains up. May run more than once.
 *
 * Preconditions:
 *  - @object is a StkDetail; called by GObject (possibly repeatedly).
 * Postconditions:
 *  - range_buttons[] entries are NULL, the pane follows no item and holds no series; the parent
 *    dispose has run. After this the public functions of the pane must not be called any more.
 * Thread affinity: main thread only.
 * Ownership: the item reference and the series reference held by the pane are released; the
 *   buttons are owned by the widget tree.
 */
static void
stk_detail_dispose(GObject *object)
{
  StkDetail *self = STK_DETAIL(object);
  /* Destroying the grouped toggle buttons must not call back into a pane
   * that is being torn down (it would emit "range-changed" to a window that
   * is going away as well), so the buttons lose their handlers first. Dispose
   * may run more than once: a button is only touched the first time. */
  for (guint i = 0; i < N_RANGES; i++) {
    if (self->range_buttons[i] == NULL)
      continue;
    g_signal_handlers_disconnect_by_data(self->range_buttons[i], self);
    self->range_buttons[i] = NULL;
  }
  disconnect_item(self);
  g_clear_pointer(&self->shown_series, stk_series_unref);
  G_OBJECT_CLASS(stk_detail_parent_class)->dispose(object);
}

/**
 * stk_detail_class_init:
 * @klass: (not nullable): the class structure being initialised.
 *
 * GObject class initialiser: installs dispose() and registers "range-changed" (uint range,
 * G_SIGNAL_RUN_LAST).
 *
 * Preconditions:
 *  - called once by GObject while the type is registered.
 * Postconditions:
 *  - signals[SIGNAL_RANGE_CHANGED] holds the signal id.
 * Thread affinity: main thread only.
 * Ownership: @klass is owned by GObject.
 */
static void
stk_detail_class_init(StkDetailClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = stk_detail_dispose;
  signals[SIGNAL_RANGE_CHANGED] =
      g_signal_new("range-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                   NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
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
 * stk_detail_init:
 * @self: (not nullable): the new pane (zero-filled by GObject).
 *
 * GObject instance initialiser: builds the placeholder page and the content page (labels, badge,
 * the five grouped range buttons for 1D, 1M, 3M, 6M, 1Y, the chart) and shows the placeholder.
 * The button handlers are connected after the 1D button was activated, so no signal is emitted
 * here, and with g_signal_connect_object() and the pane as the object.
 *
 * Preconditions:
 *  - called once by GObject for a new instance; GTK is initialised.
 * Postconditions:
 *  - range is STK_RANGE_1D, all widget pointers (range_buttons[0..N_RANGES-1]) are set, no item.
 * Thread affinity: main thread only.
 * Ownership: @self is owned by GObject; the widget owns the children.
 */
static void
stk_detail_init(StkDetail *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class(GTK_WIDGET(self), "stk-detail");
  self->range = STK_RANGE_1D;

  self->stack = gtk_stack_new();
  gtk_widget_set_vexpand(self->stack, TRUE);
  gtk_widget_set_hexpand(self->stack, TRUE);
  gtk_box_append(GTK_BOX(self), self->stack);

  GtkWidget *placeholder = gtk_label_new("Select a symbol");
  gtk_widget_add_css_class(placeholder, "dim-label");
  gtk_stack_add_named(GTK_STACK(self->stack), placeholder, "empty");

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(content, "stk-detail-content");
  gtk_stack_add_named(GTK_STACK(self->stack), content, "content");

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(titles, TRUE);
  self->symbol_label = make_label("stk-detail-symbol", 0.0f);
  self->name_label = make_label("stk-detail-name", 0.0f);
  gtk_box_append(GTK_BOX(titles), self->symbol_label);
  gtk_box_append(GTK_BOX(titles), self->name_label);
  self->badge = make_label("stk-badge", 0.5f);
  gtk_widget_set_valign(self->badge, GTK_ALIGN_START);
  gtk_widget_set_visible(self->badge, FALSE);
  gtk_box_append(GTK_BOX(header), titles);
  gtk_box_append(GTK_BOX(header), self->badge);
  gtk_box_append(GTK_BOX(content), header);

  self->price_label = make_label("stk-detail-price", 0.0f);
  gtk_box_append(GTK_BOX(content), self->price_label);

  GtkWidget *change_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  self->change_label = make_label("stk-change", 0.5f);
  self->caption_label = make_label("stk-detail-caption", 0.0f);
  gtk_box_append(GTK_BOX(change_row), self->change_label);
  gtk_box_append(GTK_BOX(change_row), self->caption_label);
  gtk_box_append(GTK_BOX(content), change_row);

  GtkWidget *ranges = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(ranges, "linked");
  gtk_widget_set_halign(ranges, GTK_ALIGN_START);
  for (guint i = 0; i < N_RANGES; i++) {
    GtkWidget *button = gtk_toggle_button_new_with_label(range_labels[i]);
    gtk_widget_add_css_class(button, "stk-range");
    if (i > 0)
      gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(button),
                                  GTK_TOGGLE_BUTTON(self->range_buttons[0]));
    self->range_buttons[i] = button;
    gtk_box_append(GTK_BOX(ranges), button);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->range_buttons[STK_RANGE_1D]), TRUE);
  for (guint i = 0; i < N_RANGES; i++)
    g_signal_connect_object(self->range_buttons[i], "toggled", G_CALLBACK(on_range_toggled), self,
                            0);
  gtk_box_append(GTK_BOX(content), ranges);

  self->chart = stk_chart_new();
  gtk_box_append(GTK_BOX(content), self->chart);

  gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "empty");
}

/* Documented in stk-detail.h. */
GtkWidget *
stk_detail_new(void)
{
  return g_object_new(STK_TYPE_DETAIL, NULL);
}
