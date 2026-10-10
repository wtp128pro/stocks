/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
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

/*
 * G_DEFINE_TYPE() below generates stk_picker_item_get_type() (registers the type once;
 * thread-safe, any thread) and the static instance/class trampolines
 * (stk_picker_item_class_intern_init() and the parent-class/private-offset bookkeeping), which
 * call stk_picker_item_class_init() and stk_picker_item_init(). They own nothing; the
 * documented functions that follow are the only hand-written parts of the type.
 */
G_DEFINE_TYPE(StkPickerItem, stk_picker_item, G_TYPE_OBJECT)

/**
 * stk_picker_item_finalize:
 * @object: (not nullable): the StkPickerItem being finalized; the GObject machinery passes an
 *   instance of this type.
 *
 * GObject finalize: frees the three strings of the item and chains up.
 *
 * Preconditions:
 *  - called once by GObject when the last reference is dropped (never called directly); the
 *    strings are either NULL or owned by the item (item_new() sets all three).
 * Postconditions:
 *  - symbol, name and category are freed; the parent finalize has run.
 * Thread affinity: main thread only (items are created, referenced and dropped by the picker
 *   and by GTK list widgets on the main thread).
 * Ownership: @object is owned by GObject; the item owns its three strings.
 */
static void
stk_picker_item_finalize(GObject *object)
{
  StkPickerItem *self = STK_PICKER_ITEM(object);
  g_free(self->symbol);
  g_free(self->name);
  g_free(self->category);
  G_OBJECT_CLASS(stk_picker_item_parent_class)->finalize(object);
}

/**
 * stk_picker_item_class_init:
 * @klass: (not nullable): the class structure being initialised, passed by GObject.
 *
 * GObject class initialiser: installs stk_picker_item_finalize().
 *
 * Preconditions:
 *  - called once by GObject during type registration (never called directly).
 * Postconditions:
 *  - the class' finalize vfunc is stk_picker_item_finalize().
 * Thread affinity: main thread only (runs once, when the type is first used).
 * Ownership: @klass is owned by the type system; nothing is allocated.
 */
static void
stk_picker_item_class_init(StkPickerItemClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = stk_picker_item_finalize;
}

/**
 * stk_picker_item_init:
 * @self: (not nullable): the new instance, zero-initialised by GObject.
 *
 * GObject instance initialiser. Nothing to do: the fields stay NULL/0 until item_new() fills
 * them.
 *
 * Preconditions:
 *  - called once by GObject while the instance is created (never called directly).
 * Postconditions:
 *  - @self is unchanged.
 * Thread affinity: main thread only (items are created by the picker).
 * Ownership: @self is owned by GObject; nothing is allocated.
 */
