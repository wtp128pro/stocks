/*
 * stk-picker.c - StkPicker implementation. Behaviour is specified in
 * stk-picker.h.
 *
 * Structure:
 *  - StkPickerItem: one row of either list (symbol, name, category, kind).
 *    The items of the whole catalog are created once; the catalog list model
 *    is re-filled with a subset of them on every filter change.
 *  - Rows are built by a GtkSignalListItemFactory shared by both lists. The
 *    rows currently bound are tracked in a set so a change of the watchlist
 *    can update their check buttons in place (keeping keyboard focus).
 *  - A search request carries a weak reference to the picker and its own
 *    GCancellable; it is applied only while that cancellable is still the
 *    picker's current one.
 */
#include "stk-picker.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Item                                                                     */
/* ------------------------------------------------------------------------ */

#define STK_TYPE_PICKER_ITEM (stk_picker_item_get_type())
G_DECLARE_FINAL_TYPE(StkPickerItem, stk_picker_item, STK, PICKER_ITEM, GObject)

struct _StkPickerItem {
  GObject parent_instance;
  char *symbol;   /* valid symbol, ASCII upper-case */
  char *name;     /* valid UTF-8, no control characters, <= watchlist name cap */
  char *category; /* "" for search hits */
  StkKind kind;
};

G_DEFINE_TYPE(StkPickerItem, stk_picker_item, G_TYPE_OBJECT)

static void
stk_picker_item_finalize(GObject *object)
{
  StkPickerItem *self = STK_PICKER_ITEM(object);
  g_free(self->symbol);
  g_free(self->name);
  g_free(self->category);
  G_OBJECT_CLASS(stk_picker_item_parent_class)->finalize(object);
}

static void
stk_picker_item_class_init(StkPickerItemClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = stk_picker_item_finalize;
}

