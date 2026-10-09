/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-cache.h - bounded on-disk cache of the latest StkQuote and StkSeries per
 * symbol, used to show stale data instantly and when offline.
 *
 * Threading: a StkCache is main-thread-only (no locking); every function
 * except the internals of stk_cache_save_async()'s worker must be called from
 * the thread that owns the cache. stk_cache_load() and stk_cache_save() are
 * SYNCHRONOUS file operations (load: one bounded read plus JSON parsing; save:
 * serialisation plus write + fsync) and are meant for start-up and for
 * shutdown. For saving while the UI runs use stk_cache_save_async(), which
 * serialises and writes on a worker thread.
 *
 * The cache never saves by itself. Debouncing is the caller's job: after
 * put_*() calls, schedule one save (e.g. g_timeout_add_seconds) when
 * stk_cache_is_dirty() is TRUE. Before the process exits, keep the main loop
 * running until the save_async callback has fired: exiting in the middle of a
 * write leaves a "<path>.stk-tmp-XXXXXX" temporary file (megabytes), which the
 * next stk_cache_load() removes once it is older than a minute.
 */
#ifndef STK_CACHE_H
#define STK_CACHE_H

#include <gio/gio.h>

#include "stk-types.h"

G_BEGIN_DECLS

/** Maximum number of symbols held; storing a new one evicts the least recently used. */
#define STK_CACHE_MAX_SYMBOLS 200
/** Largest series (in points) accepted by stk_cache_put_series(). */
#define STK_CACHE_MAX_SERIES_POINTS 2000
/**
 * Maximum points held across all series. Exceeding it evicts least recently
 * used symbols (never the one just stored). This bounds both memory and the
 * file size: a file written by this module is always smaller than
 * STK_CACHE_MAX_FILE_BYTES.
 */
#define STK_CACHE_MAX_TOTAL_POINTS 250000
/** Largest cache file that stk_cache_load() accepts. */
#define STK_CACHE_MAX_FILE_BYTES (16 * 1024 * 1024)
/** Maximum byte length of a cached quote's name. */
#define STK_CACHE_MAX_NAME_BYTES 256

#define STK_TYPE_CACHE (stk_cache_get_type())
G_DECLARE_FINAL_TYPE(StkCache, stk_cache, STK, CACHE, GObject)

/**
 * stk_cache_new:
 * @path: (nullable): file used by load/save; NULL selects
 *   g_get_user_cache_dir()/stocks/cache.json. Copied.
 *
 * Returns: (transfer full): a new empty, clean (not dirty) cache. Nothing is
 *   read from disk until stk_cache_load() is called.
 */
StkCache *stk_cache_new(const char *path);

/** Returns: the file path used by load/save; owned by @self. */
const char *stk_cache_get_path(StkCache *self);

/** Returns: the number of symbols currently held (at most STK_CACHE_MAX_SYMBOLS). */
guint stk_cache_get_n_symbols(StkCache *self);

/**
 * stk_cache_put_quote:
 * @self: the cache.
 * @quote: quote to store (deep-copied). Its symbol must satisfy
 *   stk_symbol_is_valid(); its name must be valid UTF-8 of at most
 *   STK_CACHE_MAX_NAME_BYTES bytes; its currency must be 1-8 ASCII letters or
 *   digits; its kind must be valid; price/change/change_pct may be NAN
 *   (stored as JSON null) but must not be infinite; as_of_ms must be >= 0.
 * @fetched_at_ms: when the data was fetched, ms since the Unix epoch (UTC);
 *   must be >= 0.
 *
 * Replaces the symbol's cached quote, makes the symbol most recently used and
 * marks the cache dirty. Storing a new symbol into a full cache evicts the
 * least recently used symbol (with all its data).
 *
 * Returns: TRUE if stored; FALSE, with the cache unchanged, if any
 *   requirement above is violated.
 */
gboolean stk_cache_put_quote(StkCache *self, const StkQuote *quote, gint64 fetched_at_ms);

/**
 * stk_cache_get_quote:
 * @self: the cache.
 * @symbol: (nullable): ticker, matched ASCII-case-insensitively.
 * @age_ms: (out) (nullable): receives the milliseconds elapsed since the
 *   stored fetched_at_ms according to the system clock, clamped to >= 0.
 *
 * Makes the symbol most recently used when a quote is returned (this does not
 * mark the cache dirty).
 *
 * Returns: (transfer full) (nullable): a copy of the cached quote (release
 *   with stk_quote_free()), or NULL if @symbol is invalid or has no quote.
 */
StkQuote *stk_cache_get_quote(StkCache *self, const char *symbol, gint64 *age_ms);

/**
 * stk_cache_put_series:
 * @self: the cache.
 * @symbol: ticker; must satisfy stk_symbol_is_valid().
 * @series: series to store for (@symbol, @series->range); at most
 *   STK_CACHE_MAX_SERIES_POINTS points. The cache takes a reference without
 *   copying the points, so the caller MUST NOT append to it afterwards.
 * @fetched_at_ms: when the data was fetched, ms since the Unix epoch; >= 0.
 *
 * Replaces any series stored for the same (symbol, range), makes the symbol
 * most recently used and marks the cache dirty. May evict least recently used
 * symbols to honour STK_CACHE_MAX_SYMBOLS and STK_CACHE_MAX_TOTAL_POINTS.
 *
 * Returns: TRUE if stored; FALSE, with the cache unchanged, if @symbol is
 *   invalid, @series is NULL, has an invalid range or too many points, or
 *   @fetched_at_ms is negative.
 */
