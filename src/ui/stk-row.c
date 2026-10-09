/* stk-row.c - StkRowItem model, StkRow widget and shared formatting. */
#include "stk-row.h"

#include <math.h>
#include <string.h>

#include "stk-sparkline.h"

/* ------------------------------------------------------------------------ */
/* Formatting                                                               */
/* ------------------------------------------------------------------------ */

#define HUGE_VALUE 1e12

char *
stk_format_price(double value)
{
  if (!isfinite(value))
    return g_strdup(STK_MISSING_TEXT);
  if (fabs(value) >= HUGE_VALUE)
    return g_strdup_printf("%.3e", value);
  return g_strdup_printf("%.2f", value);
}

/* -1, 0, +1: the sign the value has once rounded to two decimals. */
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

char *
stk_format_percent(double value)
{
  if (!isfinite(value))
    return g_strdup(STK_MISSING_TEXT);
  g_autofree char *num = stk_format_change(value);
  return g_strconcat(num, "%", NULL);
}

const char *
stk_change_class(double value)
{
  int s = displayed_sign(value);
  return s > 0 ? "positive" : (s < 0 ? "negative" : "flat");
}

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
  for (guint i = 0; i < 3 && len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80; i++)
    len--;

  char *out = g_utf8_make_valid(text, (gssize)len);
  /* One pass, in place. Every replacement is one ASCII byte, never longer
   * than the UTF-8 sequence it replaces, so the write position @w never
   * passes the read position @r and nothing is ever shifted. */
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

static void
stk_row_item_init(StkRowItem *self)
{
  self->detail_range = STK_RANGE_1D;
}

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

const char *
stk_row_item_get_symbol(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), NULL);
  return self->symbol;
}

const char *
stk_row_item_get_name(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), NULL);
  return self->name;
}

StkKind
stk_row_item_get_kind(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), STK_KIND_STOCK);
  return self->kind;
}

const StkQuote *
stk_row_item_get_quote(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), NULL);
  return self->quote;
}

StkSeries *
stk_row_item_get_spark(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), NULL);
  return self->spark;
}

gboolean
stk_row_item_get_stale(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), FALSE);
  return self->stale;
}

StkRange
stk_row_item_get_detail_range(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), STK_RANGE_1D);
  return self->detail_range;
}

StkSeries *
stk_row_item_get_detail_series(StkRowItem *self)
{
  g_return_val_if_fail(STK_IS_ROW_ITEM(self), NULL);
  return self->detail_series;
}

static gboolean
same_double(double a, double b)
{
  return a == b || (isnan(a) && isnan(b));
}

static gboolean
same_quote(const StkQuote *a, const StkQuote *b)
{
  if (a == NULL || b == NULL)
    return a == b;
  return same_double(a->price, b->price) && same_double(a->change, b->change) &&
         same_double(a->change_pct, b->change_pct) && a->as_of_ms == b->as_of_ms &&
         a->is_realtime == b->is_realtime;
}

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

G_DEFINE_FINAL_TYPE(StkRow, stk_row, GTK_TYPE_BOX)

enum { ROW_REMOVE_REQUESTED, ROW_MOVE_REQUESTED, ROW_REORDER_REQUESTED, N_ROW_SIGNALS };
static guint row_signals[N_ROW_SIGNALS];

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

static void
on_remove(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void)action;
  (void)parameter;
  emit_with_symbol(STK_ROW(user_data), row_signals[ROW_REMOVE_REQUESTED], 0, FALSE);
}

static void
on_move_up(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void)action;
  (void)parameter;
  emit_with_symbol(STK_ROW(user_data), row_signals[ROW_MOVE_REQUESTED], -1, TRUE);
}

static void
on_move_down(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void)action;
  (void)parameter;
  emit_with_symbol(STK_ROW(user_data), row_signals[ROW_MOVE_REQUESTED], 1, TRUE);
}

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

/* The popover (and its widgets) is only built when the menu is first opened. */
static void
create_popup(GtkMenuButton *button, gpointer user_data)
{
  (void)user_data;
  if (gtk_menu_button_get_menu_model(button) == NULL)
    gtk_menu_button_set_menu_model(button, row_menu());
}

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

static void
on_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer user_data)
{
  (void)drag;
  g_autoptr(GdkPaintable) icon = gtk_widget_paintable_new(GTK_WIDGET(user_data));
  gtk_drag_source_set_icon(source, icon, 0, 0);
}

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

GtkWidget *
stk_row_new(void)
{
  return g_object_new(STK_TYPE_ROW, NULL);
}
