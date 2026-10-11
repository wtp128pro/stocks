/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
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

/*
 * G_DECLARE_FINAL_TYPE() below declares the types StkDetail and
 * StkDetailClass, stk_detail_get_type(), the inline cast function STK_DETAIL()
 * and the inline type-check function STK_IS_DETAIL(), and the g_autoptr()
 * cleanup for StkDetail. STK_TYPE_DETAIL is defined here. The type itself is
 * defined by G_DEFINE_FINAL_TYPE() in stk-detail.c.
 */
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
 * Preconditions:
 *  - GTK is initialised (a display is available) and the call is made on the
 *    main thread.
 * Postconditions:
 *  - the pane shows the placeholder, has no item, range STK_RANGE_1D and is
 *    not offline.
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides the returned widget, which is floating).
 *
 * Returns: (transfer floating): a pane without item (placeholder shown), range
 *   1D, not offline.
 */
GtkWidget *stk_detail_new(void);

/**
 * stk_detail_set_item:
 * @self: (not nullable): the pane; must satisfy STK_IS_DETAIL().
 * @item: (nullable): the item to show, or NULL for the placeholder. A
 *   reference is held until another item is set or the pane is disposed.
 *
 * Renders @item completely and follows its signals. The pane shows the
 * item's view range (stk_row_item_get_view_range()) and stores a range the
 * user picks there (stk_row_item_set_view_range()), so every item keeps its
 * own range. A fund item whose range is 1D shows 1M instead (silently, see
 * range-changed; the stored range is not rewritten).
 *
 * Preconditions:
 *  - @self satisfies STK_IS_DETAIL() and is not disposed (dispose clears the
 *    range buttons); @item is NULL or satisfies STK_IS_ROW_ITEM() (else:
 *    critical, nothing changed).
 * Postconditions:
 *  - nothing happens when @item is the item already shown. Otherwise the
 *    previous item (if any) is unreferenced and its handlers disconnected; the
 *    pane holds one reference on @item (if non-NULL), follows its "changed"
 *    and "detail-changed" signals, and shows its texts, badge and chart. For
 *    NULL the placeholder is shown and the chart is cleared.
 * Thread affinity: main thread only.
 * Ownership: @item stays with the caller (the pane takes its own reference).
 */
void stk_detail_set_item(StkDetail *self, StkRowItem *item);

/**
 * stk_detail_get_range:
 * @self: (not nullable): the pane; must satisfy STK_IS_DETAIL().
 *
 * Preconditions:
 *  - @self satisfies STK_IS_DETAIL() (else: critical, STK_RANGE_1D returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides @self).
 *
 * Returns: the selected range (always valid for the current item's kind).
 */
StkRange stk_detail_get_range(StkDetail *self);

/**
 * stk_detail_set_offline:
 * @self: (not nullable): the pane; must satisfy STK_IS_DETAIL().
 * @offline: whether the network is unavailable (any non-zero value counts as
 *   TRUE).
 *
 * Shows or hides the "Offline" badge (it takes precedence over "Stale") and
 * marks the chart stale while offline. Does nothing when unchanged.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_DETAIL() (else: critical, nothing changed) and is
 *    not disposed.
 * Postconditions:
 *  - the offline flag is @offline; the badge and the chart's stale marker
 *    were refreshed iff it changed.
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides @self).
 */
void stk_detail_set_offline(StkDetail *self, gboolean offline);

G_END_DECLS

#endif /* STK_DETAIL_H */