static void
stk_picker_item_init(StkPickerItem *self)
{
  (void)self;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Returns a newly allocated, valid UTF-8 copy of @text (NULL = ""), cut to at
 * most @max_bytes bytes on a character boundary, in which every control
 * character (C0, DEL, C1, including newline and tab) is replaced by spaces.
 */
static char *
sanitize_text(const char *text, gsize max_bytes)
{
  char *out = g_utf8_make_valid(text != NULL ? text : "", -1);
  if (strlen(out) > max_bytes) {
    char *cut = out + max_bytes;
    while (cut > out && ((unsigned char)*cut & 0xC0) == 0x80)
      cut--;
    *cut = '\0';
  }
  for (char *p = out; *p != '\0';) {
    char *next = g_utf8_next_char(p);
    if (g_unichar_iscntrl(g_utf8_get_char(p)))
      memset(p, ' ', (gsize)(next - p));
    p = next;
  }
  return out;
}

static const char *
kind_label(StkKind kind)
{
  switch (kind) {
  case STK_KIND_STOCK:
    return "Stock";
  case STK_KIND_ETF:
    return "ETF";
  case STK_KIND_FUND:
    return "Fund";
  case STK_KIND_INDEX:
    return "Index";
  }
  return "";
}

static gboolean
kind_is_valid(StkKind kind)
{
  return kind == STK_KIND_STOCK || kind == STK_KIND_ETF || kind == STK_KIND_FUND ||
         kind == STK_KIND_INDEX;
}

/* Returns a newly allocated copy of the text of @entry without surrounding
 * whitespace. */
static char *
entry_text_stripped(GtkWidget *entry)
{
  return g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
}

/* @symbol must pass stk_symbol_is_valid(); name/category are sanitised. */
static StkPickerItem *
item_new(const char *symbol, const char *name, const char *category, StkKind kind)
{
  StkPickerItem *item = g_object_new(STK_TYPE_PICKER_ITEM, NULL);
  item->symbol = g_ascii_strup(symbol, -1);
  item->name = sanitize_text(name, STK_WATCHLIST_MAX_NAME_BYTES);
  item->category = sanitize_text(category, STK_WATCHLIST_MAX_NAME_BYTES);
  item->kind = kind;
  return item;
}

/* ------------------------------------------------------------------------ */
/* Picker                                                                   */
/* ------------------------------------------------------------------------ */

struct _StkPicker {
  GtkWindow parent_instance;

  const StkCatalog *catalog; /* borrowed; NULL after dispose */
  StkWatchlist *watchlist;   /* strong; NULL after dispose */
  gulong watchlist_handler;
  StkProvider *provider; /* strong; NULL after dispose */

  GListModel *toplevels; /* GTK's list of toplevel windows; strong */
  gulong toplevels_handler;
  gboolean destroyed; /* gtk_window_destroy() ran; the object may still be alive */

  GtkWidget *filter_entry;
  GtkWidget *kind_dropdown;
  GtkWidget *category_dropdown;
  GtkWidget *search_entry;
  GtkWidget *status;
  GtkWidget *results_box;
  gboolean status_is_edit_error; /* the status line shows a refused edit */

  GPtrArray *catalog_items; /* StkPickerItem, index == catalog entry index */
  GListStore *catalog_store;
  GListStore *results_store;

  GHashTable *bound_rows; /* set of Row* currently bound to an item */

  guint debounce_id;
  GCancellable *search_cancellable; /* the in-flight request, or NULL */
};

G_DEFINE_TYPE(StkPicker, stk_picker, GTK_TYPE_WINDOW)

enum { SIGNAL_WATCHLIST_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* "" or NULL @message hides the status line. */
static void
set_status(StkPicker *self, const char *message, gboolean is_error)
{
  gboolean show = message != NULL && message[0] != '\0';
  self->status_is_edit_error = FALSE;
  gtk_label_set_text(GTK_LABEL(self->status), show ? message : "");
  gtk_widget_set_visible(self->status, show);
  if (show && is_error)
    gtk_widget_add_css_class(self->status, "error");
  else
    gtk_widget_remove_css_class(self->status, "error");
}

/* ------------------------------------------------------------------------ */
/* Rows                                                                     */
/* ------------------------------------------------------------------------ */

#define ROW_KEY "stk-picker-row"

typedef struct {
  StkPicker *picker; /* the row's widgets are descendants of the picker */
  GtkWidget *check;
  GtkWidget *symbol;
  GtkWidget *name;
  GtkWidget *category;
  GtkWidget *kind;
  StkPickerItem *item; /* strong while bound, else NULL */
  gboolean syncing;    /* TRUE while the check button is set programmatically */
} Row;

static void
row_free(gpointer data)
{
  Row *row = data;
  g_clear_object(&row->item);
  g_free(row);
}

/* Makes the check button show whether the bound item is in the watchlist. */
static void
row_sync(Row *row)
{
  StkPicker *self = row->picker;
  if (row->item == NULL || self->watchlist == NULL)
    return;
  gboolean in_list = stk_watchlist_contains(self->watchlist, row->item->symbol);
  if (gtk_check_button_get_active(GTK_CHECK_BUTTON(row->check)) != in_list) {
    row->syncing = TRUE;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(row->check), in_list);
    row->syncing = FALSE;
  }
}

/*
 * Adds (@want) or removes @item through the watchlist API. A refused add shows
 * its reason inline. Returns TRUE iff the watchlist changed. Handlers of the
 * watchlist's "changed" signal run inside the call.
 */
static gboolean
edit_watchlist(StkPicker *self, StkPickerItem *item, gboolean want)
{
  if (self->watchlist == NULL || want == stk_watchlist_contains(self->watchlist, item->symbol))
    return FALSE;
  if (want) {
    if (!stk_watchlist_add(self->watchlist, item->symbol, item->name, item->kind)) {
      g_autofree char *msg = NULL;
      if (stk_watchlist_get_n_entries(self->watchlist) >= STK_WATCHLIST_MAX_ENTRIES)
        msg = g_strdup_printf("The watchlist is full (%d entries).", STK_WATCHLIST_MAX_ENTRIES);
      else
        msg = g_strdup_printf("Could not add %s to the watchlist.", item->symbol);
      set_status(self, msg, TRUE);
      self->status_is_edit_error = TRUE;
      return FALSE;
    }
    return TRUE;
  }
  return stk_watchlist_remove(self->watchlist, item->symbol);
}

/*
 * The one place where the user adds/removes @item (check button or row
 * activation): edits the watchlist and, when it changed, emits
 * "watchlist-changed". Returns TRUE iff the watchlist changed.
 *
 * The watchlist runs foreign "changed" handlers inside the edit, and the
 * emission runs foreign "watchlist-changed" handlers; any of them may destroy
 * the picker. gtk_window_destroy() only drops GTK's reference, so the picker
 * and the item are held until this function returns, and the edit finishes
 * on live objects. @self is not touched after the emission. A picker that was
 * destroyed before the edit refuses it; one destroyed during the edit keeps
 * the edit (the watchlist did change) but emits nothing, because a destroyed
 * picker emits nothing.
 */
static gboolean
apply_edit(StkPicker *self, StkPickerItem *item, gboolean want)
{
  g_autoptr(StkPicker) hold = g_object_ref(self);
  g_autoptr(StkPickerItem) keep = g_object_ref(item);

  if (self->destroyed || !edit_watchlist(self, item, want))
    return FALSE;
  if (self->status_is_edit_error)
    set_status(self, NULL, FALSE); /* the refusal is outdated once an edit succeeds */
  if (!self->destroyed)
    g_signal_emit(self, signals[SIGNAL_WATCHLIST_CHANGED], 0);
  return TRUE;
}

static void
on_row_toggled(GtkCheckButton *check, gpointer user_data)
{
  Row *row = user_data;
  if (row->syncing || row->item == NULL)
    return;
  /* On success the picker, and with it @row, may be gone: no use of @row after
   * apply_edit() reports a change. Without a change nothing foreign ran. */
  if (!apply_edit(row->picker, row->item, gtk_check_button_get_active(check)))
    row_sync(row); /* reverts the button when the edit was refused */
}

static void
factory_setup(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  (void)factory;
  Row *row = g_new0(Row, 1);
  row->picker = user_data;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(box, 6);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 3);
  gtk_widget_set_margin_bottom(box, 3);

  row->check = gtk_check_button_new();
  gtk_widget_set_name(row->check, "stk-picker-toggle");
  gtk_widget_set_valign(row->check, GTK_ALIGN_CENTER);
  g_signal_connect(row->check, "toggled", G_CALLBACK(on_row_toggled), row);
  gtk_box_append(GTK_BOX(box), row->check);

  row->symbol = gtk_label_new(NULL);
  gtk_widget_set_name(row->symbol, "stk-picker-symbol");
  gtk_label_set_xalign(GTK_LABEL(row->symbol), 0.0f);
  gtk_label_set_width_chars(GTK_LABEL(row->symbol), 8);
  PangoAttrList *bold = pango_attr_list_new();
  pango_attr_list_insert(bold, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
  gtk_label_set_attributes(GTK_LABEL(row->symbol), bold);
  pango_attr_list_unref(bold);
  gtk_box_append(GTK_BOX(box), row->symbol);

  GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(info, TRUE);
  row->name = gtk_label_new(NULL);
  gtk_widget_set_name(row->name, "stk-picker-name");
  gtk_label_set_xalign(GTK_LABEL(row->name), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(row->name), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(info), row->name);
  row->category = gtk_label_new(NULL);
  gtk_widget_set_name(row->category, "stk-picker-category-label");
  gtk_widget_add_css_class(row->category, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(row->category), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(row->category), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(info), row->category);
  gtk_box_append(GTK_BOX(box), info);

  row->kind = gtk_label_new(NULL);
  gtk_widget_set_name(row->kind, "stk-picker-kind-label");
  gtk_box_append(GTK_BOX(box), row->kind);

  g_object_set_data_full(G_OBJECT(box), ROW_KEY, row, row_free);
  gtk_list_item_set_child(list_item, box);
}

static Row *
list_item_row(GtkListItem *list_item)
{
  GtkWidget *box = gtk_list_item_get_child(list_item);
  return box != NULL ? g_object_get_data(G_OBJECT(box), ROW_KEY) : NULL;
}

static void
factory_bind(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  (void)factory;
  StkPicker *self = user_data;
  Row *row = list_item_row(list_item);
  StkPickerItem *item = gtk_list_item_get_item(list_item);
  if (row == NULL || item == NULL)
    return;

  g_clear_object(&row->item);
  row->item = g_object_ref(item);
  gtk_label_set_text(GTK_LABEL(row->symbol), item->symbol);
  gtk_label_set_text(GTK_LABEL(row->name), item->name);
  gtk_label_set_text(GTK_LABEL(row->category), item->category);
  gtk_widget_set_visible(row->category, item->category[0] != '\0');
  gtk_label_set_text(GTK_LABEL(row->kind), kind_label(item->kind));

  g_autofree char *label =
      item->name[0] != '\0' ? g_strdup_printf("%s, %s", item->symbol, item->name)
                            : g_strdup(item->symbol);
  gtk_accessible_update_property(GTK_ACCESSIBLE(row->check), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 label, -1);

  g_hash_table_add(self->bound_rows, row);
  row_sync(row);
}

static void
factory_unbind(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  (void)factory;
  StkPicker *self = user_data;
  Row *row = list_item_row(list_item);
  if (row == NULL)
    return;
  g_hash_table_remove(self->bound_rows, row);
  g_clear_object(&row->item);
}

static void
on_watchlist_changed(StkWatchlist *watchlist, gpointer user_data)
{
  (void)watchlist;
  StkPicker *self = user_data;
  /* Only this picker's own widgets react below, but the picker is held anyway
   * so that nothing in here can outlive it. */
  g_autoptr(StkPicker) hold = g_object_ref(self);
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, self->bound_rows);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    row_sync(key);
}