gboolean stk_cache_put_series(StkCache *self, const char *symbol, StkSeries *series,
                              gint64 fetched_at_ms);

/**
 * stk_cache_get_series:
 * @self: the cache.
 * @symbol: (nullable): ticker, matched ASCII-case-insensitively.
 * @range: a valid StkRange.
 * @age_ms: (out) (nullable): as in stk_cache_get_quote().
 *
 * Makes the symbol most recently used when a series is returned (does not mark
 * the cache dirty).
 *
 * Returns: (transfer full) (nullable): a new reference to the cached series
 *   (release with stk_series_unref()), or NULL if absent or @symbol / @range
 *   is invalid.
 */
StkSeries *stk_cache_get_series(StkCache *self, const char *symbol, StkRange range,
                                gint64 *age_ms);

/**
 * stk_cache_is_dirty:
 * @self: the cache.
 *
 * Returns: TRUE iff put_*() stored something since the last stk_cache_load()
 *   (successful or not) or successful save. A save started before the latest
 *   put does not clear the flag.
 */
gboolean stk_cache_is_dirty(StkCache *self);

/**
 * stk_cache_load:
 * @self: the cache.
 * @error: (nullable): return location for a GError.
 *
 * Synchronously replaces the cache content with the file at
 * stk_cache_get_path(). Entries that are invalid (bad symbol, malformed quote
 * or series, wrong types) are skipped individually; the rest is kept in the
 * stored recency order; skipped elements never count against the limits. A
 * symbol (case-insensitive) that repeats an EARLIER accepted element is
 * skipped as a duplicate even if that earlier element has been evicted
 * meanwhile: the first accepted occurrence is the only one that can load. If
 * the file holds more than STK_CACHE_MAX_SYMBOLS valid symbols the least
 * recently used ones are ignored, and STK_CACHE_MAX_TOTAL_POINTS is enforced
 * as for put_series(). Also removes stale "<path>.stk-tmp-XXXXXX" temporary
 * files (see stk_file_remove_stale_temps()).
 *
 * On any failure the cache is left EMPTY and @error is set:
 * - G_FILE_ERROR / G_FILE_ERROR_NOENT: no file yet (normal on first run);
 * - G_IO_ERROR / G_IO_ERROR_MESSAGE_TOO_LARGE: file larger than
 *   STK_CACHE_MAX_FILE_BYTES; other G_FILE_ERROR codes: I/O failure;
 * - STK_ERROR / STK_ERROR_PARSE: not valid JSON, wrong shape, unsupported
 *   "version", or rejected by stk_json_check_strict() (comments, single-quoted
 *   strings or other non-RFC 8259 bytes, or nesting deeper than
 *   STK_JSON_MAX_DEPTH; checked before the JSON parser sees the data).
 * No GLib warning or critical is logged for any file content. Either way the
 * cache is clean (not dirty) afterwards.
 *
 * Fails with G_IO_ERROR / G_IO_ERROR_PENDING (cache untouched) while an
 * asynchronous save is running.
 *
 * Returns: TRUE if the file was loaded.
 */
gboolean stk_cache_load(StkCache *self, GError **error);

/**
 * stk_cache_save:
 * @self: the cache.
 * @error: (nullable): return location for a GError.
 *
 * Synchronously writes the whole cache (in recency order, least recently used
 * first) with stk_file_write_atomic(): the old file stays intact on failure.
 * On success the cache becomes clean. Saves unconditionally, even when the
 * cache is already clean.
 *
 * Fails with G_IO_ERROR / G_IO_ERROR_PENDING if an asynchronous save is
 * running. Other errors are in the G_FILE_ERROR domain (a document larger than
 * STK_CACHE_MAX_FILE_BYTES, which the limits above make impossible, would report
 * G_IO_ERROR_MESSAGE_TOO_LARGE).
 *
 * Returns: TRUE if the file was replaced.
 */
gboolean stk_cache_save(StkCache *self, GError **error);

/**
 * stk_cache_save_async:
 * @self: the cache.
 * @cancellable: (nullable): cancelling before the worker starts writing makes
 *   the save fail with G_IO_ERROR_CANCELLED (nothing is written); once writing
 *   has started it runs to completion and is reported as a success even if
 *   cancellation arrives meanwhile.
 * @callback: (nullable): invoked in the caller's thread-default main context.
 * @user_data: passed to @callback.
 *
 * Takes a snapshot of the cache on the calling thread (cheap: series are
 * reference-counted, not copied) and serialises + writes it on a worker
 * thread. Only one save may run at a time: if one is already running,
 * @callback receives G_IO_ERROR_PENDING (from the main loop, never
 * synchronously) and the dirty flag is untouched, so the caller should retry
 * later. Saves unconditionally, even when the cache is clean. On success the
 * dirty flag is cleared before @callback runs, unless put_*() stored
 * something after the snapshot was taken. The cache is kept alive until the
 * save finishes.
 */
void stk_cache_save_async(StkCache *self, GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data);

/**
 * stk_cache_save_finish:
 * @self: the cache.
 * @result: the result passed to the callback of stk_cache_save_async().
 * @error: (nullable): return location for a GError.
 *
 * Returns: TRUE if the file was replaced.
 */
gboolean stk_cache_save_finish(StkCache *self, GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* STK_CACHE_H */
