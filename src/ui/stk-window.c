/* stk-window.c - StkWindow: watchlist list view, detail pane, picker launcher. */
#include "stk-window.h"

#include "stk-detail.h"
#include "stk-picker.h"
#include "stk-row.h"

#define LIST_MIN_WIDTH 420
#define DETAIL_MIN_WIDTH 420

struct _StkWindow {
  GtkApplicationWindow parent_instance;

  StkApp *app; /* borrowed: the application outlives all its windows */
  GtkWidget *stack; /* "list" | "empty" */
  GtkWidget *detail;
  GtkWidget *add_buttons[2]; /* header bar, empty state */
  GtkWidget *picker;         /* weak: NULL once destroyed */
  GtkSingleSelection *selection;
  StkRowItem *current; /* strong: the selected item, NULL for none */

  gboolean mapped;
  GdkToplevelState toplevel_state;
  gboolean reported_visible;
};

G_DEFINE_FINAL_TYPE(StkWindow, stk_window, GTK_TYPE_APPLICATION_WINDOW)

static StkApp *
window_app(StkWindow *self)
{
  return self->app;
}

/* ------------------------------------------------------------------------ */
/* Visibility and focus                                                     */
/* ------------------------------------------------------------------------ */

static void
report_visibility(StkWindow *self)
{
  gboolean visible = self->mapped && (self->toplevel_state & (GDK_TOPLEVEL_STATE_MINIMIZED |
                                                              GDK_TOPLEVEL_STATE_SUSPENDED)) == 0;
  if (visible == self->reported_visible)
    return;
  self->reported_visible = visible;
  stk_app_set_window_visible(window_app(self), visible);
}

static void
read_toplevel_state(StkWindow *self)
{
  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(self));
  self->toplevel_state = (surface != NULL && GDK_IS_TOPLEVEL(surface))
                             ? gdk_toplevel_get_state(GDK_TOPLEVEL(surface))
                             : 0;
  report_visibility(self);
}

static void
on_surface_state(StkWindow *self)
{
  read_toplevel_state(self);
}

static void
on_realize(GtkWidget *widget, gpointer user_data)
{
  (void)user_data;
  StkWindow *self = STK_WINDOW(widget);
  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(self));
  if (surface != NULL)
    g_signal_connect_object(surface, "notify::state", G_CALLBACK(on_surface_state), self,
                            G_CONNECT_SWAPPED);
}

static void
on_map(GtkWidget *widget, gpointer user_data)
{
  (void)user_data;
  StkWindow *self = STK_WINDOW(widget);
  self->mapped = TRUE;
  read_toplevel_state(self);
}

static void
on_unmap(GtkWidget *widget, gpointer user_data)
{
  (void)user_data;
  StkWindow *self = STK_WINDOW(widget);
  self->mapped = FALSE;
  report_visibility(self);
}

static void
on_active_changed(StkWindow *self)
{
  if (gtk_window_is_active(GTK_WINDOW(self)))
    stk_app_window_focused(window_app(self));
}

static void
on_offline_changed(StkWindow *self)
{
  stk_detail_set_offline(STK_DETAIL(self->detail), stk_app_get_offline(window_app(self)));
}

/* ------------------------------------------------------------------------ */
/* Picker                                                                   */
/* ------------------------------------------------------------------------ */

static void
on_add_clicked(StkWindow *self)
{
  if (self->picker != NULL) {
    gtk_window_present(GTK_WINDOW(self->picker));
    return;
  }
  StkApp *app = window_app(self);
  const StkCatalog *catalog = stk_app_get_catalog(app);
  if (catalog == NULL) { /* the compiled-in resource is unusable: nothing to pick from */
    for (guint i = 0; i < G_N_ELEMENTS(self->add_buttons); i++)
      gtk_widget_set_sensitive(self->add_buttons[i], FALSE);
    return;
  }
  GtkWidget *picker =
      stk_picker_new(catalog, stk_app_get_watchlist(app), stk_app_get_provider(app));
  if (picker == NULL)
    return;
  gtk_window_set_transient_for(GTK_WINDOW(picker), GTK_WINDOW(self));
  gtk_window_set_destroy_with_parent(GTK_WINDOW(picker), TRUE);
  self->picker = picker;
  g_object_add_weak_pointer(G_OBJECT(picker), (gpointer *)&self->picker);
  gtk_window_present(GTK_WINDOW(picker));
}

/* ------------------------------------------------------------------------ */
/* Selection, detail, rows                                                  */
/* ------------------------------------------------------------------------ */

static void
select_current(StkWindow *self)
{
  StkDetail *detail = STK_DETAIL(self->detail);
  const char *symbol = self->current != NULL ? stk_row_item_get_symbol(self->current) : NULL;
  stk_app_select(window_app(self), symbol, stk_detail_get_range(detail));
}