static void
on_list_activate(GtkListView *view, guint position, gpointer user_data)
{
  StkPicker *self = user_data;
  if (self->watchlist == NULL)
    return;
  g_autoptr(StkPickerItem) item =
      g_list_model_get_item(G_LIST_MODEL(gtk_list_view_get_model(view)), position);
  if (item != NULL)
    apply_edit(self, item, !stk_watchlist_contains(self->watchlist, item->symbol));
}

/* ------------------------------------------------------------------------ */
/* Catalog filtering                                                        */
/* ------------------------------------------------------------------------ */

/* Re-fills the catalog list with the entries passing the three filters. */
static void
refilter(StkPicker *self)
{
  if (self->catalog == NULL)
    return;

  guint kind_index = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->kind_dropdown));
  StkCatalogKindFilter kind = kind_index == 1   ? STK_CATALOG_FILTER_ETF
                              : kind_index == 2 ? STK_CATALOG_FILTER_FUND
                              : kind_index == 3 ? STK_CATALOG_FILTER_INDEX
                                                : STK_CATALOG_FILTER_ALL;

  const char *category = NULL;
  guint cat_index = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->category_dropdown));
  GtkStringList *cats =
      GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(self->category_dropdown)));
  if (cat_index > 0 && cat_index != GTK_INVALID_LIST_POSITION && cats != NULL)
    category = gtk_string_list_get_string(cats, cat_index);

  g_autofree char *query = entry_text_stripped(self->filter_entry);
  gsize n_entries = 0;
  gsize n_hits = 0;
  const StkCatalogEntry *entries = stk_catalog_entries(self->catalog, &n_entries);
  g_autofree const StkCatalogEntry **hits =
      stk_catalog_filter(self->catalog, kind, query, &n_hits);

  g_autoptr(GPtrArray) selected = g_ptr_array_sized_new((guint)n_hits);
  for (gsize i = 0; i < n_hits; i++) {
    if (category != NULL && strcmp(hits[i]->category, category) != 0)
      continue;
    g_ptr_array_add(selected, g_ptr_array_index(self->catalog_items, (guint)(hits[i] - entries)));
  }
  g_list_store_splice(self->catalog_store, 0,
                      g_list_model_get_n_items(G_LIST_MODEL(self->catalog_store)),
                      selected->pdata, selected->len);
}

