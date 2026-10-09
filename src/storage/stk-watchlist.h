/*
 * stk-watchlist.h - the user's ordered list of tracked instruments, persisted
 * as JSON.
 *
 * Threading: a StkWatchlist is main-thread-only (no locking); every function
 * except the internals of stk_watchlist_save_async()'s worker must be called
 * from the thread that owns the list. stk_watchlist_load() and
 * stk_watchlist_save() are SYNCHRONOUS file operations (save includes fsync
 * and rename) meant for start-up, shutdown and tests only. The UI MUST persist
 * edits with stk_watchlist_save_async(), which snapshots the list on the
 * calling thread and serialises + writes on a worker thread; running the
 * synchronous save from an idle or debounce handler would still block the main
 * loop. Never hold pointers returned by get_entry() across a mutation.
 *
 * The list never saves by itself. Debouncing is the caller's job: after edits,
 * schedule one save when stk_watchlist_is_dirty() is TRUE. Before the process
 * exits, keep the main loop running until the save callback has fired (an
 * exit in the middle of a write leaves a "<path>.stk-tmp-XXXXXX" temporary file,
 * which the next stk_watchlist_load() removes once it is older than a minute).
 */
#ifndef STK_WATCHLIST_H
#define STK_WATCHLIST_H

#include <gio/gio.h>

#include "stk-types.h"

G_BEGIN_DECLS

/** Maximum number of entries; add() fails when reached, load() drops the rest. */
#define STK_WATCHLIST_MAX_ENTRIES 500
/** Maximum byte length of an entry's display name. */
#define STK_WATCHLIST_MAX_NAME_BYTES 256
/** Largest watchlist file that load() accepts. */
#define STK_WATCHLIST_MAX_FILE_BYTES (4 * 1024 * 1024)

#define STK_TYPE_WATCHLIST (stk_watchlist_get_type())
G_DECLARE_FINAL_TYPE(StkWatchlist, stk_watchlist, STK, WATCHLIST, GObject)

/** One watchlist row. Owned by the list; read-only for callers. */
typedef struct {
  char *symbol; /**< upper-case, satisfies stk_symbol_is_valid() */
  char *name;   /**< valid UTF-8, at most STK_WATCHLIST_MAX_NAME_BYTES bytes, may be "" */
  StkKind kind;
} StkWatchlistEntry;

/**
 * StkWatchlist::changed:
 * @self: the list.
 *
 * Emitted (synchronously, once per call) after add(), remove() or move()
 * changed the list, and after load() when the list was or became non-empty.
 * It is NOT emitted by calls that fail or change nothing, nor by saving.
 * Handlers may read the list; they MUST NOT mutate it re-entrantly.
 */

/**
 * stk_watchlist_new:
 * @path: (nullable): file used by load()/save(); NULL selects
 *   g_get_user_config_dir()/stocks/watchlist.json. The string is copied.
 *
 * Returns: (transfer full): a new empty list. Nothing is read from disk until
 *   stk_watchlist_load() is called.
 */
StkWatchlist *stk_watchlist_new(const char *path);

/** Returns: the file path used by load()/save(); owned by @self. */
const char *stk_watchlist_get_path(StkWatchlist *self);

/** Returns: the number of entries. */
guint stk_watchlist_get_n_entries(StkWatchlist *self);

/**
 * stk_watchlist_get_entry:
 * @self: the list.
 * @index: zero-based position.
 *
 * Returns: (transfer none) (nullable): the entry, or NULL if @index is out of
 *   range. Valid only until the next add/remove/move/load.
 */
const StkWatchlistEntry *stk_watchlist_get_entry(StkWatchlist *self, guint index);

/**
 * stk_watchlist_index_of:
 * @self: the list.
 * @symbol: (nullable): ticker; compared ASCII-case-insensitively.
 *
 * Returns: the position of @symbol, or -1 if absent or @symbol is not a valid
 *   symbol (stk_symbol_is_valid()).
 */
gint stk_watchlist_index_of(StkWatchlist *self, const char *symbol);

/** Returns: TRUE iff stk_watchlist_index_of() would return >= 0. */
gboolean stk_watchlist_contains(StkWatchlist *self, const char *symbol);

/**
 * stk_watchlist_add:
 * @self: the list.
 * @symbol: (nullable): ticker; must satisfy stk_symbol_is_valid(). Stored
 *   ASCII-upper-cased.
 * @name: (nullable): display name; NULL means "". Must be valid UTF-8 of at
 *   most STK_WATCHLIST_MAX_NAME_BYTES bytes. Copied.
 * @kind: a valid StkKind.
 *
 * Appends a new entry at the end. Emits "changed" on success.
 *
 * Returns: TRUE if added. FALSE, with the list unchanged, if the symbol is
 *   invalid, already present (case-insensitively), the name is invalid or too
 *   long, @kind is invalid, or the list holds STK_WATCHLIST_MAX_ENTRIES.
 */
gboolean stk_watchlist_add(StkWatchlist *self, const char *symbol, const char *name,
                           StkKind kind);

/**
 * stk_watchlist_remove:
 * @self: the list.
 * @symbol: (nullable): ticker; compared ASCII-case-insensitively.
 *
 * Emits "changed" on success.
 *
 * Returns: TRUE if an entry was removed; FALSE if @symbol was not present.
 */