static void
on_selected_item_changed(StkWindow *self)
{
  StkRowItem *item = gtk_single_selection_get_selected_item(self->selection);
  if (item == self->current)
    return;
  g_set_object(&self->current, item);
  stk_detail_set_item(STK_DETAIL(self->detail), item);
  select_current(self);
}

static void
on_range_changed(StkDetail *detail, guint range, StkWindow *self)
{
  (void)detail;
  (void)range;
  select_current(self);
}

static void
on_items_changed(GListModel *model, guint position, guint removed, guint added, StkWindow *self)
{
  (void)position;
  (void)removed;
  (void)added;
  gtk_stack_set_visible_child_name(GTK_STACK(self->stack),
                                   g_list_model_get_n_items(model) > 0 ? "list" : "empty");
}

static void
on_row_remove(StkRow *row, const char *symbol, StkWindow *self)
{
  (void)row;
  stk_watchlist_remove(stk_app_get_watchlist(window_app(self)), symbol);
}

static void
on_row_move(StkRow *row, const char *symbol, int delta, StkWindow *self)
{
  (void)row;
  StkWatchlist *wl = stk_app_get_watchlist(window_app(self));
  gint from = stk_watchlist_index_of(wl, symbol);
  if (from < 0)
    return;
  gint64 to = (gint64)from + delta;
  if (to < 0 || to >= (gint64)stk_watchlist_get_n_entries(wl))
    return;
  stk_watchlist_move(wl, (guint)from, (guint)to);
}

static void
on_row_reorder(StkRow *row, const char *source, const char *target, StkWindow *self)
{
  (void)row;
  StkWatchlist *wl = stk_app_get_watchlist(window_app(self));
  gint from = stk_watchlist_index_of(wl, source); /* validates untrusted text */
  gint to = stk_watchlist_index_of(wl, target);
  if (from >= 0 && to >= 0)
    stk_watchlist_move(wl, (guint)from, (guint)to);
}

static void
on_factory_setup(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  (void)factory;
  GtkWidget *row = stk_row_new();
  g_signal_connect_object(row, "remove-requested", G_CALLBACK(on_row_remove), user_data, 0);
  g_signal_connect_object(row, "move-requested", G_CALLBACK(on_row_move), user_data, 0);
  g_signal_connect_object(row, "reorder-requested", G_CALLBACK(on_row_reorder), user_data, 0);
  gtk_list_item_set_child(list_item, row);
}

static void
on_factory_bind(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  (void)factory;
  (void)user_data;
  stk_row_bind(STK_ROW(gtk_list_item_get_child(list_item)),
               STK_ROW_ITEM(gtk_list_item_get_item(list_item)));
}

static void
on_factory_unbind(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  (void)factory;
  (void)user_data;
  stk_row_bind(STK_ROW(gtk_list_item_get_child(list_item)), NULL);
}

/* ------------------------------------------------------------------------ */
/* Construction                                                             */
/* ------------------------------------------------------------------------ */

static GtkWidget *
build_list_page(StkWindow *self, StkApp *app)
{
  GListModel *items = stk_app_get_items(app);
  self->selection = gtk_single_selection_new(g_object_ref(items));
  gtk_single_selection_set_autoselect(self->selection, TRUE);
  gtk_single_selection_set_can_unselect(self->selection, FALSE);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect_object(factory, "setup", G_CALLBACK(on_factory_setup), self, 0);
  g_signal_connect(factory, "bind", G_CALLBACK(on_factory_bind), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(on_factory_unbind), NULL);
  GtkWidget *list = gtk_list_view_new(g_object_ref(GTK_SELECTION_MODEL(self->selection)), factory);
  gtk_list_view_set_show_separators(GTK_LIST_VIEW(list), TRUE);
  gtk_widget_add_css_class(list, "stk-list");

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);
  gtk_widget_set_size_request(scrolled, LIST_MIN_WIDTH, -1);
  return scrolled;
}

static GtkWidget *
build_empty_page(StkWindow *self)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(box, LIST_MIN_WIDTH, -1);

  GtkWidget *title = gtk_label_new("No stocks yet");
  gtk_widget_add_css_class(title, "stk-empty-title");
  GtkWidget *hint = gtk_label_new("Add a stock, an ETF or a Fidelity fund to follow it.");
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_label_set_justify(GTK_LABEL(hint), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class(hint, "dim-label");
  GtkWidget *button = gtk_button_new_with_label("Add");
  gtk_widget_add_css_class(button, "suggested-action");
  gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
  self->add_buttons[1] = button;
  g_signal_connect_object(button, "clicked", G_CALLBACK(on_add_clicked), self, G_CONNECT_SWAPPED);

  gtk_box_append(GTK_BOX(box), title);
  gtk_box_append(GTK_BOX(box), hint);
  gtk_box_append(GTK_BOX(box), button);
  return box;
}