static void
on_filter_changed(GtkWidget *widget, gpointer user_data)
{
  (void)widget;
  refilter(user_data);
}

static void
on_dropdown_selected(GObject *object, GParamSpec *pspec, gpointer user_data)
{
  (void)object;
  (void)pspec;
  refilter(user_data);
}

/* ------------------------------------------------------------------------ */
/* Search                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
  GWeakRef picker;
  GCancellable *cancellable; /* identifies the request; strong */
} SearchRequest;

/* Cancels the pending debounce timer and the in-flight request. Idempotent.
 * The request stops being the current one BEFORE it is cancelled, so a
 * provider that completes from inside the cancellation finds it stale. */
static void
cancel_search(StkPicker *self)
{
  g_clear_handle_id(&self->debounce_id, g_source_remove);
  g_autoptr(GCancellable) cancellable = g_steal_pointer(&self->search_cancellable);
  if (cancellable != NULL)
    g_cancellable_cancel(cancellable);
}

static void
clear_results(StkPicker *self)
{
  g_list_store_remove_all(self->results_store);
  gtk_widget_set_visible(self->results_box, FALSE);
}

static void
apply_hits(StkPicker *self, GPtrArray *hits)
{
  g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; hits != NULL && i < hits->len && items->len < STK_PICKER_MAX_RESULTS; i++) {
    const StkSearchHit *hit = g_ptr_array_index(hits, i);
    if (hit == NULL || !stk_symbol_is_valid(hit->symbol) || !kind_is_valid(hit->kind))
      continue;
    g_ptr_array_add(items, item_new(hit->symbol, hit->name, NULL, hit->kind));
  }
  g_list_store_splice(self->results_store, 0,
                      g_list_model_get_n_items(G_LIST_MODEL(self->results_store)), items->pdata,
                      items->len);
  gtk_widget_set_visible(self->results_box, items->len > 0);
  set_status(self, items->len > 0 ? NULL : "No results", FALSE);
}

