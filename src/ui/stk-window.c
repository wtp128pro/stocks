/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/* stk-window.c - StkWindow: watchlist list view, detail pane, picker launcher. */
#include "stk-window.h"

#include "stk-detail.h"
#include "stk-picker.h"
#include "stk-row.h"

#define LIST_MIN_WIDTH 420
#define DETAIL_MIN_WIDTH 420

struct _StkWindow {
  GtkApplicationWindow parent_instance;

  StkApp *app; /* borrowed: the application outlives all its windows; never unreffed */
  GtkWidget *stack; /* "list" | "empty" */
  GtkWidget *detail;
  GtkWidget *add_buttons[2]; /* header bar, empty state */
  GtkWidget *picker;         /* weak pointer (g_object_add_weak_pointer): NULL once disposed */
  GtkSingleSelection *selection;
  StkRowItem *current; /* strong: the selected item, NULL for none */

  gboolean mapped;
  GdkToplevelState toplevel_state;
  gboolean reported_visible;
};

/*
 * G_DEFINE_FINAL_TYPE() below defines stk_window_get_type(), the static
 * stk_window_parent_class pointer and the static class initialiser that runs
 * stk_window_class_init() once; stk_window_init() is registered as the
 * instance initialiser. The instance struct is _StkWindow (StkWindow).
 */
G_DEFINE_FINAL_TYPE(StkWindow, stk_window, GTK_TYPE_APPLICATION_WINDOW)

/**
 * window_app:
 * @self: (not nullable): the window.
 *
 * Accessor for the borrowed application pointer.
 *
 * Preconditions:
 *  - @self is a StkWindow on which build_window() has run (it sets the pointer first); every
 *    caller is a handler that is connected inside or after build_window(). The application
 *    outlives its windows, so the pointer stays valid until the window is finalized, including
 *    while the window is being disposed.
 * Postconditions:
 *  - nothing is changed.
 * Thread affinity: main thread only.
 * Ownership: the application is not owned by the window; no reference is added.
 *
 * Returns: (transfer none): the application the window was built for.
 */
static StkApp *
window_app(StkWindow *self)
{
  return self->app;
}

/* ------------------------------------------------------------------------ */
/* Visibility and focus                                                     */
/* ------------------------------------------------------------------------ */

/**
 * report_visibility:
 * @self: (not nullable): the window.
 *
 * Reports to the application whether the window is mapped and neither minimised nor suspended;
 * only when the value differs from the last one reported.
 *
 * Preconditions:
 *  - @self is a built StkWindow (see window_app()).
 * Postconditions:
 *  - self->reported_visible equals the current visibility; stk_app_set_window_visible() was called
 *    iff it changed.
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides @self).
 */
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

/**
 * read_toplevel_state:
 * @self: (not nullable): the window.
 *
 * Reads the GdkToplevel state of the window's surface (0 when it has no surface or the surface is
 * not a toplevel) and reports the resulting visibility.
 *
 * Preconditions:
 *  - @self is a built StkWindow.
 * Postconditions:
 *  - self->toplevel_state is the current state; report_visibility() ran.
 * Thread affinity: main thread only.
 * Ownership: the surface is borrowed from the widget for the call.
 */
static void
read_toplevel_state(StkWindow *self)
{
  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(self));
  self->toplevel_state = (surface != NULL && GDK_IS_TOPLEVEL(surface))
                             ? gdk_toplevel_get_state(GDK_TOPLEVEL(surface))
                             : 0;
  report_visibility(self);
}

/**
 * on_surface_state:
 * @self: (not nullable): the window (the receiver of the swapped "notify::state" connection made
 *   by on_realize(); the GParamSpec and the surface the signal also passes are not used).
 *
 * Handler for state changes of the window's surface.
 *
 * Preconditions:
 *  - @self is a built StkWindow that is not finalized: the handler is connected with
 *    g_signal_connect_object(), and dispose disconnects it by data.
 * Postconditions:
 *  - the visibility was re-read and reported (read_toplevel_state()).
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides @self).
 */
