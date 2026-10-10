/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-row.h - the watchlist's per-instrument model (StkRowItem), the list row
 * widget that shows it (StkRow) and the number/text formatting shared with
 * the detail pane.
 *
 * Threading: StkRowItem and StkRow are main thread only (GObject / GtkWidget).
 * The formatting functions and stk_sanitize_text() keep no state and touch
 * only their arguments (they use g_strdup_printf(), so the decimal separator
 * follows the process's LC_NUMERIC locale): any thread.
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
 * @value: any double (NAN and infinities are handled).
 *
 * Preconditions:
 *  - none; every double is accepted.
 * Postconditions:
 *  - nothing is changed; a new string is returned.
 * Thread affinity: any thread (no state).
 * Ownership: none (no pointers besides the returned string, which the caller
 *   owns).
 *
 * Returns: (transfer full): "%.2f" of @value, "%.3e" when |value| >= 1e12
 *   (keeps the text short whatever the input), or the em dash for NAN/inf.
 *   Never NULL.
 */
char *stk_format_price(double value);

/**
 * stk_format_change:
 * @value: any double (NAN and infinities are handled).
 *
 * Preconditions:
 *  - none; every double is accepted.
 * Postconditions:
 *  - nothing is changed; a new string is returned.
 * Thread affinity: any thread (no state).
 * Ownership: none (no pointers besides the returned string, which the caller
 *   owns).
 *
 * Returns: (transfer full): like stk_format_price() but with an explicit sign
 *   ("+1.25", "-0.40"); a value that rounds to 0.00 has no sign. The em dash
 *   for NAN/inf. Never NULL.
 */
char *stk_format_change(double value);

/**
 * stk_format_percent:
 * @value: percent units (1.5 == 1.5%); any double (NAN and infinities are
 *   handled).
 *
 * Preconditions:
 *  - none; every double is accepted.
 * Postconditions:
 *  - nothing is changed; a new string is returned.
 * Thread affinity: any thread (no state).
 * Ownership: none (no pointers besides the returned string, which the caller
 *   owns).
 *
 * Returns: (transfer full): stk_format_change() of @value followed by "%";
 *   the em dash alone (no "%") for NAN/inf. Never NULL.
 */
char *stk_format_percent(double value);

/**
 * stk_change_class:
 * @value: a change (absolute or percent); any double.
 *
 * Preconditions:
 *  - none; every double is accepted.
 * Postconditions:
 *  - nothing is changed.
 * Thread affinity: any thread (no state).
 * Ownership: none (no pointers besides the returned static string).
 *
 * Returns: (transfer none): "positive" if it displays as > 0, "negative" if it
 *   displays as < 0, "flat" otherwise (including NAN). Static string, never
 *   NULL.
 */
const char *stk_change_class(double value);

/** Only the first this many bytes of a text are used by stk_sanitize_text(). */
#define STK_SANITIZE_MAX_BYTES 1024

/**
 * stk_sanitize_text:
 * @text: (nullable): text from disk or the network.
 *
 * Preconditions:
 *  - @text is NULL, or NUL-terminated, or points to at least
 *    STK_SANITIZE_MAX_BYTES + 1 readable bytes (only that many are ever
 *    examined, so an over-long or unterminated text is never scanned further).
 * Postconditions:
 *  - nothing is changed; a new string is returned. Only the first
 *    STK_SANITIZE_MAX_BYTES bytes of @text are used, cut so that no multi-byte
 *    sequence is split. Runs in time linear in
 *    min(length of @text, STK_SANITIZE_MAX_BYTES), whatever the content.
 * Thread affinity: any thread (no state).
 * Ownership: @text is borrowed for the call only; the result is owned by the
 *   caller.
 *
 * Returns: (transfer full): the used part of @text as valid UTF-8 (each
 *   invalid byte becomes U+FFFD, so the result may be longer than the input
 *   part, at most 3 * STK_SANITIZE_MAX_BYTES bytes) in which control,
 *   line/paragraph separator and format (bidirectional override) characters
 *   are replaced by spaces; "" for NULL. Safe for a single-line label. Never
 *   NULL.
 */
char *stk_sanitize_text(const char *text);

/* ------------------------------------------------------------------------ */
/* StkRowItem                                                               */
/* ------------------------------------------------------------------------ */

/*
 * G_DECLARE_FINAL_TYPE() below declares the types StkRowItem and
 * StkRowItemClass, stk_row_item_get_type(), the inline cast function
 * STK_ROW_ITEM() and the inline type-check function STK_IS_ROW_ITEM(), and
 * the g_autoptr() cleanup for StkRowItem. STK_TYPE_ROW_ITEM is defined here.
 * The type itself is defined by G_DEFINE_FINAL_TYPE() in stk-row.c.
 */
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
 * @symbol: non-NULL ticker satisfying stk_symbol_is_valid() (copied,
 *   ASCII-uppercased).
 * @name: (nullable): display name (copied, made single-line safe with
 *   stk_sanitize_text()); NULL becomes "".
 * @kind: the instrument class.
 *
 * Preconditions:
 *  - @symbol is non-NULL and satisfies stk_symbol_is_valid() (else: critical,
 *    NULL returned, nothing allocated).
 *  - @name is NULL, or satisfies the preconditions of stk_sanitize_text().
 * Postconditions:
 *  - on success the item holds copies of @symbol and of the sanitised @name;
 *    @symbol and @name are not retained.
 * Thread affinity: main thread only.
 * Ownership: @symbol and @name are borrowed for the call only; the returned
 *   item is owned by the caller.
 *
 * Returns: (transfer full): a new item without data (no quote, no series,
 *   not stale, detail range STK_RANGE_1D with no series), or NULL with a
 *   critical if @symbol is invalid.
 */
StkRowItem *stk_row_item_new(const char *symbol, const char *name, StkKind kind);

/**
 * stk_row_item_get_symbol:
 * @self: (not nullable): the item; must satisfy STK_IS_ROW_ITEM().
 *
 * Preconditions:
 *  - @self satisfies STK_IS_ROW_ITEM() (else: critical, NULL returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread only.
 * Ownership: the string is owned by @self and lives as long as it.
 *
 * Returns: (transfer none): the upper-case symbol (1..15 bytes, NUL-terminated).
 */
const char *stk_row_item_get_symbol(StkRowItem *self);

/**
 * stk_row_item_get_name:
 * @self: (not nullable): the item; must satisfy STK_IS_ROW_ITEM().
 *
 * Preconditions:
 *  - @self satisfies STK_IS_ROW_ITEM() (else: critical, NULL returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread only.
 * Ownership: the string is owned by @self and lives as long as it.
 *
 * Returns: (transfer none): the sanitised display name ("" when unknown).
 */
const char *stk_row_item_get_name(StkRowItem *self);

/**
 * stk_row_item_get_kind:
 * @self: (not nullable): the item; must satisfy STK_IS_ROW_ITEM().
 *
 * Preconditions:
 *  - @self satisfies STK_IS_ROW_ITEM() (else: critical, STK_KIND_STOCK
 *    returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides @self).
 *
 * Returns: the kind.
 */
StkKind stk_row_item_get_kind(StkRowItem *self);

/**
 * stk_row_item_get_quote:
 * @self: (not nullable): the item; must satisfy STK_IS_ROW_ITEM().
 *
 * Preconditions:
 *  - @self satisfies STK_IS_ROW_ITEM() (else: critical, NULL returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread only.
 * Ownership: the quote is owned by @self; no copy and no reference is made.
 *
 * Returns: (transfer none) (nullable): the latest quote, valid until the next
 *   stk_row_item_set_quote() that changes it (or until @self is finalized);
 *   NULL if there is none.
 */
const StkQuote *stk_row_item_get_quote(StkRowItem *self);

/**
 * stk_row_item_get_spark:
 * @self: (not nullable): the item; must satisfy STK_IS_ROW_ITEM().
 *
 * Preconditions:
 *  - @self satisfies STK_IS_ROW_ITEM() (else: critical, NULL returned).
 * Postconditions:
 *  - no state is changed; no reference is added.
 * Thread affinity: main thread only.
 * Ownership: the series is owned by @self (no new ref); a caller that keeps it
 *   beyond the next stk_row_item_set_spark() must take its own reference.
 *
 * Returns: (transfer none) (nullable): the sparkline series; NULL if none.
 */
StkSeries *stk_row_item_get_spark(StkRowItem *self);

/**
 * stk_row_item_get_stale:
 * @self: (not nullable): the item; must satisfy STK_IS_ROW_ITEM().
 *
 * Preconditions:
 *  - @self satisfies STK_IS_ROW_ITEM() (else: critical, FALSE returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides @self).
 *
 * Returns: whether the item is marked stale.
 */
gboolean stk_row_item_get_stale(StkRowItem *self);

/**
 * stk_row_item_get_detail_range:
 * @self: (not nullable): the item; must satisfy STK_IS_ROW_ITEM().
 *
 * Preconditions:
 *  - @self satisfies STK_IS_ROW_ITEM() (else: critical, STK_RANGE_1D
 *    returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides @self).
 *
 * Returns: the range the detail series belongs to (STK_RANGE_1D for a new
 *   item).
 */
StkRange stk_row_item_get_detail_range(StkRowItem *self);

/**
 * stk_row_item_get_detail_series:
 * @self: (not nullable): the item; must satisfy STK_IS_ROW_ITEM().
 *
 * Preconditions:
 *  - @self satisfies STK_IS_ROW_ITEM() (else: critical, NULL returned).
 * Postconditions:
 *  - no state is changed; no reference is added.
 * Thread affinity: main thread only.
 * Ownership: the series is owned by @self (no new ref); a caller that keeps it
 *   beyond the next stk_row_item_set_detail() must take its own reference.
 *
 * Returns: (transfer none) (nullable): the detail series; NULL if none.
 */
StkSeries *stk_row_item_get_detail_series(StkRowItem *self);

/**
 * stk_row_item_set_quote:
 * @self: (not nullable): the item; must satisfy STK_IS_ROW_ITEM().
 * @quote: (nullable): the new quote (deep-copied), NULL to clear it.
 *
 * Compares price, change, change %, timestamp and the realtime flag.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_ROW_ITEM() (else: critical, FALSE returned).
 *  - @quote is NULL or a valid StkQuote.
 * Postconditions:
 *  - when any compared field differs (or one side is NULL and the other is
 *    not) the item holds a deep copy of @quote (or none) and "changed" has been
 *    emitted after the state was updated; otherwise nothing is changed and
 *    the old quote is kept. @quote is not retained.
 * Thread affinity: main thread only.
 * Ownership: @quote is borrowed for the call only; the copy is owned by @self.
 *
 * Returns: TRUE (and "changed" emitted) iff any of them differs.
 */
gboolean stk_row_item_set_quote(StkRowItem *self, const StkQuote *quote);

/**
 * stk_row_item_set_spark:
 * @self: (not nullable): the item; must satisfy STK_IS_ROW_ITEM().
 * @series: (nullable) (transfer none): the new sparkline series (a reference
 *   is taken); NULL clears it. The caller must not modify it afterwards.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_ROW_ITEM() (else: critical, FALSE returned).
 *  - @series is NULL or a valid StkSeries.
 * Postconditions:
 *  - on TRUE the item holds one reference on @series (or none) and has
 *    released its previous one; "changed" was emitted after the state was
 *    updated. On FALSE nothing is changed and no reference is taken.
 * Thread affinity: main thread only.
 * Ownership: @series stays with the caller (the item takes its own reference).
 *
 * Returns: TRUE (and "changed" emitted) iff the series content (range,
 *   prev_close, every point) differs; on FALSE the old series is kept.
 */
gboolean stk_row_item_set_spark(StkRowItem *self, StkSeries *series);

/**
 * stk_row_item_set_stale:
 * @self: (not nullable): the item; must satisfy STK_IS_ROW_ITEM().
 * @stale: the new flag (any non-zero value counts as TRUE).
 *
 * Preconditions:
 *  - @self satisfies STK_IS_ROW_ITEM() (else: critical, FALSE returned).
 * Postconditions:
 *  - the flag is stored as TRUE or FALSE; "changed" was emitted after it
 *    was updated iff it differed.
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides @self).
 *
 * Returns: TRUE (and "changed" emitted) iff it differs.
 */
gboolean stk_row_item_set_stale(StkRowItem *self, gboolean stale);

/**
 * stk_row_item_set_detail:
 * @self: (not nullable): the item; must satisfy STK_IS_ROW_ITEM().
 * @range: the range @series belongs to (a valid StkRange).
 * @series: (nullable) (transfer none): the detail series (a reference is
 *   taken); NULL clears it. The caller must not modify it afterwards.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_ROW_ITEM() and @range is STK_RANGE_1D..STK_RANGE_1Y
 *    (else: critical, FALSE returned, nothing changed).
 *  - @series is NULL or a valid StkSeries.
 * Postconditions:
 *  - on TRUE the item holds @range and one reference on @series (or none) and
 *    has released its previous series; "detail-changed" was emitted after the
 *    state was updated. On FALSE nothing is changed and no reference is taken.
 * Thread affinity: main thread only.
 * Ownership: @series stays with the caller (the item takes its own reference).
 *
 * Returns: TRUE (and "detail-changed" emitted) iff the range or the series
 *   content differs; on FALSE the old series is kept.
 */
gboolean stk_row_item_set_detail(StkRowItem *self, StkRange range, StkSeries *series);

/* ------------------------------------------------------------------------ */
/* StkRow                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * G_DECLARE_FINAL_TYPE() below declares the types StkRow and StkRowClass,
 * stk_row_get_type(), the inline cast function STK_ROW() and the inline
 * type-check function STK_IS_ROW(), and the g_autoptr() cleanup for StkRow.
 * STK_TYPE_ROW is defined here. The type itself is defined by
 * G_DEFINE_FINAL_TYPE() in stk-row.c.
 */
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
 * Preconditions:
 *  - GTK is initialised (a display is available) and the call is made on the
 *    main thread.
 * Postconditions:
 *  - the row shows nothing and is bound to no item.
 * Thread affinity: main thread only.
 * Ownership: none (no pointers besides the returned widget, which is floating).
 *
 * Returns: (transfer floating): an unbound row.
 */
GtkWidget *stk_row_new(void);

/**
 * stk_row_bind:
 * @self: (not nullable): the row; must satisfy STK_IS_ROW().
 * @item: (nullable): the item to show (a reference is held while bound);
 *   NULL unbinds and blanks the row.
 *
 * Shows @item now and follows its "changed" signal until unbound or disposed.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_ROW() and @item is NULL or satisfies
 *    STK_IS_ROW_ITEM() (else: critical, nothing changed).
 *  - the caller keeps @item alive for the call by a reference of its own (the
 *    row's reference on a previously bound item is dropped before the new one
 *    is taken); the GtkListItemFactory callbacks satisfy this, the item model
 *    holds every item.
 * Postconditions:
 *  - the previous item (if any) is unreferenced and its "changed" handler
 *    disconnected; the row holds one reference on @item (if non-NULL) and
 *    follows its "changed" signal; the row displays @item's current data (or
 *    is blank for NULL).
 * Thread affinity: main thread only.
 * Ownership: @item stays with the caller (the row takes its own reference).
 */
void stk_row_bind(StkRow *self, StkRowItem *item);

G_END_DECLS

#endif /* STK_ROW_H */
