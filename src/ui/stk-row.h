/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-row.h - the watchlist's per-instrument model (StkRowItem), the list row
 * widget that shows it (StkRow) and the number/text formatting shared with
 * the detail pane.
 *
 * Threading: main thread only (GObject / GtkWidget).
 *
 * StkRowItem
 * ----------
 * One instrument of the watchlist: symbol, display name, kind and the data
 * shown for it: the latest quote, the sparkline series (STK_SCHED_SPARK_RANGE,
 * 1M), a stale flag and, only while the item is the selected one, the series of
 * the detail chart. Every setter compares the new value with the stored one
 * first and returns FALSE without emitting anything when nothing changed, so
 * widgets that listen to the item are touched only when data really changed.
 * Items are created and updated by StkApp; widgets only read them.
 *
 * StkRow
 * ------
 * The widget bound to an item by the list view's factory: symbol, name,
 * price, change %, StkSparkline and a "..." menu button (Move up, Move down,
 * Remove) with the CSS class "stk-row". The row is also a drag source and drop
 * target: dropping another row on it asks to move that row to this row's
 * position. The row never touches the watchlist: it only emits requests that
 * carry the symbols involved; the owner validates them (a drop may originate
 * outside the application) and acts.
 *
 * CSS: the labels have the style classes "stk-symbol", "stk-name",
 * "stk-price" and "stk-change"; the last carries "positive", "negative" or
 * "flat". The row has the class "stale" while the item is stale.
 */
#ifndef STK_ROW_H
#define STK_ROW_H

#include <gtk/gtk.h>

#include "stk-types.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* Formatting                                                               */
/* ------------------------------------------------------------------------ */

/** Text shown for a missing number: an em dash. */
#define STK_MISSING_TEXT "\xe2\x80\x94"

/**
 * stk_format_price:
 * @value: any double.
 *
 * Returns: (transfer full): "%.2f" of @value, "%.3e" when |value| >= 1e12
 *   (keeps the text short whatever the input), or the em dash for NAN/inf.
 */
char *stk_format_price(double value);

/**
 * stk_format_change:
 * @value: any double.
 *
 * Returns: (transfer full): like stk_format_price() but with an explicit sign
 *   ("+1.25", "-0.40"); a value that rounds to 0.00 has no sign.
 */
char *stk_format_change(double value);

/**
 * stk_format_percent:
 * @value: percent units (1.5 == 1.5%); any double.
 *
 * Returns: (transfer full): stk_format_change() of @value followed by "%".
 */
char *stk_format_percent(double value);

/**
 * stk_change_class:
 * @value: a change (absolute or percent); any double.
 *
 * Returns: (transfer none): "positive" if it displays as > 0, "negative" if it
 *   displays as < 0, "flat" otherwise (including NAN). Static string.
 */
const char *stk_change_class(double value);

/** Only the first this many bytes of a text are used by stk_sanitize_text(). */
#define STK_SANITIZE_MAX_BYTES 1024

/**
 * stk_sanitize_text:
 * @text: (nullable): text from disk or the network, of any length.
 *
 * Returns: (transfer full): the first STK_SANITIZE_MAX_BYTES bytes of @text
 *   (cut between characters) as valid UTF-8 in which control, line/paragraph
 *   separator and format (bidirectional override) characters are replaced by
 *   spaces; "" for NULL. Safe for a single-line label. Runs in time linear in
 *   min(strlen(@text), STK_SANITIZE_MAX_BYTES), whatever the content.
 */
char *stk_sanitize_text(const char *text);

/* ------------------------------------------------------------------------ */
/* StkRowItem                                                               */
/* ------------------------------------------------------------------------ */

#define STK_TYPE_ROW_ITEM (stk_row_item_get_type())
G_DECLARE_FINAL_TYPE(StkRowItem, stk_row_item, STK, ROW_ITEM, GObject)

/**
 * StkRowItem::changed:
 * @self: the item.
 *
 * Emitted after the quote, the sparkline series or the stale flag changed.
 */

/**
 * StkRowItem::detail-changed:
 * @self: the item.
 *
 * Emitted after the detail series (or its range) changed.
 */

/**
 * stk_row_item_new:
 * @symbol: non-NULL ticker satisfying stk_symbol_is_valid() (copied).
 * @name: (nullable): display name (copied, made single-line safe with
 *   stk_sanitize_text()); NULL becomes "".
 * @kind: the instrument class.
 *
 * Returns: (transfer full): a new item without data (no quote, no series,
 *   not stale, detail range STK_RANGE_1D with no series), or NULL with a
 *   critical if @symbol is invalid.
 */