static void
on_search_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  SearchRequest *req = user_data;
  GError *error = NULL;
  GPtrArray *hits = stk_provider_search_finish(STK_PROVIDER(source), result, &error);

  /* Only the latest request may change the UI. A request that is no longer
   * the picker's current one was cancelled or superseded (even if the provider
   * delivered a result anyway), or the picker is gone. */
  StkPicker *self = g_weak_ref_get(&req->picker);
  if (self != NULL && self->search_cancellable == req->cancellable) {
    g_clear_object(&self->search_cancellable);
    if (error == NULL) {
      apply_hits(self, hits);
    } else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      set_status(self, NULL, FALSE);
    } else {
      g_autofree char *text = sanitize_text(error->message, STK_PICKER_MAX_MESSAGE_BYTES);
      g_autofree char *msg = g_strdup_printf("Search failed: %s", text);
      clear_results(self);
      set_status(self, msg, TRUE);
    }
  }

  g_clear_object(&self);
  g_clear_error(&error);
  g_clear_pointer(&hits, g_ptr_array_unref);
  g_weak_ref_clear(&req->picker);
  g_object_unref(req->cancellable);
  g_free(req);
}

static gboolean
on_debounce_fired(gpointer user_data)
{
  StkPicker *self = user_data;
  self->debounce_id = 0;

  /* The timer only runs for non-empty text and is cancelled by dispose. */
  g_autofree char *text = entry_text_stripped(self->search_entry);

  SearchRequest *req = g_new0(SearchRequest, 1);
  g_weak_ref_init(&req->picker, self);
  self->search_cancellable = g_cancellable_new();
  req->cancellable = g_object_ref(self->search_cancellable);
  set_status(self, "Searching\xE2\x80\xA6", FALSE);
  stk_provider_search_async(self->provider, text, req->cancellable, on_search_done, req);
  return G_SOURCE_REMOVE;
}