static void
on_surface_state(StkWindow *self)
{
  read_toplevel_state(self);
}

/**
 * on_realize:
 * @widget: the window that was realized.
 * @user_data: unused (NULL).
 *
 * "realize" handler: starts listening to the state of the new surface. Each realization creates
 * a new surface, so the connection is made once per surface.
 *
 * Preconditions:
 *  - @widget is a StkWindow (the handler is only connected to the window itself).
 * Postconditions:
 *  - when the window has a surface, "notify::state" of that surface is connected to
 *    on_surface_state() with the window as the (weakly tracked) receiver; it is disconnected by
 *    stk_window_dispose() or when the surface or the window goes away.
 * Thread affinity: main thread only.
 * Ownership: @widget is borrowed; the surface is borrowed for the call.
 */
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

/**
 * on_map:
 * @widget: the window that was mapped.
 * @user_data: unused (NULL).
 *
 * "map" handler: marks the window mapped and reports the visibility.
 *
 * Preconditions:
 *  - @widget is a StkWindow. The handler stays connected until finalization; it only reports
 *    to the borrowed application, which outlives the window.
 * Postconditions:
 *  - self->mapped is TRUE and the state was re-read and reported.
 * Thread affinity: main thread only.
 * Ownership: @widget is borrowed.
 */
static void
on_map(GtkWidget *widget, gpointer user_data)
{
  (void)user_data;
  StkWindow *self = STK_WINDOW(widget);
  self->mapped = TRUE;
  read_toplevel_state(self);
}

/**
 * on_unmap:
 * @widget: the window that was unmapped.
 * @user_data: unused (NULL).
 *
 * "unmap" handler: marks the window not mapped and reports the visibility.
 *
 * Preconditions:
 *  - @widget is a StkWindow (see on_map()).
 * Postconditions:
 *  - self->mapped is FALSE and the visibility was reported.
 * Thread affinity: main thread only.
 * Ownership: @widget is borrowed.
 */
static void
on_unmap(GtkWidget *widget, gpointer user_data)
{
  (void)user_data;
  StkWindow *self = STK_WINDOW(widget);
  self->mapped = FALSE;
  report_visibility(self);
}

/**
 * on_active_changed:
 * @self: (not nullable): the window (the instance argument of the "notify::is-active"
 *   connection made in build_window(); the GParamSpec and user data (NULL) are not used).
 *
 * Tells the application when the window becomes the active window.
 *
 * Preconditions:
 *  - @self is a built StkWindow (the handler stays connected until finalization; the application
 *    it reports to outlives the window).
 * Postconditions:
 *  - stk_app_window_focused() was called iff the window is active; nothing else changed.
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides @self).
 */
static void
on_active_changed(StkWindow *self)
{
  if (gtk_window_is_active(GTK_WINDOW(self)))
    stk_app_window_focused(window_app(self));
}

/**
 * on_offline_changed:
 * @self: (not nullable): the window (receiver of the swapped "notify::offline" connection to the
 *   application; the GParamSpec and the application the signal also passes are not used).
 *
 * Copies the application's offline state to the detail pane. Also called once by build_window().
 *
 * Preconditions:
 *  - @self is a built StkWindow whose detail pane exists (self->detail is not NULL). The
 *    connection to the application is removed in stk_window_dispose(), which also clears
 *    self->detail, so it cannot fire afterwards.
 * Postconditions:
 *  - the detail pane's offline flag equals stk_app_get_offline().
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides @self).
 */
static void
on_offline_changed(StkWindow *self)
{
  stk_detail_set_offline(STK_DETAIL(self->detail), stk_app_get_offline(window_app(self)));
}