/* The application is not a construct property of GtkWindow, so the window is
 * built here, after g_object_new(), and added to the application last. */
static void
build_window(StkWindow *self, StkApp *app)
{
  self->app = app;

  gtk_window_set_title(GTK_WINDOW(self), "Stocks");
  gtk_window_set_default_size(GTK_WINDOW(self), 1040, 640);

  GtkWidget *header = gtk_header_bar_new();
  GtkWidget *add = gtk_button_new_with_label("Add");
  gtk_widget_set_tooltip_text(add, "Add a stock, ETF or fund");
  self->add_buttons[0] = add;
  g_signal_connect_object(add, "clicked", G_CALLBACK(on_add_clicked), self, G_CONNECT_SWAPPED);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), add);
  gtk_window_set_titlebar(GTK_WINDOW(self), header);

  self->stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(self->stack), build_list_page(self, app), "list");
  gtk_stack_add_named(GTK_STACK(self->stack), build_empty_page(self), "empty");
  GListModel *items = stk_app_get_items(app);
  g_signal_connect_object(items, "items-changed", G_CALLBACK(on_items_changed), self, 0);
  on_items_changed(items, 0, 0, 0, self);

  self->detail = stk_detail_new();
  gtk_widget_set_size_request(self->detail, DETAIL_MIN_WIDTH, -1);
  g_signal_connect_object(self->detail, "range-changed", G_CALLBACK(on_range_changed), self, 0);
  g_signal_connect_object(app, "notify::offline", G_CALLBACK(on_offline_changed), self,
                          G_CONNECT_SWAPPED);
  on_offline_changed(self);

  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_start_child(GTK_PANED(paned), self->stack);
  gtk_paned_set_end_child(GTK_PANED(paned), self->detail);
  gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_position(GTK_PANED(paned), LIST_MIN_WIDTH);
  gtk_window_set_child(GTK_WINDOW(self), paned);

  g_signal_connect_object(self->selection, "notify::selected-item",
                          G_CALLBACK(on_selected_item_changed), self, G_CONNECT_SWAPPED);
  on_selected_item_changed(self); /* the first row is already selected */

  g_signal_connect(self, "realize", G_CALLBACK(on_realize), NULL);
  g_signal_connect(self, "map", G_CALLBACK(on_map), NULL);
  g_signal_connect(self, "unmap", G_CALLBACK(on_unmap), NULL);
  g_signal_connect(self, "notify::is-active", G_CALLBACK(on_active_changed), NULL);
}

static void
stk_window_dispose(GObject *object)
{
  StkWindow *self = STK_WINDOW(object);
  /* Every handler that has this window as its receiver is disconnected
   * before the toolkit starts to take the widgets apart: while it does,
   * children (the detail pane, the list view and its selection) and the
   * surface may still emit signals, and none of them may reach a window that
   * is being disposed. Dispose may run more than once: pointers to children
   * are cleared after their use (the application is borrowed and outlives
   * the window). */
  if (self->app != NULL) {
    g_signal_handlers_disconnect_by_data(self->app, self);
    g_signal_handlers_disconnect_by_data(stk_app_get_items(self->app), self);
  }
  if (self->detail != NULL) {
    g_signal_handlers_disconnect_by_data(self->detail, self);
    self->detail = NULL;
  }
  if (self->selection != NULL)
    g_signal_handlers_disconnect_by_data(self->selection, self);
  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(self));
  if (surface != NULL)
    g_signal_handlers_disconnect_by_data(surface, self);

  if (self->picker != NULL) {
    g_object_remove_weak_pointer(G_OBJECT(self->picker), (gpointer *)&self->picker);
    self->picker = NULL;
  }
  g_clear_object(&self->current);
  g_clear_object(&self->selection);
  G_OBJECT_CLASS(stk_window_parent_class)->dispose(object);
}

static void
stk_window_class_init(StkWindowClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = stk_window_dispose;
}

static void
stk_window_init(StkWindow *self)
{
  (void)self;
}

GtkWidget *
stk_window_new(StkApp *app)
{
  g_return_val_if_fail(STK_IS_APP(app), NULL);
  StkWindow *self = g_object_new(STK_TYPE_WINDOW, NULL);
  build_window(self, app);
  gtk_window_set_application(GTK_WINDOW(self), GTK_APPLICATION(app));
  return GTK_WIDGET(self);
}
