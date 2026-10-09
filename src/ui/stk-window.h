/*
 * stk-window.h - StkWindow: the main window.
 *
 * Layout: a header bar with an "Add" button, and a paned view whose start side
 * is the watchlist and whose end side is the StkDetail pane.
 *
 *  - Watchlist: a virtualised GtkListView (only the visible rows have widgets)
 *    over StkApp's item model, one StkRow per item, with a single selection
 *    (the first row is selected automatically, and after a removal a
 *    neighbour is). While the watchlist is empty an empty-state page with an
 *    "Add" button replaces the list. Row actions (Remove, Move up, Move down)
 *    and drag-and-drop reordering edit the StkWatchlist; the window never
 *    saves it: StkApp reacts to the watchlist's "changed" signal (updates the
 *    list, the scheduler and the debounced save), so edits are shown at once
 *    whoever made them. A drop whose payload is not the symbol of a tracked
 *    instrument is ignored.
 *  - Detail pane: shows the selected item; its range switcher and the
 *    selection are forwarded to stk_app_select().
 *  - "Add" opens one StkPicker (transient for the window, destroyed with it,
 *    not modal) on the application's catalog, watchlist and provider; while it
 *    is open, "Add" presents it again instead of opening another. If the
 *    compiled-in catalog cannot be loaded the button is disabled.
 *  - Visibility for the refresh policy: the window reports to StkApp whether it
 *    is mapped and neither minimised nor suspended (GdkToplevel state), and
 *    reports when it becomes the active window. It also shows the app's
 *    "offline" state in the detail pane.
 *
 * Threading: main thread only. The window must be created after the
 * application's start-up load has finished (StkApp does that).
 *
 * Teardown: when the window is disposed (closed, or destroyed by the
 * application at shutdown) it first disconnects every handler it has on the
 * detail pane, the selection, the item model, the application and the
 * surface, so no signal of those objects emitted while the toolkit takes the
 * widgets apart reaches the window or leads to stk_app_select(). Its own
 * "map", "unmap" and "notify::is-active" handlers stay: they only report
 * visibility and focus, which StkApp ignores once it is shutting down.
 */
#ifndef STK_WINDOW_H
#define STK_WINDOW_H

#include <gtk/gtk.h>

#include "stk-app.h"

G_BEGIN_DECLS

#define STK_TYPE_WINDOW (stk_window_get_type())
G_DECLARE_FINAL_TYPE(StkWindow, stk_window, STK, WINDOW, GtkApplicationWindow)

/**
 * stk_window_new:
 * @app: the application (not NULL); the window is added to it.
 *
 * Returns: (transfer none): the new window, not shown yet; call
 *   gtk_window_present(). It is owned by the application / toplevel list
 *   until it is closed or destroyed.
 */
GtkWidget *stk_window_new(StkApp *app);

G_END_DECLS

#endif /* STK_WINDOW_H */