/* ------------------------------------------------------------------------ */
/* Picker                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * on_add_clicked:
 * @self: (not nullable): the window (receiver of the swapped "clicked" connections of both
 *   "Add" buttons; the button argument is not used).
 *
 * Opens the picker, or presents it again when it is open already. When the catalog cannot be
 * loaded both "Add" buttons are made insensitive and nothing is opened.
 *
 * Preconditions:
 *  - @self is a built StkWindow, not disposed (the handler is connected with
 *    g_signal_connect_object(), so it cannot run on a finalized window).
 * Postconditions:
 *  - at most one picker exists: self->picker is the open picker (a weak pointer, cleared when
 *    the picker is disposed; the picker is transient for the window and destroyed with it) or
 *    NULL if none could be created.
 * Thread affinity: main thread only.
 * Ownership: the picker is owned by GTK's window list, not by the window; the window holds only
 *   the weak pointer, which stk_window_dispose() removes.
 */
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

/**
 * select_current:
 * @self: (not nullable): the window.
 *
 * Tells the application which item and range the detail pane shows.
 *
 * Preconditions:
 *  - @self is a built StkWindow with a live detail pane (self->detail is not NULL); all callers are
 *    handlers that stk_window_dispose() disconnects before it clears self->detail.
 * Postconditions:
 *  - stk_app_select() was called with the current item's symbol (NULL for none) and the pane's
 *    range; the symbol is borrowed from the item for the call only.
 * Thread affinity: main thread only.
 * Ownership: the symbol string is owned by self->current (borrowed for the call).
 */
static void
select_current(StkWindow *self)
{
  StkDetail *detail = STK_DETAIL(self->detail);
  const char *symbol = self->current != NULL ? stk_row_item_get_symbol(self->current) : NULL;
  stk_app_select(window_app(self), symbol, stk_detail_get_range(detail));
}

/**
 * on_selected_item_changed:
 * @self: (not nullable): the window (receiver of the swapped "notify::selected-item" connection
 *   to the selection; the GParamSpec and the selection the signal also passes are not used).
 *
 * Follows the list's selection: stores the selected item (a strong reference), shows it in the
 * detail pane and reports it. Also called once by build_window().
 *
 * Preconditions:
 *  - @self is a built StkWindow with a selection and a live detail pane (the connection is removed
 *    in stk_window_dispose() before either is released).
 * Postconditions:
 *  - when the selected item differs from self->current: self->current holds a reference on the
 *    new item (or is NULL), the pane shows it and stk_app_select() was called; otherwise nothing
 *    is changed.
 * Thread affinity: main thread only.
 * Ownership: the item returned by the selection is borrowed; self->current takes its own
 *   reference.
 */
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

/**
 * on_range_changed:
 * @detail: the detail pane (unused).
 * @range: the range the user chose (unused: select_current() reads it from the pane).
 * @self: (not nullable): the window, user data of the connection made in build_window().
 *
 * "range-changed" handler of the detail pane: reports the new selection range.
 *
 * Preconditions:
 *  - @self is a built StkWindow whose detail pane is live (the connection is removed in
 *    stk_window_dispose() before the pointer is cleared).
 * Postconditions:
 *  - stk_app_select() was called as in select_current().
 * Thread affinity: main thread only.
 * Ownership: all parameters are borrowed for the call.
 */
static void
on_range_changed(StkDetail *detail, guint range, StkWindow *self)
{
  (void)detail;
  (void)range;
  select_current(self);
}

/**
 * on_items_changed:
 * @model: the item model of the application.
 * @position: first changed position (unused).
 * @removed: number of removed items (unused).
 * @added: number of added items (unused).
 * @self: (not nullable): the window, user data of the connection made in build_window().
 *
 * Shows the list page when the model has items and the empty-state page otherwise. Also called
 * once by build_window() with zeros.
 *
 * Preconditions:
 *  - @self is a built StkWindow whose stack exists; the connection is removed in
 *    stk_window_dispose().
 * Postconditions:
 *  - the stack shows "list" iff g_list_model_get_n_items(@model) > 0, else "empty".
 * Thread affinity: main thread only.
 * Ownership: all parameters are borrowed for the call.
 */