static void
stk_picker_item_init(StkPickerItem *self)
{
  (void)self;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

/**
 * sanitize_text:
 * @text: (nullable): the text to clean; NULL means "". Not modified.
 * @max_bytes: maximum byte length of the result (a cut is made on a character boundary, so
 *   the result may be shorter).
 *
 * Makes a display-safe copy of @text: valid UTF-8 (invalid bytes become U+FFFD), cut to at most
 * @max_bytes bytes on a character boundary, every control character (C0, DEL, C1, including
 * newline and tab) replaced by as many spaces as it had bytes. Unlike stk_sanitize_text() of
 * the row widgets (src/ui), which scans at most its own byte cap of the input and also replaces
 * line/paragraph separators and format characters, this function applies no cap to the input
 * and only handles control characters. The strlen() below measures the copy made by
 * g_utf8_make_valid() in this same function, which is always NUL-terminated.
 *
 * Preconditions:
 *  - @text is NULL or NUL-terminated; g_utf8_make_valid() scans it up to the NUL and no input
 *    cap is applied here, so the caller passes text whose size it already bounds (catalog
 *    strings, the provider's hit names and error messages).
 * Postconditions:
 *  - the result is valid UTF-8, strlen(result) <= @max_bytes, contains no control character.
 *  - no state is changed.
 * Thread affinity: main thread only (the module's threading rule; the function itself is pure).
 * Ownership: @text is only read; the caller owns the returned string.
 *
 * Returns: (transfer full): the newly allocated cleaned copy; never NULL.
 */
static char *
sanitize_text(const char *text, gsize max_bytes)
{
  char *out = g_utf8_make_valid(text != NULL ? text : "", -1);
  /* strlen() of the GLib-created, NUL-terminated copy made on the previous line. */
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

/**
 * kind_label:
 * @kind: the instrument class.
 *
 * Maps an instrument class to the label shown in a row.
 *
 * Preconditions:
 *  - none: any value of @kind is accepted.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread only (the module's threading rule; the function itself is pure).
 * Ownership: the result is a static string literal owned by the program.
 *
 * Returns: (transfer none): "Stock", "ETF", "Fund" or "Index"; "" for a value that is not a
 *   StkKind enumerator.
 */
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

/**
 * kind_is_valid:
 * @kind: the value to test.
 *
 * Tells whether @kind is one of the four StkKind enumerators.
 *
 * Preconditions:
 *  - none: any value of @kind is accepted.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread only (the module's threading rule; the function itself is pure).
 * Ownership: none (no pointers).
 *
 * Returns: TRUE iff @kind is STK_KIND_STOCK, STK_KIND_ETF, STK_KIND_FUND or STK_KIND_INDEX.
 */
static gboolean
kind_is_valid(StkKind kind)
{
  return kind == STK_KIND_STOCK || kind == STK_KIND_ETF || kind == STK_KIND_FUND ||
         kind == STK_KIND_INDEX;
}

/**
 * entry_text_stripped:
 * @entry: (not nullable): a GtkEditable widget (here always a GtkEntry).
 *
 * Copies the text of @entry without surrounding ASCII whitespace (g_strstrip()).
 *
 * Preconditions:
 *  - @entry is a live GtkEditable; called on the main thread.
 * Postconditions:
 *  - no state is changed; the entry's own text is not modified.
 * Thread affinity: main thread only.
 * Ownership: @entry is only read; the caller owns the returned string.
 *
 * Returns: (transfer full): the newly allocated stripped copy; never NULL (may be "").
 */
static char *
entry_text_stripped(GtkWidget *entry)
{
  return g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
}

/**
 * item_new:
 * @symbol: (not nullable): ticker; MUST pass stk_symbol_is_valid() (callers: the catalog
 *   entries in stk_picker_new(), and apply_hits(), which checks it first). Copied and
 *   ASCII-upper-cased.
 * @name: (nullable): display name; NULL means "". Copied through sanitize_text() and cut to
 *   STK_WATCHLIST_MAX_NAME_BYTES bytes.
 * @category: (nullable): category text; NULL means "". Copied through sanitize_text() and cut
 *   to STK_WATCHLIST_MAX_NAME_BYTES bytes.
 * @kind: instrument class stored as is; callers pass a valid StkKind.
 *
 * Creates one row item of either list.
 *
 * Preconditions:
 *  - @symbol satisfies stk_symbol_is_valid() (at most 15 bytes plus NUL); @name and @category
 *    are NULL or NUL-terminated.
 * Postconditions:
 *  - the item's symbol, name and category are non-NULL; name and category are valid UTF-8
 *    without control characters.
 * Thread affinity: main thread only.
 * Ownership: the arguments are only read (copied); the caller owns the returned item.
 *
 * Returns: (transfer full): the new item; never NULL.
 */
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

/*
 * G_DEFINE_TYPE() below generates stk_picker_get_type() (registers the type once; thread-safe,
 * any thread) and the static instance/class trampolines that call stk_picker_class_init() and
 * stk_picker_init(). They own nothing; the documented functions that follow are the only
 * hand-written parts of the type.
 */
G_DEFINE_TYPE(StkPicker, stk_picker, GTK_TYPE_WINDOW)

enum { SIGNAL_WATCHLIST_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/**
 * set_status:
 * @self: (not nullable): the picker; the status label must exist (the picker is constructed).
 * @message: (nullable): text for the status line; NULL or "" hides the line. Copied by GTK.
 * @is_error: whether the message is an error (adds the CSS class "error"; ignored when hidden).
 *
 * Shows or hides the inline status line and clears the "refused edit" marker.
 *
 * Preconditions:
 *  - @self is a live StkPicker whose widgets exist (it is not yet finalized); @message is NULL
 *    or NUL-terminated; main thread.
 * Postconditions:
 *  - the label shows @message (or "" and is hidden); the "error" class is set iff @message is
 *    non-empty and @is_error; status_is_edit_error is FALSE.
 * Thread affinity: main thread only.
 * Ownership: @self and @message are only read; nothing is retained.
 */
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

/* Row: the widgets of one list row plus the item it shows. It is owned by the row's root box
 * (g_object_set_data_full() with row_free()), i.e. it lives exactly as long as that box. */
#define ROW_KEY "stk-picker-row"

typedef struct {
  StkPicker *picker; /* raw: widgets are descendants of the picker; see factory_setup() */
  GHashTable *bound_rows; /* strong; the picker's set of bound rows, so row_free() can leave it */
  GtkWidget *check;
  GtkWidget *symbol;
  GtkWidget *name;
  GtkWidget *category;
  GtkWidget *kind;
  StkPickerItem *item; /* strong while bound, else NULL */
  gboolean syncing;    /* TRUE while the check button is set programmatically */
} Row;

/**
 * row_free:
 * @data: (not nullable): the Row* stored on the row's root box.
 *
 * GDestroyNotify of the box's ROW_KEY data: runs when the box is finalized. If GTK called
 * factory_unbind() before the widget went away, that already removed the row from the set of
 * bound rows; if it did not, this removes the row from the set so that the picker never
 * iterates a freed Row. The set is kept alive by the reference the row holds, independent of the
 * picker's own lifetime.
 *
 * Preconditions:
 *  - @data is a Row created by factory_setup() and not yet freed; called once.
 * Postconditions:
 *  - the row is not in the set of bound rows, its item reference is dropped and it is freed.
 * Thread affinity: main thread only.
 * Ownership: @data is owned by this call (freed here).
 */
static void
row_free(gpointer data)
{
  Row *row = data;
  g_hash_table_remove(row->bound_rows, row);
  g_hash_table_unref(row->bound_rows);
  g_clear_object(&row->item);
  g_free(row);
}

/**
 * row_sync:
 * @row: (not nullable): a row created by factory_setup(), alive.
 *
 * Makes the check button show whether the bound item is in the watchlist; keyboard focus is
 * kept because the button is only set when its state differs. Does nothing for an unbound row
 * or a disposed picker.
 *
 * Preconditions:
 *  - @row is a live Row whose picker is alive (not finalized); main thread.
 * Postconditions:
 *  - if the row is bound and the picker still has its watchlist, the check button state equals
 *    "item symbol is in the watchlist"; "toggled" handlers ran with syncing set, so no edit was
 *    made; otherwise nothing is changed.
 * Thread affinity: main thread only.
 * Ownership: @row is only used; nothing is retained.
 */
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

/**
 * edit_watchlist:
 * @self: (not nullable): the picker.
 * @item: (not nullable): the item to add or remove; its symbol is valid.
 * @want: TRUE to have the item in the watchlist, FALSE to have it removed.
 *
 * Adds (@want) or removes @item through the watchlist API. A refused add shows its reason
 * inline. Handlers of the watchlist's "changed" signal run inside the call.
 *
 * Preconditions:
 *  - @self and @item are live; the caller holds references on both for the whole call
 *    (apply_edit() does), because foreign handlers may destroy the picker; main thread.
 * Postconditions:
 *  - nothing changes (FALSE) if the watchlist is gone or already is in the wanted state;
 *    otherwise the instrument was added or removed; a refused add sets the status line to the
 *    reason (status_is_edit_error TRUE) and returns FALSE.
 * Thread affinity: main thread only.
 * Ownership: @self and @item are only used; nothing is retained.
 *
 * Returns: TRUE iff the watchlist changed.
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

/**
 * apply_edit:
 * @self: (not nullable): the picker.
 * @item: (not nullable): the item the user toggled or activated.
 * @want: TRUE to add the item to the watchlist, FALSE to remove it.
 *
 * The one place where the user adds/removes @item (check button or row activation): edits the
 * watchlist and, when it changed, emits "watchlist-changed".
 *
 * The watchlist runs foreign "changed" handlers inside the edit, and the emission runs foreign
 * "watchlist-changed" handlers; any of them may destroy the picker. gtk_window_destroy() only
 * drops GTK's reference, so the picker and the item are held until this function returns, and
 * the edit finishes on live objects. @self is not touched after the emission. A picker that was
 * destroyed before the edit refuses it; one destroyed during the edit keeps the edit (the
 * watchlist did change) but emits nothing, because a destroyed picker emits nothing.
 *
 * Preconditions:
 *  - @self is a StkPicker and @item a StkPickerItem that are alive at the call; main thread.
 * Postconditions:
 *  - FALSE: the watchlist was not changed (destroyed picker, already in the wanted state,
 *    refused add); a refused add leaves the reason in the status line.
 *  - TRUE: the watchlist changed, an outdated refusal message was cleared, and
 *    "watchlist-changed" was emitted unless the picker became destroyed.
 * Thread affinity: main thread only.
 * Ownership: @self and @item are referenced for the duration of the call and released on return.
 *
 * Returns: TRUE iff the watchlist changed.
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

/**
 * on_row_toggled:
 * @check: (not nullable): the row's check button.
 * @user_data: (not nullable): the Row* given to g_signal_connect() in factory_setup().
 *
 * "toggled" handler of a row's check button: adds or removes the bound item according to the
 * button's new state, or reverts the button when the edit did not happen.
 *
 * Preconditions:
 *  - emitted on the main thread by a live check button, which is a descendant of the box that
 *    owns the Row, so the Row is alive while its button can emit; @user_data is that Row.
 * Postconditions:
 *  - ignored while the row is syncing or unbound; otherwise see apply_edit(); on success the
 *    Row is not touched afterwards (the picker may have been destroyed), on failure row_sync()
 *    restores the button.
 * Thread affinity: main thread only.
 * Ownership: @check is only read; @user_data (the Row) is owned by the row's box.
 */
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

/**
 * factory_setup:
 * @factory: (not nullable): the list's GtkSignalListItemFactory (unused).
 * @list_item: (not nullable): the GtkListItem GTK created; receives the row's widgets.
 * @user_data: (not nullable): the StkPicker passed to g_signal_connect() in make_list().
 *
 * "setup" handler: builds the widgets of one row, creates its Row and stores it on the root
 * box; the list item takes the box as its child.
 *
 * Preconditions:
 *  - emitted on the main thread by GTK; @user_data is the picker. That raw pointer is safe
 *    because the factory is owned by the list view (gtk_list_view_new() takes it), the list view
 *    is a descendant of the picker, and the picker's dispose destroys its child widgets (and
 *    with them the list views) before it is finalized.
 *  - the picker's bound_rows exists (stk_picker_init() creates it before building any list).
 * Postconditions:
 *  - @list_item has a child box carrying the Row under ROW_KEY (freed by row_free() with the
 *    box); the row holds its own reference on the picker's set of bound rows.
 * Thread affinity: main thread only.
 * Ownership: @user_data is not referenced; the Row is owned by the box; @list_item owns the box.
 */
static void
factory_setup(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  (void)factory;
  Row *row = g_new0(Row, 1);
  row->picker = user_data;
  row->bound_rows = g_hash_table_ref(row->picker->bound_rows);

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

/**
 * list_item_row:
 * @list_item: (not nullable): a list item set up by factory_setup().
 *
 * Fetches the Row stored on the list item's child box.
 *
 * Preconditions:
 *  - @list_item is a live GtkListItem; main thread.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread only.
 * Ownership: the Row stays owned by the box.
 *
 * Returns: (transfer none) (nullable): the Row, or NULL if the list item has no child or the
 *   child carries none.
 */
static Row *
list_item_row(GtkListItem *list_item)
{
  GtkWidget *box = gtk_list_item_get_child(list_item);
  return box != NULL ? g_object_get_data(G_OBJECT(box), ROW_KEY) : NULL;
}

/**
 * factory_bind:
 * @factory: (not nullable): the list's factory (unused).
 * @list_item: (not nullable): the list item being bound to an item of the model.
 * @user_data: (not nullable): the StkPicker passed in make_list().
 *
 * "bind" handler: fills the row's labels from the item, sets the check button's accessible
 * label, adds the row to the set of bound rows and syncs the check button.
 *
 * Preconditions:
 *  - emitted on the main thread by GTK after factory_setup() for @list_item; @user_data is the
 *    picker (see factory_setup()); the item, when present, is a StkPickerItem (the stores hold
 *    only that type), whose strings are non-NULL (item_new()).
 * Postconditions:
 *  - without a Row or an item nothing happens; otherwise the row holds a reference on the item,
 *    shows its text and is a member of bound_rows.
 * Thread affinity: main thread only.
 * Ownership: the row takes its own reference on the item; nothing else is retained.
 */
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

/**
 * factory_unbind:
 * @factory: (not nullable): the list's factory (unused).
 * @list_item: (not nullable): the list item being unbound.
 * @user_data: (not nullable): the StkPicker passed in make_list().
 *
 * "unbind" handler: removes the row from the set of bound rows and drops its item reference.
 *
 * Preconditions:
 *  - emitted on the main thread by GTK for a list item that was set up; @user_data is the
 *    picker, and the picker's bound_rows exists (it is freed only in finalize, after the list
 *    view has been disposed). If GTK skipped this call, row_free() performs the removal.
 * Postconditions:
 *  - the row (if any) is not in bound_rows and holds no item.
 * Thread affinity: main thread only.
 * Ownership: the item reference of the row is released; nothing else is retained.
 */
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

/**
 * on_watchlist_changed:
 * @watchlist: (not nullable): the watchlist that emitted "changed" (unused).
 * @user_data: (not nullable): the StkPicker given to g_signal_connect() in stk_picker_new().
 *
 * Handler of the watchlist's "changed" signal: brings the check buttons of all bound rows in
 * line with the watchlist.
 *
 * Preconditions:
 *  - emitted on the main thread; @user_data is a live picker: the handler is disconnected in
 *    stk_picker_dispose() before the picker can be finalized.
 * Postconditions:
 *  - every row in bound_rows shows the current watchlist state (row_sync()); no edit is made.
 * Thread affinity: main thread only.
 * Ownership: @user_data is not referenced beyond the call (a temporary reference is held).
 */
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

/**
 * on_list_activate:
 * @view: (not nullable): the list view whose row was activated.
 * @position: index of the activated row in the view's model.
 * @user_data: (not nullable): the StkPicker passed in make_list().
 *
 * "activate" handler (Enter / double click): toggles the activated item's watchlist membership.
 *
 * Preconditions:
 *  - emitted on the main thread by GTK; @user_data is the picker (see factory_setup()); @view
 *    has a model.
 * Postconditions:
 *  - nothing happens if the watchlist is gone or @position has no item; otherwise see
 *    apply_edit().
 * Thread affinity: main thread only.
 * Ownership: the item obtained from the model is referenced for the call and released.
 */
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

/**
 * refilter:
 * @self: (not nullable): the picker.
 *
 * Re-fills the catalog list with the entries passing the three filters (kind, category,
 * substring). The kind and substring filters are applied by stk_catalog_filter(); the category
 * filter keeps entries whose category equals the selected one exactly. A hit that does not lie
 * inside the catalog's entry array (cannot happen: stk_catalog_filter() returns pointers into
 * it) is skipped rather than indexed.
 *
 * Preconditions:
 *  - @self is a live picker; if its catalog is still set (not disposed) it outlives the call
 *    and catalog_items holds one item per catalog entry, index == entry index (built by
 *    stk_picker_new() before the first call); main thread.
 * Postconditions:
 *  - catalog_store holds, in symbol order, the items of the entries that pass; unchanged if the
 *    picker was disposed.
 * Thread affinity: main thread only.
 * Ownership: the catalog is borrowed and only read; the items stay owned by catalog_items and
 *   are referenced by the store.
 */
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

  /* Only a capacity hint: clamp instead of truncating. */
  g_autoptr(GPtrArray) selected = g_ptr_array_sized_new((guint)MIN(n_hits, (gsize)G_MAXUINT));
  for (gsize i = 0; i < n_hits; i++) {
    if (category != NULL && !g_str_equal(hits[i]->category, category))
      continue;
    gssize slot = hits[i] - entries;
    if (slot < 0 || (gsize)slot >= self->catalog_items->len)
      continue;
    g_ptr_array_add(selected, g_ptr_array_index(self->catalog_items, (guint)slot));
  }
  g_list_store_splice(self->catalog_store, 0,
                      g_list_model_get_n_items(G_LIST_MODEL(self->catalog_store)),
                      selected->pdata, selected->len);
}

/**
 * on_filter_changed:
 * @widget: (not nullable): the filter entry (unused).
 * @user_data: (not nullable): the StkPicker passed to g_signal_connect() in stk_picker_init().
 *
 * "changed" handler of the filter entry: re-applies the filters.
 *
 * Preconditions:
 *  - emitted on the main thread by the picker's own entry; @user_data is the picker.
 * Postconditions:
 *  - see refilter().
 * Thread affinity: main thread only.
 * Ownership: @user_data is not referenced.
 */
static void
on_filter_changed(GtkWidget *widget, gpointer user_data)
{
  (void)widget;
  refilter(user_data);
}

/**
 * on_dropdown_selected:
 * @object: (not nullable): the drop-down (unused).
 * @pspec: the notified property (unused).
 * @user_data: (not nullable): the StkPicker passed to g_signal_connect() in stk_picker_init().
 *
 * "notify::selected" handler of the kind and category drop-downs: re-applies the filters.
 *
 * Preconditions:
 *  - emitted on the main thread by the picker's own drop-down; @user_data is the picker.
 * Postconditions:
 *  - see refilter().
 * Thread affinity: main thread only.
 * Ownership: @user_data is not referenced.
 */
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

/**
 * cancel_search:
 * @self: (not nullable): the picker.
 *
 * Cancels the pending debounce timer and the in-flight request. Idempotent. The request stops
 * being the current one BEFORE it is cancelled, so a provider that completes from inside the
 * cancellation finds it stale.
 *
 * Preconditions:
 *  - @self is a live picker; main thread.
 * Postconditions:
 *  - debounce_id is 0 and its source is removed; search_cancellable is NULL and the request
 *    that was current (if any) is cancelled; its SearchRequest is freed later by on_search_done().
 * Thread affinity: main thread only.
 * Ownership: the picker's reference on the cancellable is released; the request keeps its own.
 */
static void
cancel_search(StkPicker *self)
{
  g_clear_handle_id(&self->debounce_id, g_source_remove);
  g_autoptr(GCancellable) cancellable = g_steal_pointer(&self->search_cancellable);
  if (cancellable != NULL)
    g_cancellable_cancel(cancellable);
}

/**
 * clear_results:
 * @self: (not nullable): the picker.
 *
 * Empties the search results list and hides its box.
 *
 * Preconditions:
 *  - @self is a live picker (before finalize); main thread.
 * Postconditions:
 *  - results_store is empty and results_box is hidden.
 * Thread affinity: main thread only.
 * Ownership: @self is only used.
 */
static void
clear_results(StkPicker *self)
{
  g_list_store_remove_all(self->results_store);
  gtk_widget_set_visible(self->results_box, FALSE);
}

/**
 * apply_hits:
 * @self: (not nullable): the picker.
 * @hits: (nullable) (element-type StkSearchHit): the provider's hits; NULL is treated as empty.
 *   Not modified.
 *
 * Replaces the results list with at most STK_PICKER_MAX_RESULTS hits in provider order, skipping
 * NULL hits and hits with an invalid symbol or kind, and shows "No results" when nothing is left.
 *
 * Preconditions:
 *  - @self is a live picker; each element of @hits is NULL or a StkSearchHit with a
 *    NUL-terminated symbol and name (name may be NULL); main thread.
 * Postconditions:
 *  - results_store holds the listed hits; results_box is visible iff there is at least one; the
 *    status line is hidden, or shows "No results".
 * Thread affinity: main thread only.
 * Ownership: @hits stays owned by the caller; the new items are owned by the store.
 */
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

/**
 * on_search_done:
 * @source: (not nullable): the StkProvider the search was started on.
 * @result: (not nullable): the GAsyncResult of the search.
 * @user_data: (transfer full): the SearchRequest created by on_debounce_fired().
 *
 * GAsyncReadyCallback of stk_provider_search_async(). Applies the outcome only if the request is
 * still the picker's current one; a cancelled or superseded request, or a vanished picker, is
 * silent. The picker is reached only through the request's GWeakRef, so a late completion never
 * touches a finalized picker. Comparing the request's cancellable with the picker's current one
 * is free of address reuse (ABA): the request holds a strong reference on its cancellable until
 * the end of this function, so no newer cancellable can have the same address.
 *
 * Preconditions:
 *  - called exactly once, on the main thread, by the provider's GAsyncReadyCallback contract;
 *    @user_data is the SearchRequest that on_debounce_fired() allocated for this call.
 * Postconditions:
 *  - the SearchRequest (weak reference, cancellable reference) is released and freed; the hits
 *    and error returned by stk_provider_search_finish() are released. If the request was current:
 *    the picker's search_cancellable is cleared and, on success, the hits are listed; on
 *    cancellation the status line is cleared; on any other error the results are cleared and
 *    "Search failed: <message>" is shown (message through sanitize_text(), cut to
 *    STK_PICKER_MAX_MESSAGE_BYTES).
 * Thread affinity: main thread only.
 * Ownership: @source and @result are borrowed; @user_data is owned by this function (freed).
 */
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

/**
 * on_debounce_fired:
 * @user_data: (not nullable): the StkPicker passed to g_timeout_add() in on_search_changed().
 *
 * Debounce timer callback: starts the search for the text of the search entry.
 *
 * Preconditions:
 *  - runs on the main thread from the default main context; @user_data is a live picker whose
 *    provider is set: the source is armed only by on_search_changed() with a provider present,
 *    and cancel_search() (called by on_search_changed(), on_toplevels_changed() and
 *    stk_picker_dispose(), the last one before the provider is released) removes it, so it can
 *    neither fire after dispose nor outlive the picker. The cancellable is NULL when the timer
 *    fires, because arming the timer cancelled any request.
 * Postconditions:
 *  - debounce_id is 0; a new request is the picker's current one, "Searching..." is shown, and
 *    stk_provider_search_async() was called with the stripped entry text; on_search_done() will
 *    run once for it.
 * Thread affinity: main thread only.
 * Ownership: @user_data is not referenced; the new SearchRequest is owned by the callback.
 *
 * Returns: G_SOURCE_REMOVE always (the timer is one-shot).
 */
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

/**
 * on_search_changed:
 * @widget: (not nullable): the search entry (unused).
 * @user_data: (not nullable): the StkPicker passed to g_signal_connect() in stk_picker_init().
 *
 * "changed" handler of the search entry: cancels the pending/in-flight search and, for non-empty
 * text, restarts the debounce timer; empty text clears the results and the status line.
 *
 * Preconditions:
 *  - emitted on the main thread by the picker's own entry; @user_data is the picker.
 * Postconditions:
 *  - nothing happens after dispose (provider released); otherwise the previous search is
 *    cancelled and either the results are cleared (empty text) or debounce_id is a new timer
 *    of STK_PICKER_SEARCH_DELAY_MS.
 * Thread affinity: main thread only.
 * Ownership: @user_data is not referenced (the timer holds it raw; see on_debounce_fired()).
 */
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

/**
 * on_key_pressed:
 * @controller: (not nullable): the picker's key controller (unused).
 * @keyval: the key pressed.
 * @keycode: the hardware keycode (unused).
 * @state: the modifier state.
 * @user_data: (not nullable): the StkPicker (a GtkWindow) passed in stk_picker_init().
 *
 * Capture-phase key handler of the window: Escape without modifiers closes the window.
 *
 * Preconditions:
 *  - emitted on the main thread by a controller attached to the picker itself, so @user_data
 *    outlives it.
 * Postconditions:
 *  - Escape (no modifier of the default mask): gtk_window_close() was called and the event is
 *    stopped; any other key: nothing, the event propagates.
 * Thread affinity: main thread only.
 * Ownership: @user_data is not referenced.
 *
 * Returns: GDK_EVENT_STOP for a plain Escape, GDK_EVENT_PROPAGATE otherwise.
 */
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

/**
 * on_toplevels_changed:
 * @toplevels: (not nullable): GTK's list of toplevel windows.
 * @position: index of the change (unused).
 * @removed: number of removed windows.
 * @added: number of added windows (unused).
 * @user_data: (not nullable): the StkPicker passed in stk_picker_init().
 *
 * gtk_window_destroy() removes the window from GTK's list of toplevels but, while anybody else
 * holds a reference, does not dispose it. That removal is how the picker learns that it is gone
 * for good: from then on it cancels its search, issues no new one, accepts no edits and emits
 * nothing. (Closing alone is not destruction: a close request may be vetoed, or only hide the
 * window.)
 *
 * Preconditions:
 *  - emitted on the main thread; @user_data is a live picker: the handler is disconnected in
 *    stk_picker_dispose() before the picker can be finalized.
 * Postconditions:
 *  - ignored unless something was removed and the picker is no longer in the list; then
 *    destroyed is TRUE, the search is cancelled and the status line cleared.
 * Thread affinity: main thread only.
 * Ownership: @toplevels is only read; items fetched from it are released.
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

/**
 * stk_picker_dispose:
 * @object: (not nullable): the StkPicker being disposed.
 *
 * GObject dispose (may run more than once): cancels the search and the timer, disconnects the
 * handlers on GTK's toplevel list and on the watchlist, releases the references and forgets the
 * borrowed catalog, then chains up (which disposes the child widgets, whose factories call
 * factory_unbind() while bound_rows is still alive).
 *
 * Preconditions:
 *  - called by GObject (never directly), on the main thread.
 * Postconditions:
 *  - no timer, request, handler or reference to the toplevel list, watchlist or provider
 *    remains; the catalog pointer is NULL; a repeated call does nothing more.
 * Thread affinity: main thread only.
 * Ownership: @object is owned by GObject; the picker's references are released.
 */
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

/**
 * stk_picker_finalize:
 * @object: (not nullable): the StkPicker being finalized.
 *
 * GObject finalize: frees the set of bound rows, the catalog items and the two list stores, then
 * chains up. Rows that outlive the set (see row_free()) keep their own reference on it.
 *
 * Preconditions:
 *  - called once by GObject after dispose (never directly), on the main thread.
 * Postconditions:
 *  - the picker's own memory-owning fields are released.
 * Thread affinity: main thread only.
 * Ownership: @object is owned by GObject; the picker's references are released.
 */
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

/**
 * stk_picker_class_init:
 * @klass: (not nullable): the class structure being initialised, passed by GObject.
 *
 * GObject class initialiser: installs dispose/finalize and registers the "watchlist-changed"
 * signal.
 *
 * Preconditions:
 *  - called once by GObject during type registration (never called directly).
 * Postconditions:
 *  - the class vfuncs are set and signals[SIGNAL_WATCHLIST_CHANGED] is the signal id.
 * Thread affinity: main thread only (runs once, when the type is first used).
 * Ownership: @klass is owned by the type system; nothing is allocated except the signal.
 */
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

/**
 * make_list:
 * @self: (not nullable): the picker; used as the raw user_data of the handlers (it is the
 *   ancestor of the list view, see factory_setup()).
 * @store: (not nullable): the model of the list; referenced by the selection model.
 * @name: (not nullable): widget name of the list view.
 * @scrolled_out: (out) (not nullable): receives the scrolled window that wraps the view.
 *
 * Builds a list view with its factory, selection and scrolled window.
 *
 * Preconditions:
 *  - called from stk_picker_init() on the main thread; @self->bound_rows already exists (the
 *    factory callbacks use it).
 * Postconditions:
 *  - *@scrolled_out is a new floating widget containing the view; the factory (owned by the view)
 *    and selection model are owned by the view.
 * Thread affinity: main thread only.
 * Ownership: @store is referenced, not consumed; the caller owns the (floating) scrolled window
 *   and the returned view is a descendant of it.
 *
 * Returns: (transfer none): the list view (owned by the scrolled window).
 */
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

/**
 * make_entry:
 * @name: (not nullable): widget name of the entry.
 * @placeholder: (not nullable): placeholder text; copied by GTK.
 * @accessible_label: (not nullable): accessible label; copied by GTK.
 *
 * Creates a GtkEntry limited to STK_PICKER_MAX_QUERY_CHARS characters.
 *
 * Preconditions:
 *  - GTK is initialised; the strings are NUL-terminated; main thread.
 * Postconditions:
 *  - the entry has the name, the length limit, the placeholder and the accessible label.
 * Thread affinity: main thread only.
 * Ownership: the strings are only read (copied); the caller owns the new, floating widget.
 *
 * Returns: (transfer floating): the new entry; never NULL.
 */
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

/**
 * stk_picker_init:
 * @self: (not nullable): the new instance, zero-initialised by GObject.
 *
 * GObject instance initialiser: creates the row set and the two stores, then builds all widgets
 * and connects their handlers. The raw @self given to the handlers is safe: the widgets are
 * descendants of @self and are disposed with it; the toplevel-list handler is disconnected in
 * dispose.
 *
 * Preconditions:
 *  - called once by GObject while the instance is created (never directly); GTK is initialised
 *    with a display; main thread.
 * Postconditions:
 *  - bound_rows, the stores, all widgets, the key controller and the toplevel-list handler exist;
 *    catalog, watchlist and provider are still NULL (stk_picker_new() sets them).
 * Thread affinity: main thread only.
 * Ownership: @self is owned by GObject; the widgets are owned by the window, the stores,
 *   bound_rows and the toplevel-list reference by the picker.
 */
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

/**
 * compare_strings:
 * @a: (not nullable): pointer to a `const char *` slot (as g_ptr_array_sort() passes).
 * @b: (not nullable): pointer to a `const char *` slot.
 *
 * GCompareFunc ordering category strings in byte order.
 *
 * Preconditions:
 *  - both slots hold non-NULL NUL-terminated strings (stk_picker_new() only adds catalog
 *    categories whose first byte is not NUL).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread only (called from stk_picker_new()).
 * Ownership: the strings are only read.
 *
 * Returns: negative, 0 or positive as the first string sorts before, equal to or after the
 *   second in byte order.
 */
static int
compare_strings(gconstpointer a, gconstpointer b)
{
  return g_strcmp0(*(const char *const *)a, *(const char *const *)b);
}

/* Documented in stk-picker.h. */
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
  /* Only a capacity hint: clamp instead of truncating (the catalog is in-memory). */
  self->catalog_items =
      g_ptr_array_new_full((guint)MIN(n_entries, (gsize)G_MAXUINT), g_object_unref);
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
    if (previous == NULL || !g_str_equal(previous, category))
      gtk_string_list_append(category_list, category);
    previous = category;
  }
  gtk_drop_down_set_model(GTK_DROP_DOWN(self->category_dropdown), G_LIST_MODEL(category_list));
  g_object_unref(category_list);

  refilter(self);
  return GTK_WIDGET(self);
}
