/*
 * stk-detail.h - StkDetail: the detail pane of the main window.
 *
 * Shows one StkRowItem: symbol and name, the big price, the change (absolute
 * and percent), a caption with the as-of date, the range switcher (1D 1M 3M 6M
 * 1Y), the StkChart of the item's detail series and a badge that reads
 * "Offline" or "Stale". For an item of kind STK_KIND_FUND the 1D button is
 * hidden, a range of 1D is replaced by 1M, and the caption reads "NAV" (with
 * the date of the NAV when the quote has one): funds publish a net asset value
 * once a day. Without an item (stk_detail_set_item() with NULL) the pane
 * shows a "Select a symbol" placeholder.
 *
 * The pane only reads the item and re-renders exactly the parts whose data the
 * item reports as changed: its "changed" signal refreshes the texts, the badge
 * and the chart's stale marker; its "detail-changed" signal refreshes the
 * chart. A missing number is shown as an em dash, a missing series as the
 * chart's "No data". The chart shows the item's detail series only while the
 * item's detail range equals the selected range.
 *
 * "Stale": the item's stale flag or the offline flag (set by the owner).
 *
 * Threading: main thread only. The widget has the CSS class "stk-detail"; the
 * labels use "stk-detail-symbol", "stk-detail-name", "stk-detail-price",
 * "stk-change" (plus "positive"/"negative"/"flat"), "stk-detail-caption" and
 * "stk-badge"; the range buttons have the class "stk-range".
 *
 * Teardown: when the pane is disposed its range buttons lose their handlers
 * (and the item its signals) before the toolkit destroys them, so taking the
 * pane apart never emits "range-changed".
 */
#ifndef STK_DETAIL_H
#define STK_DETAIL_H

#include <gtk/gtk.h>

#include "stk-row.h"
#include "stk-types.h"

G_BEGIN_DECLS

#define STK_TYPE_DETAIL (stk_detail_get_type())
G_DECLARE_FINAL_TYPE(StkDetail, stk_detail, STK, DETAIL, GtkBox)

/**
 * StkDetail::range-changed:
 * @self: the pane.
 * @range: the StkRange the user chose.
 *
 * Emitted when the user picks another range with the switcher. NOT emitted
 * when stk_detail_set_item() changes the range itself (1D -> 1M for a fund):
 * read it with stk_detail_get_range().
 */

/**
 * stk_detail_new:
 *
 * Returns: (transfer floating): a pane without item (placeholder shown), range
 *   1D, not offline.
 */
GtkWidget *stk_detail_new(void);

/**
 * stk_detail_set_item:
 * @self: the pane.
 * @item: (nullable): the item to show, or NULL for the placeholder. A
 *   reference is held until another item is set or the pane is disposed.
 *
 * Renders @item completely and follows its signals. A fund item at range 1D
 * switches the range to 1M (silently, see range-changed).
 */
void stk_detail_set_item(StkDetail *self, StkRowItem *item);

/**
 * stk_detail_get_range:
 * @self: the pane.
 *
 * Returns: the selected range (always valid for the current item's kind).
 */
StkRange stk_detail_get_range(StkDetail *self);

/**
 * stk_detail_set_offline:
 * @self: the pane.
 * @offline: whether the network is unavailable.
 *
 * Shows or hides the "Offline" badge (it takes precedence over "Stale") and
 * marks the chart stale while offline. Does nothing when unchanged.
 */
void stk_detail_set_offline(StkDetail *self, gboolean offline);

G_END_DECLS

#endif /* STK_DETAIL_H */