static void
on_items_changed(GListModel *model, guint position, guint removed, guint added, StkWindow *self)
{
  (void)position;
  (void)removed;
  (void)added;
  gtk_stack_set_visible_child_name(GTK_STACK(self->stack),
                                   g_list_model_get_n_items(model) > 0 ? "list" : "empty");
}

/**
 * on_row_remove:
 * @row: the row that asked (unused).
 * @symbol: (not nullable): the symbol of the row's item, a private copy made by the row.
 * @self: (not nullable): the window, user data of the connection made in on_factory_setup().
 *
 * "remove-requested" handler: removes the instrument from the watchlist. The application reacts
 * to the watchlist's "changed" signal.
 *
 * Preconditions:
 *  - @self is a built StkWindow. The connection is made with g_signal_connect_object() and is
 *    dropped when the window is finalized; rows only emit on user action.
 *  - @symbol is NUL-terminated. It is not trusted: stk_watchlist_remove() validates it
 *    (stk_symbol_is_valid()) and ignores anything that is not a tracked symbol.
 * Postconditions:
 *  - the entry is removed from the watchlist if it was present.
 * Thread affinity: main thread only.
 * Ownership: all parameters are borrowed for the call.
 */
static void
on_row_remove(StkRow *row, const char *symbol, StkWindow *self)
{
  (void)row;
  stk_watchlist_remove(stk_app_get_watchlist(window_app(self)), symbol);
}

/**
 * on_row_move:
 * @row: the row that asked (unused).
 * @symbol: (not nullable): the symbol of the row's item, a private copy made by the row.
 * @delta: -1 (Move up) or +1 (Move down); any other value is handled by the range check below.
 * @self: (not nullable): the window, user data of the connection made in on_factory_setup().
 *
 * "move-requested" handler: moves the instrument by @delta places in the watchlist.
 *
 * Preconditions:
 *  - @self is a built StkWindow (see on_row_remove()); @symbol is NUL-terminated.
 * Postconditions:
 *  - when stk_watchlist_index_of() finds @symbol (it validates the text) and index + @delta is
 *    inside [0, n_entries) (computed in gint64, so it cannot overflow) the entry was moved with
 *    stk_watchlist_move(); otherwise nothing is changed.
 * Thread affinity: main thread only.
 * Ownership: all parameters are borrowed for the call.
 */
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

/**
 * on_row_reorder:
 * @row: the row that received the drop (unused).
 * @source: (not nullable): UNTRUSTED text carried by a drop (it may come from another
 *   application); a private copy made by the row.
 * @target: (not nullable): the symbol of the row that received the drop.
 * @self: (not nullable): the window, user data of the connection made in on_factory_setup().
 *
 * "reorder-requested" handler: moves the dropped instrument to the target's position.
 *
 * Preconditions:
 *  - @self is a built StkWindow (see on_row_remove()); @source and @target are NUL-terminated.
 * Postconditions:
 *  - both texts go through stk_watchlist_index_of(), which looks at the first 16 bytes only and
 *    returns -1 for anything that is not a valid symbol; only when both are tracked symbols was
 *    the entry moved with stk_watchlist_move() (indices come from the watchlist and are >= 0).
 *    Otherwise nothing is changed. Neither text is stored.
 * Thread affinity: main thread only.
 * Ownership: all parameters are borrowed for the call.
 */
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