static void
on_search_changed(GtkWidget *widget, gpointer user_data)
{
  (void)widget;
  StkPicker *self = user_data;
  if (self->provider == NULL)
    return;

  cancel_search(self);
  g_autofree char *text = entry_text_stripped(self->search_entry);
  if (text[0] == '\0') {
    clear_results(self);
    set_status(self, NULL, FALSE);
    return;
  }
  self->debounce_id = g_timeout_add(STK_PICKER_SEARCH_DELAY_MS, on_debounce_fired, self);
}

/* ------------------------------------------------------------------------ */
/* Window                                                                   */
/* ------------------------------------------------------------------------ */

static gboolean
on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, gpointer user_data)
{
  (void)controller;
  (void)keycode;
  if (keyval != GDK_KEY_Escape || (state & gtk_accelerator_get_default_mod_mask()) != 0)
    return GDK_EVENT_PROPAGATE;
  gtk_window_close(GTK_WINDOW(user_data));
  return GDK_EVENT_STOP;
}

/*
 * gtk_window_destroy() removes the window from GTK's list of toplevels but,
 * while anybody else holds a reference, does not dispose it. That removal is
 * how the picker learns that it is gone for good: from then on it cancels its
 * search, issues no new one, accepts no edits and emits nothing. (Closing
 * alone is not destruction: a close request may be vetoed, or only hide the
 * window.)
 */
static void
on_toplevels_changed(GListModel *toplevels, guint position, guint removed, guint added,
                     gpointer user_data)
{
  (void)position;
  (void)added;
  StkPicker *self = user_data;
  if (removed == 0 || self->destroyed)
    return;
  for (guint i = 0, n = g_list_model_get_n_items(toplevels); i < n; i++) {
    g_autoptr(GObject) window = g_list_model_get_item(toplevels, i);
    if (window == G_OBJECT(self))
      return;
  }
  self->destroyed = TRUE;
  cancel_search(self);
  set_status(self, NULL, FALSE); /* no "Searching..." without a request */
}

static void
stk_picker_dispose(GObject *object)
{
  StkPicker *self = STK_PICKER(object);
  cancel_search(self);
  if (self->toplevels_handler != 0) {
    g_signal_handler_disconnect(self->toplevels, self->toplevels_handler);
    self->toplevels_handler = 0;
  }
  g_clear_object(&self->toplevels);
  if (self->watchlist_handler != 0) {
    g_signal_handler_disconnect(self->watchlist, self->watchlist_handler);
    self->watchlist_handler = 0;
  }
  g_clear_object(&self->watchlist);
  g_clear_object(&self->provider);
  self->catalog = NULL;
  G_OBJECT_CLASS(stk_picker_parent_class)->dispose(object);
}

static void
stk_picker_finalize(GObject *object)
{
  StkPicker *self = STK_PICKER(object);
  g_clear_pointer(&self->bound_rows, g_hash_table_unref);
  g_clear_pointer(&self->catalog_items, g_ptr_array_unref);
  g_clear_object(&self->catalog_store);
  g_clear_object(&self->results_store);
  G_OBJECT_CLASS(stk_picker_parent_class)->finalize(object);
}

