/*
 * stk-picker.h - StkPicker: window to browse the Fidelity catalog, search
 * symbols online and add/remove instruments of the watchlist.
 *
 * Layout (top to bottom): a catalog filter entry, a kind drop-down (all /
 * ETFs / mutual funds) and a category drop-down; a symbol search entry; an
 * inline status line; the search results list (only visible while there are
 * results); the catalog list.
 *
 *  - Catalog list: a virtualised GtkListView (only the visible rows have
 *    widgets) over the entries of the StkCatalog that pass the three filters.
 *    The kind filter and the substring filter are applied by
 *    stk_catalog_filter(); the category filter keeps only entries whose
 *    category equals the selected one exactly (the drop-down lists every
 *    non-empty category of the catalog once, in strcmp order). The filter
 *    entry is applied on every edit (no debounce, no I/O); surrounding ASCII
 *    whitespace of its text is ignored. It accepts at most
 *    STK_PICKER_MAX_QUERY_CHARS characters.
 *  - Search: the symbol search entry (at most STK_PICKER_MAX_QUERY_CHARS
 *    characters) is debounced by STK_PICKER_SEARCH_DELAY_MS: every edit
 *    cancels the in-flight search and restarts the timer; when it fires, the
 *    text (surrounding ASCII whitespace removed) is passed to
 *    stk_provider_search_async() with a fresh GCancellable. Text that is
 *    empty after trimming issues no request and clears the results. The text
 *    is not validated here: whatever the provider rejects (for example more
 *    than 64 bytes, STK_ERROR_INVALID_ARG) is shown as an inline error. At
 *    most one search is in flight. A completion is applied only if it
 *    belongs to the latest request, even when the provider ignored the
 *    cancellation; a cancelled completion is silent. On failure the result
 *    list is cleared and "Search failed: <message>" is shown with the CSS
 *    class "error" (the message is made valid UTF-8, control characters
 *    become spaces, and it is cut to STK_PICKER_MAX_MESSAGE_BYTES). On
 *    success at most STK_PICKER_MAX_RESULTS hits are listed, in provider
 *    order; hits with an invalid symbol or kind are skipped; an empty
 *    result shows "No results".
 *  - Every row shows a check button (checked == the symbol is in the
 *    watchlist), the symbol, the name, the category (catalog rows only) and
 *    a kind label ("Stock", "ETF" or "Fund"; for search results the kind the
 *    provider reported). Toggling the check button, or activating the row
 *    (Enter / double click), adds the instrument with that kind and its name
 *    (made valid UTF-8 and cut to STK_WATCHLIST_MAX_NAME_BYTES), or removes
 *    it. All rows, in both lists, follow the watchlist: a change made
 *    elsewhere (the watchlist's "changed" signal) is reflected in the rows
 *    at once. If an add is refused (the list is full, STK_WATCHLIST_MAX_ENTRIES)
 *    the check button is reverted and the status line shows the reason; the
 *    reason is removed by the next add or remove made here that succeeds (or
 *    by anything else that rewrites the status line, such as the search).
 *  - The picker never saves: it mutates the watchlist only through
 *    stk_watchlist_add()/stk_watchlist_remove() and leaves persistence
 *    (stk_watchlist_save_async(), debouncing) to the owner, which learns
 *    about edits made here from "watchlist-changed".
 *
 * Keyboard: everything is reachable with Tab. In each list Tab visits one row
 * (the row, then its check button) and then leaves the list; the arrow keys
 * move between rows. Space toggles a focused check button, Enter toggles a
 * focused row, and Escape closes the window from any widget of the window
 * (an open drop-down list takes the Escape itself and only closes the list).
 * Check buttons carry an accessible label "<symbol>, <name>".
 *
 * Closing: Escape, the window's close button and gtk_window_close() emit
 * "close-request". Closing as such cancels nothing, because it does not mean
 * the window goes away: a handler may veto it, and gtk_window_set_hide_on_close()
 * only hides the window. The debounce timer and the in-flight search then keep
 * running and a re-presented picker shows what the query still in its entry
 * produced. They are cancelled (silently, with the status line cleared) when
 * the picker is destroyed: at gtk_window_destroy(), which the default close
 * handler calls, even if the owner still holds a reference of its own, and at
 * dispose. A destroyed picker issues no search, accepts no edit (a refused
 * check button is reverted) and emits nothing; an owner that keeps a reference
 * to it must not reuse it. Late completions of the provider are dropped
 * without touching the picker.
 *
 * Threading: main thread only. Nothing blocks: the provider is asynchronous,
 * the catalog and watchlist calls are in-memory, and the only timer is the
 * debounce timer (g_timeout_add() on the default main context).
 *
 * CSS: node name "window", style class "stk-picker"; the inline message is
 * the label named "stk-picker-status" (class "error" for errors); the other
 * named widgets are "stk-picker-filter-entry", "stk-picker-kind",
 * "stk-picker-category", "stk-picker-search-entry", "stk-picker-results" and
 * "stk-picker-catalog" (the list views), "stk-picker-toggle",
 * "stk-picker-symbol", "stk-picker-name", "stk-picker-category-label" and
 * "stk-picker-kind-label" (row widgets).
 */