/**
 * on_factory_setup:
 * @factory: the list item factory (unused).
 * @list_item: the list item that needs a child.
 * @user_data: (not nullable): the StkWindow, passed as user data by g_signal_connect_object() in
 *   build_list_page(); the connection is dropped when the window is finalized.
 *
 * "setup" handler of the factory: creates a StkRow, connects its request signals to the window
 * and makes it the list item's child.
 *
 * Preconditions:
 *  - @list_item is a GtkListItem; @user_data is the StkWindow (the row handlers use it as the
 *    object of g_signal_connect_object(), so they never outlive it).
 * Postconditions:
 *  - @list_item's child is a new unbound StkRow.
 * Thread affinity: main thread only.
 * Ownership: the row is owned by @list_item (the floating reference is sunk by
 *   gtk_list_item_set_child()); @list_item and @user_data are borrowed.
 */
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

/**
 * on_factory_bind:
 * @factory: the list item factory (unused).
 * @list_item: the list item being bound.
 * @user_data: unused (NULL).
 *
 * "bind" handler of the factory: shows the list item's StkRowItem in its row. Pairs with
 * on_factory_unbind().
 *
 * Preconditions:
 *  - @list_item was set up by on_factory_setup() (its child is a StkRow) and holds a StkRowItem
 *    of the application's model.
 * Postconditions:
 *  - the row holds a reference on the item and follows its "changed" signal until unbound.
 * Thread affinity: main thread only.
 * Ownership: all parameters are borrowed; the row takes its own reference on the item.
 */
static void
on_factory_bind(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  (void)factory;
  (void)user_data;
  stk_row_bind(STK_ROW(gtk_list_item_get_child(list_item)),
               STK_ROW_ITEM(gtk_list_item_get_item(list_item)));
}

/**
 * on_factory_unbind:
 * @factory: the list item factory (unused).
 * @list_item: the list item being unbound.
 * @user_data: unused (NULL).
 *
 * "unbind" handler of the factory: blanks the row and releases its item. Pairs with
 * on_factory_bind().
 *
 * Preconditions:
 *  - @list_item was set up by on_factory_setup() (its child is a StkRow).
 * Postconditions:
 *  - the row is unbound: its reference on the item and its "changed" handler are gone.
 * Thread affinity: main thread only.
 * Ownership: all parameters are borrowed for the call.
 */
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

/**
 * build_list_page:
 * @self: (not nullable): the window being built.
 * @app: (not nullable): the application.
 *
 * Builds the scrolled list view over the application's item model with a single, auto-selecting
 * selection and a signal factory (setup, bind, unbind).
 *
 * Preconditions:
 *  - called once by build_window() with @app == self->app, on a window whose selection is NULL.
 * Postconditions:
 *  - self->selection holds a reference on the new selection model; the list view holds its own
 *    reference on the selection and the selection on the item model; the factory is owned by
 *    the list view. The "setup" connection uses the window as its object; "bind"/"unbind" have
 *    no user data.
 * Thread affinity: main thread only.
 * Ownership: the model reference taken for the selection and the selection reference given to
 *   the list view are transferred; the returned widget is floating.
 *
 * Returns: (transfer floating): the scrolled window holding the list view.
 */
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

/**
 * build_empty_page:
 * @self: (not nullable): the window being built.
 *
 * Builds the empty-state page ("No stocks yet", a hint and an "Add" button).
 *
 * Preconditions:
 *  - called once by build_window().
 * Postconditions:
 *  - self->add_buttons[1] is the new button; its "clicked" signal calls on_add_clicked() with
 *    the window as the (weakly tracked) receiver.
 * Thread affinity: main thread only.
 * Ownership: the returned widget is floating; the button pointer in @self is borrowed (the page
 *   owns the button).
 *
 * Returns: (transfer floating): the page widget.
 */
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