static void
stk_picker_class_init(StkPickerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = stk_picker_dispose;
  object_class->finalize = stk_picker_finalize;

  signals[SIGNAL_WATCHLIST_CHANGED] =
      g_signal_new("watchlist-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                   NULL, NULL, G_TYPE_NONE, 0);
}

static GtkWidget *
make_list(StkPicker *self, GListStore *store, const char *name, GtkWidget **scrolled_out)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(factory_setup), self);
  g_signal_connect(factory, "bind", G_CALLBACK(factory_bind), self);
  g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind), self);
  GtkNoSelection *selection = gtk_no_selection_new(g_object_ref(G_LIST_MODEL(store)));
  GtkWidget *view = gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory);
  gtk_widget_set_name(view, name);
  /* Tab visits one row (its check button included) and then leaves the list;
   * the arrow keys move between rows. Otherwise Tab would step through every
   * row of a catalog of hundreds of entries. */
  gtk_list_view_set_tab_behavior(GTK_LIST_VIEW(view), GTK_LIST_TAB_ITEM);
  g_signal_connect(view, "activate", G_CALLBACK(on_list_activate), self);

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 120);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), view);
  gtk_widget_set_vexpand(scrolled, TRUE);
  *scrolled_out = scrolled;
  return view;
}

static GtkWidget *
make_entry(const char *name, const char *placeholder, const char *accessible_label)
{
  GtkWidget *entry = gtk_entry_new();
  gtk_widget_set_name(entry, name);
  gtk_entry_set_max_length(GTK_ENTRY(entry), STK_PICKER_MAX_QUERY_CHARS);
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
  gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 accessible_label, -1);
  return entry;
}

static void
stk_picker_init(StkPicker *self)
{
  self->bound_rows = g_hash_table_new(g_direct_hash, g_direct_equal);
  self->catalog_store = g_list_store_new(STK_TYPE_PICKER_ITEM);
  self->results_store = g_list_store_new(STK_TYPE_PICKER_ITEM);

  gtk_window_set_title(GTK_WINDOW(self), "Add to watchlist");
  gtk_window_set_default_size(GTK_WINDOW(self), 520, 640);
  gtk_widget_add_css_class(GTK_WIDGET(self), "stk-picker");

  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), keys);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(root, 12);
  gtk_widget_set_margin_end(root, 12);
  gtk_widget_set_margin_top(root, 12);
  gtk_widget_set_margin_bottom(root, 12);
  gtk_window_set_child(GTK_WINDOW(self), root);

  self->filter_entry =
      make_entry("stk-picker-filter-entry", "Filter the catalog", "Filter the catalog");
  g_signal_connect(self->filter_entry, "changed", G_CALLBACK(on_filter_changed), self);
  gtk_box_append(GTK_BOX(root), self->filter_entry);

  GtkWidget *drops = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  const char *const kinds[] = { "All kinds", "ETFs", "Mutual funds", "Indices", NULL };
  self->kind_dropdown = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(kinds)), NULL);
  gtk_widget_set_name(self->kind_dropdown, "stk-picker-kind");
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->kind_dropdown),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Kind", -1);
  g_signal_connect(self->kind_dropdown, "notify::selected", G_CALLBACK(on_dropdown_selected),
                   self);
  gtk_box_append(GTK_BOX(drops), self->kind_dropdown);
  const char *const all_categories[] = { "All categories", NULL };
  self->category_dropdown =
      gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(all_categories)), NULL);
  gtk_widget_set_name(self->category_dropdown, "stk-picker-category");
  gtk_widget_set_hexpand(self->category_dropdown, TRUE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->category_dropdown),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Category", -1);
  g_signal_connect(self->category_dropdown, "notify::selected",
                   G_CALLBACK(on_dropdown_selected), self);
  gtk_box_append(GTK_BOX(drops), self->category_dropdown);
  gtk_box_append(GTK_BOX(root), drops);

  self->search_entry = make_entry("stk-picker-search-entry", "Search symbols and names online",
                                  "Search symbols and names online");
  g_signal_connect(self->search_entry, "changed", G_CALLBACK(on_search_changed), self);
  gtk_box_append(GTK_BOX(root), self->search_entry);

  self->status = gtk_label_new(NULL);
  gtk_widget_set_name(self->status, "stk-picker-status");
  gtk_label_set_xalign(GTK_LABEL(self->status), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(self->status), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->status), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(self->status), 50);
  gtk_widget_set_visible(self->status, FALSE);
  gtk_box_append(GTK_BOX(root), self->status);

  GtkWidget *scrolled = NULL;
  make_list(self, self->results_store, "stk-picker-results", &scrolled);
  self->results_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  gtk_widget_set_vexpand(self->results_box, TRUE);
  GtkWidget *heading = gtk_label_new("Search results");
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
  gtk_box_append(GTK_BOX(self->results_box), heading);
  gtk_box_append(GTK_BOX(self->results_box), scrolled);
  gtk_widget_set_visible(self->results_box, FALSE);
  gtk_box_append(GTK_BOX(root), self->results_box);

  make_list(self, self->catalog_store, "stk-picker-catalog", &scrolled);
  GtkWidget *catalog_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  gtk_widget_set_vexpand(catalog_box, TRUE);
  heading = gtk_label_new("Fidelity catalog");
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
  gtk_box_append(GTK_BOX(catalog_box), heading);
  gtk_box_append(GTK_BOX(catalog_box), scrolled);
  gtk_box_append(GTK_BOX(root), catalog_box);

  self->toplevels = g_object_ref(gtk_window_get_toplevels());
  self->toplevels_handler =
      g_signal_connect(self->toplevels, "items-changed", G_CALLBACK(on_toplevels_changed), self);
}