gboolean stk_watchlist_remove(StkWatchlist *self, const char *symbol);

/**
 * stk_watchlist_move:
 * @self: the list.
 * @from: current zero-based position of the entry.
 * @to: zero-based position the entry has afterwards; the entries in between
 *   shift by one.
 *
 * Emits "changed" on success.
 *
 * Returns: TRUE if the order changed; FALSE (list unchanged) if either index
 *   is out of range or @from == @to.
 */
gboolean stk_watchlist_move(StkWatchlist *self, guint from, guint to);

/**
 * stk_watchlist_load:
 * @self: the list.
 * @n_dropped: (out) (nullable): receives how many stored entries were
 *   discarded (invalid symbol, kind, name or shape; duplicates; entries beyond
 *   STK_WATCHLIST_MAX_ENTRIES). Always set to 0 first, so it is 0 after any
 *   failure.
 * @error: (nullable): return location for a GError.
 *
 * Replaces the list with the content of the file at stk_watchlist_get_path()
 * (format: {"version":1,"entries":[{"symbol":..,"name":..,"kind":..}]}).
 * Valid entries are kept in file order; invalid ones are dropped and counted.
 *
 * Also removes stale "<path>.stk-tmp-XXXXXX" temporary files (see
 * stk_file_remove_stale_temps()). Synchronous blocking file I/O: start-up and
 * tests only.
 *
 * If an asynchronous save is running the call fails at once with
 * G_IO_ERROR / G_IO_ERROR_PENDING and changes nothing: the entries, the dirty
 * flag and the file are untouched, no signal is emitted (only @n_dropped is
 * set to 0). Every other failure leaves the list EMPTY, sets @error and
 * emits "changed" once if the list was non-empty; the list is then clean (not
 * dirty). Failures:
 * - G_FILE_ERROR / G_FILE_ERROR_NOENT: no file yet (normal on first run);
 * - G_IO_ERROR / G_IO_ERROR_MESSAGE_TOO_LARGE: file larger than
 *   STK_WATCHLIST_MAX_FILE_BYTES; other G_FILE_ERROR codes: I/O failure;
 * - STK_ERROR / STK_ERROR_PARSE: not valid JSON, not the expected shape, or
 *   an unsupported "version", or rejected by stk_json_check_strict() (comments,
 *   single-quoted strings or other non-RFC 8259 bytes, or nesting deeper than
 *   STK_JSON_MAX_DEPTH; checked before the JSON parser sees the data).
 * No GLib warning or critical is logged for any file content.
 *
 * On success "changed" is emitted once if the list was or became non-empty,
 * and the list is clean (not dirty).
 *
 * Returns: TRUE if the file was loaded (possibly with dropped entries).
 */
gboolean stk_watchlist_load(StkWatchlist *self, guint *n_dropped, GError **error);

/**
 * stk_watchlist_is_dirty:
 * @self: the list.
 *
 * Returns: TRUE iff add()/remove()/move() changed the list since the last
 *   stk_watchlist_load() (successful or not) or successful save. A save that
 *   was started before the latest edit does not clear the flag.
 */
gboolean stk_watchlist_is_dirty(StkWatchlist *self);

/**
 * stk_watchlist_save:
 * @self: the list.
 * @error: (nullable): return location for a GError (G_FILE_ERROR domain, or
 *   G_IO_ERROR / G_IO_ERROR_PENDING).
 *
 * Writes the list with stk_file_write_atomic(): on failure the previous file
 * is untouched. SYNCHRONOUS, includes fsync: for start-up, shutdown and tests;
 * the UI must use stk_watchlist_save_async(). Clears the dirty flag on
 * success. Fails with G_IO_ERROR_PENDING (nothing written) while an
 * asynchronous save is running.
 *
 * Returns: TRUE if the file was replaced.
 */
gboolean stk_watchlist_save(StkWatchlist *self, GError **error);

/**
 * stk_watchlist_save_async:
 * @self: the list.
 * @cancellable: (nullable): honoured only until the write starts; a
 *   cancelled save writes nothing and reports G_IO_ERROR_CANCELLED.
 * @callback: (nullable): invoked in the caller's thread-default main context.
 * @user_data: passed to @callback.
 *
 * Takes a snapshot of the list on the calling thread (at most
 * STK_WATCHLIST_MAX_ENTRIES small entries) and serialises + writes it on a
 * worker thread; the calling thread never touches the disk. Only one save may
 * run at a time: if one is already running, @callback receives
 * G_IO_ERROR_PENDING (from the main loop, never synchronously) and nothing
 * else changes, so the caller should retry later. Saves unconditionally, even
 * when the list is clean. On success the dirty flag is cleared before
 * @callback runs, unless an edit happened after the snapshot was taken (the
 * list then stays dirty and needs another save). The list is kept alive until
 * the save finishes.
 */
void stk_watchlist_save_async(StkWatchlist *self, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data);

/**
 * stk_watchlist_save_finish:
 * @self: the list.
 * @result: the result passed to the callback of stk_watchlist_save_async().
 * @error: (nullable): return location for a GError.
 *
 * Returns: TRUE if the file was replaced.
 */
gboolean stk_watchlist_save_finish(StkWatchlist *self, GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* STK_WATCHLIST_H */