/**
 * build_window:
 * @self: (not nullable): the new window, straight from g_object_new().
 * @app: (not nullable): the application; borrowed, it must outlive the window.
 *
 * The application is not a construct property of GtkWindow, so the window is built here, after
 * g_object_new(), and added to the application last (by stk_window_new()). Creates the header
 * bar, the list and empty pages, the detail pane and the paned layout, connects the handlers and
 * applies the current items, offline state and selection.
 *
 * Preconditions:
 *  - called exactly once per window, by stk_window_new(), after STK_IS_APP(@app) was checked,
 *    on the main thread.
 * Postconditions:
 *  - self->app is @app (no reference taken); stack, detail, selection and add_buttons are set.
 *    The handlers on the application, the item model, the detail pane and the selection are
 *    connected with the window as the object (dropped at finalization) and are also disconnected
 *    by stk_window_dispose(); so are the surface handlers made later by on_realize(). The
 *    handlers on the two "Add" buttons and (through on_factory_setup()) on the rows are
 *    connected the same way and live until finalization. The "realize", "map", "unmap" and
 *    "notify::is-active" handlers are plain connections without user data on the window itself
 *    and stay (they only report to the application).
 * Thread affinity: main thread only.
 * Ownership: @app is borrowed; children are owned by their containers.
 */
static void
build_window(StkWindow *self, StkApp *app)
{
  self->app = app;

  gtk_window_set_title(GTK_WINDOW(self), "Stocks");
  gtk_window_set_default_size(GTK_WINDOW(self), 1040, 640);

  GtkWidget *header = gtk_header_bar_new();
  /* "list-add-symbolic" is a freedesktop standard name every icon theme provides or inherits
   * (GTK also ships Adwaita's copy as a fallback); an icon-only button needs an explicit
   * accessible label. */
  GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic");
  gtk_widget_set_tooltip_text(add, "Add a stock, ETF or fund");
  gtk_accessible_update_property(GTK_ACCESSIBLE(add), GTK_ACCESSIBLE_PROPERTY_LABEL, "Add", -1);
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

/**
 * stk_window_dispose:
 * @object: the window being disposed.
 *
 * GObject dispose vfunc. Disconnects every handler that has this window as its receiver (on the
 * application, the item model, the detail pane, the selection and the surface), so that nothing
 * the toolkit emits while it takes the widgets apart reaches the window; clears the detail
 * pointer, removes the weak pointer on the picker, releases the current item and the selection,
 * then chains up. May run more than once.
 *
 * Preconditions:
 *  - @object is a StkWindow; self->app is NULL (never built) or the live application: the
 *    application is borrowed and outlives the window, so using it here is safe.
 * Postconditions:
 *  - self->detail, self->picker, self->current and self->selection are NULL; the parent dispose
 *    has run. A repeated call skips the parts already done (the application and surface
 *    disconnections are repeated and harmless).
 * Thread affinity: main thread only.
 * Ownership: the references on the current item and the selection, and the picker weak pointer,
 *   are released; the application is not owned.
 */
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

/**
 * stk_window_class_init:
 * @klass: (not nullable): the class structure being initialised.
 *
 * GObject class initialiser: installs dispose().
 *
 * Preconditions:
 *  - called once by GObject while the type is registered.
 * Postconditions:
 *  - the class dispose vfunc is stk_window_dispose().
 * Thread affinity: main thread only.
 * Ownership: @klass is owned by GObject.
 */
static void
stk_window_class_init(StkWindowClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = stk_window_dispose;
}

/**
 * stk_window_init:
 * @self: (not nullable): the new instance (zero-filled by GObject).
 *
 * GObject instance initialiser: nothing to do, the window is built by build_window() because
 * the application is not known yet.
 *
 * Preconditions:
 *  - called once by GObject for a new instance.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread only.
 * Ownership: @self is owned by GObject.
 */
static void
stk_window_init(StkWindow *self)
{
  (void)self;
}

/* Documented in stk-window.h. */
GtkWidget *
stk_window_new(StkApp *app)
{
  g_return_val_if_fail(STK_IS_APP(app), NULL);
  StkWindow *self = g_object_new(STK_TYPE_WINDOW, NULL);
  build_window(self, app);
  gtk_window_set_application(GTK_WINDOW(self), GTK_APPLICATION(app));
  return GTK_WIDGET(self);
}