static int
compare_strings(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

GtkWidget *
stk_picker_new(const StkCatalog *catalog, StkWatchlist *watchlist, StkProvider *provider)
{
  g_return_val_if_fail(catalog != NULL, NULL);
  g_return_val_if_fail(STK_IS_WATCHLIST(watchlist), NULL);
  g_return_val_if_fail(STK_IS_PROVIDER(provider), NULL);

  StkPicker *self = g_object_new(STK_TYPE_PICKER, NULL);
  self->catalog = catalog;
  self->watchlist = g_object_ref(watchlist);
  self->provider = g_object_ref(provider);
  self->watchlist_handler =
      g_signal_connect(watchlist, "changed", G_CALLBACK(on_watchlist_changed), self);

  gsize n_entries = 0;
  const StkCatalogEntry *entries = stk_catalog_entries(catalog, &n_entries);
  self->catalog_items = g_ptr_array_new_full((guint)n_entries, g_object_unref);
  g_autoptr(GPtrArray) categories = g_ptr_array_new();
  for (gsize i = 0; i < n_entries; i++) {
    g_ptr_array_add(self->catalog_items,
                    item_new(entries[i].symbol, entries[i].name, entries[i].category,
                             entries[i].kind));
    if (entries[i].category[0] != '\0')
      g_ptr_array_add(categories, (gpointer)entries[i].category);
  }

  g_ptr_array_sort(categories, compare_strings);
  GtkStringList *category_list = gtk_string_list_new(NULL);
  gtk_string_list_append(category_list, "All categories");
  const char *previous = NULL;
  for (guint i = 0; i < categories->len; i++) {
    const char *category = g_ptr_array_index(categories, i);
    if (previous == NULL || strcmp(previous, category) != 0)
      gtk_string_list_append(category_list, category);
    previous = category;
  }
  gtk_drop_down_set_model(GTK_DROP_DOWN(self->category_dropdown), G_LIST_MODEL(category_list));
  g_object_unref(category_list);

  refilter(self);
  return GTK_WIDGET(self);
}