StkRowItem *stk_row_item_new(const char *symbol, const char *name, StkKind kind);

/** Returns: the symbol; owned by @self. */
const char *stk_row_item_get_symbol(StkRowItem *self);
/** Returns: the sanitised display name ("" when unknown); owned by @self. */
const char *stk_row_item_get_name(StkRowItem *self);
/** Returns: the kind. */
StkKind stk_row_item_get_kind(StkRowItem *self);
/** Returns: (nullable): the latest quote; owned by @self, valid until the
 *   next stk_row_item_set_quote(). */
const StkQuote *stk_row_item_get_quote(StkRowItem *self);
/** Returns: (nullable): the sparkline series; owned by @self (no new ref). */
StkSeries *stk_row_item_get_spark(StkRowItem *self);
/** Returns: whether the item is marked stale. */
gboolean stk_row_item_get_stale(StkRowItem *self);
/** Returns: the range the detail series belongs to. */
StkRange stk_row_item_get_detail_range(StkRowItem *self);
/** Returns: (nullable): the detail series; owned by @self (no new ref). */
StkSeries *stk_row_item_get_detail_series(StkRowItem *self);

/**
 * stk_row_item_set_quote:
 * @self: the item.
 * @quote: (nullable): the new quote (deep-copied), NULL to clear it.
 *
 * Compares price, change, change %, timestamp and the realtime flag.
 *
 * Returns: TRUE (and "changed" emitted) iff any of them differs.
 */
gboolean stk_row_item_set_quote(StkRowItem *self, const StkQuote *quote);

/**
 * stk_row_item_set_spark:
 * @self: the item.
 * @series: (nullable) (transfer none): the new sparkline series (a reference
 *   is taken); NULL clears it. The caller must not modify it afterwards.
 *
 * Returns: TRUE (and "changed" emitted) iff the series content (range,
 *   prev_close, every point) differs; on FALSE the old series is kept.
 */
gboolean stk_row_item_set_spark(StkRowItem *self, StkSeries *series);

/**
 * stk_row_item_set_stale:
 * @self: the item.
 * @stale: the new flag.
 *
 * Returns: TRUE (and "changed" emitted) iff it differs.
 */
gboolean stk_row_item_set_stale(StkRowItem *self, gboolean stale);

/**
 * stk_row_item_set_detail:
 * @self: the item.
 * @range: the range @series belongs to (a valid StkRange).
 * @series: (nullable) (transfer none): the detail series (a reference is
 *   taken); NULL clears it. The caller must not modify it afterwards.
 *
 * Returns: TRUE (and "detail-changed" emitted) iff the range or the series
 *   content differs; on FALSE the old series is kept.
 */
gboolean stk_row_item_set_detail(StkRowItem *self, StkRange range, StkSeries *series);

/* ------------------------------------------------------------------------ */
/* StkRow                                                                   */
/* ------------------------------------------------------------------------ */

#define STK_TYPE_ROW (stk_row_get_type())
G_DECLARE_FINAL_TYPE(StkRow, stk_row, STK, ROW, GtkBox)

/**
 * StkRow::remove-requested:
 * @self: the row.
 * @symbol: the bound item's symbol.
 *
 * The user chose "Remove" in the row menu.
 */

/**
 * StkRow::move-requested:
 * @self: the row.
 * @symbol: the bound item's symbol.
 * @delta: -1 (Move up) or +1 (Move down).
 */

/**
 * StkRow::reorder-requested:
 * @self: the row.
 * @source: the symbol carried by a drop on this row; UNTRUSTED text (it may
 *   come from another application).
 * @target: this row's symbol: the dropped instrument should take its place.
 */

/**
 * stk_row_new:
 *
 * Returns: (transfer floating): an unbound row.
 */
GtkWidget *stk_row_new(void);

/**
 * stk_row_bind:
 * @self: the row.
 * @item: (nullable): the item to show (a reference is held while bound);
 *   NULL unbinds and blanks the row.
 *
 * Shows @item now and follows its "changed" signal until unbound or disposed.
 */
void stk_row_bind(StkRow *self, StkRowItem *item);

G_END_DECLS

#endif /* STK_ROW_H */