#ifndef STK_PICKER_H
#define STK_PICKER_H

#include <gtk/gtk.h>

#include "stk-catalog.h"
#include "stk-provider.h"
#include "stk-watchlist.h"

G_BEGIN_DECLS

/** Quiet time after the last edit of the search entry before a search starts. */
#define STK_PICKER_SEARCH_DELAY_MS 300
/** Maximum number of characters of either entry. */
#define STK_PICKER_MAX_QUERY_CHARS 64
/** Maximum number of search hits listed. */
#define STK_PICKER_MAX_RESULTS 100
/** Maximum byte length of a provider error message shown inline. */
#define STK_PICKER_MAX_MESSAGE_BYTES 200

#define STK_TYPE_PICKER (stk_picker_get_type())
G_DECLARE_FINAL_TYPE(StkPicker, stk_picker, STK, PICKER, GtkWindow)

/**
 * StkPicker::watchlist-changed:
 * @self: the picker.
 *
 * Emitted (synchronously, once per change) after the picker itself added an
 * instrument to or removed one from the watchlist (check button or row
 * activation). It is NOT emitted for changes made by anyone else, for refused
 * edits, or once the picker is destroyed. The picker is kept alive for the
 * whole edit: a handler of the watchlist's own "changed" signal may destroy it
 * while it edits; the edit then stands but nothing is emitted. Handlers of this
 * signal may read the watchlist and may close or destroy the picker; they MUST
 * NOT mutate the watchlist re-entrantly. The picker does not save the
 * watchlist; a handler typically schedules stk_watchlist_save_async().
 */

/**
 * stk_picker_new:
 * @catalog: non-NULL catalog. BORROWED: it is not copied or reference
 *   counted, so it MUST stay valid until the picker is disposed (destroyed
 *   and unreferenced). The picker only reads it.
 * @watchlist: non-NULL watchlist; a reference is taken and released on
 *   dispose. Main-thread only; the picker edits it through the watchlist API.
 * @provider: non-NULL StkProvider; a reference is taken and released on
 *   dispose. Only search_async()/search_finish() are used.
 *
 * Creates the picker window (not shown; call gtk_window_present(), and
 * gtk_window_set_transient_for()/set_modal() if wanted). Like gtk_window_new()
 * the window is owned by GTK's list of toplevels until gtk_window_destroy();
 * the caller does not own a reference and must not unref it. Needs an
 * initialised GTK with a display. Must be called from the main thread.
 *
 * Returns: (transfer none): the new window, or NULL (with a critical) if
 *   an argument is NULL or of the wrong type.
 */
GtkWidget *stk_picker_new(const StkCatalog *catalog, StkWatchlist *watchlist,
                          StkProvider *provider);

G_END_DECLS

#endif /* STK_PICKER_H */
