/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-watchlist.h - the user's ordered list of tracked instruments, persisted
 * as JSON.
 *
 * Threading: a StkWatchlist has no locking: at any moment it must be used by
 * ONE thread only, normally the main thread. Two exceptions exist:
 *  - the start-up load: src/ui/stk-app.c load_thread() calls
 *    stk_watchlist_load() on a GTask worker thread. That is correct only
 *    because nobody else touches the list until the task has completed back
 *    on the main thread (the hand-off). Consequently the "changed" signal
 *    that load() emits is emitted on that worker thread; the application
 *    connects its own handler only after the load has completed, so none of
 *    its handlers runs on the worker, but any handler connected earlier would;
 *  - the internals of stk_watchlist_save_async()'s worker, which touch only a
 *    private snapshot of the entries and the (immutable) file path.
 * Every other function must be called from the thread that owns the list.
 * stk_watchlist_load() and
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
/* Declared by G_DECLARE_FINAL_TYPE below (GLib gtype.h): the StkWatchlist typedef, the
 * StkWatchlistClass struct, GType stk_watchlist_get_type(void) (defined by G_DEFINE_TYPE() in
 * stk-watchlist.c, see there), the static inline casts STK_WATCHLIST() and STK_IS_WATCHLIST(),
 * and g_autoptr() support for StkWatchlist and StkWatchlistClass. STK_TYPE_WATCHLIST above is
 * hand-written. */
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
 *
 * Thread affinity: emitted in the thread that runs the emitting call; for
 * load() that is the start-up worker thread (see the file header).
 */

/**
 * stk_watchlist_new:
 * @path: (nullable): file used by load()/save(); NULL selects
 *   g_get_user_config_dir()/stocks/watchlist.json. The string is copied.
 *
 * Preconditions:
 *  - @path is NULL, or a non-empty NUL-terminated string (an empty string makes later
 *    load/save/save_async calls fail without setting a #GError; see stk_file_write_atomic()).
 * Postconditions:
 *  - the list is empty and clean; no file is read or created.
 * Thread affinity: any thread; the creating thread owns the list until it is
 *   handed over.
 * Ownership: @path is borrowed and copied; the caller owns the returned list.
 *
 * Returns: (transfer full): a new empty list. Nothing is read from disk until
 *   stk_watchlist_load() is called.
 */
StkWatchlist *stk_watchlist_new(const char *path);

/**
 * stk_watchlist_get_path:
 * @self: (not nullable): the list; must satisfy STK_IS_WATCHLIST().
 *
 * Preconditions:
 *  - @self is a valid list (else: critical, NULL returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the list.
 * Ownership: the returned string is owned by @self and lives as long as it.
 *
 * Returns: (transfer none): the file path used by load()/save().
 */
const char *stk_watchlist_get_path(StkWatchlist *self);

/**
 * stk_watchlist_get_n_entries:
 * @self: (not nullable): the list; must satisfy STK_IS_WATCHLIST().
 *
 * Preconditions:
 *  - @self is a valid list (else: critical, 0 returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the list.
 * Ownership: none (no pointers returned).
 *
 * Returns: the number of entries (at most STK_WATCHLIST_MAX_ENTRIES).
 */
guint stk_watchlist_get_n_entries(StkWatchlist *self);

/**
 * stk_watchlist_get_entry:
 * @self: (not nullable): the list; must satisfy STK_IS_WATCHLIST().
 * @index: zero-based position.
 *
 * Preconditions:
 *  - @self is a valid list (else: critical, NULL returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the list.
 * Ownership: the entry is owned by the list; the caller must not free or
 *   modify it.
 *
 * Returns: (transfer none) (nullable): the entry, or NULL if @index is out of
 *   range. Valid until the entry is removed or the list is reloaded (move() and add() keep it).
 */
const StkWatchlistEntry *stk_watchlist_get_entry(StkWatchlist *self, guint index);

/**
 * stk_watchlist_index_of:
 * @self: (not nullable): the list; must satisfy STK_IS_WATCHLIST().
 * @symbol: (nullable): ticker; compared ASCII-case-insensitively.
 *
 * Preconditions:
 *  - @self is a valid list (else: critical, -1 returned); @symbol is NULL,
 *    NUL-terminated, or at least 16 readable bytes (stk_symbol_is_valid()
 *    reads at most 16 bytes).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the list.
 * Ownership: @symbol is borrowed for the call only.
 *
 * Returns: the position of @symbol, or -1 if absent or @symbol is not a valid
 *   symbol (stk_symbol_is_valid()).
 */
gint stk_watchlist_index_of(StkWatchlist *self, const char *symbol);

/**
 * stk_watchlist_contains:
 * @self: (not nullable): the list; must satisfy STK_IS_WATCHLIST().
 * @symbol: (nullable): ticker; compared ASCII-case-insensitively.
 *
 * Preconditions:
 *  - as for stk_watchlist_index_of() (an invalid @self gives a critical and
 *    FALSE).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the list.
 * Ownership: @symbol is borrowed for the call only.
 *
 * Returns: TRUE iff stk_watchlist_index_of() would return >= 0.
 */
gboolean stk_watchlist_contains(StkWatchlist *self, const char *symbol);

/**
 * stk_watchlist_add:
 * @self: (not nullable): the list; must satisfy STK_IS_WATCHLIST().
 * @symbol: (nullable): ticker; must satisfy stk_symbol_is_valid(). Stored
 *   ASCII-upper-cased.
 * @name: (nullable): display name; NULL means "". Must be valid UTF-8 of at
 *   most STK_WATCHLIST_MAX_NAME_BYTES bytes. Copied.
 * @kind: a valid StkKind.
 *
 * Appends a new entry at the end. Emits "changed" on success.
 *
 * Preconditions:
 *  - @self is a valid list (else: critical, FALSE returned); @symbol and
 *    @name are NULL or NUL-terminated.
 * Postconditions:
 *  - on TRUE: the entry is the last one, the list is dirty and "changed" has
 *    been emitted once.
 *  - on FALSE: the list is unchanged, nothing is emitted.
 * Thread affinity: the thread that owns the list.
 * Ownership: @symbol and @name are borrowed and copied.
 *
 * Returns: TRUE if added. FALSE, with the list unchanged, if the symbol is
 *   invalid, already present (case-insensitively), the name is invalid or too
 *   long, @kind is invalid, or the list holds STK_WATCHLIST_MAX_ENTRIES.
 */
gboolean stk_watchlist_add(StkWatchlist *self, const char *symbol, const char *name,
                           StkKind kind);

/**
 * stk_watchlist_remove:
 * @self: (not nullable): the list; must satisfy STK_IS_WATCHLIST().
 * @symbol: (nullable): ticker; compared ASCII-case-insensitively.
 *
 * Removes the entry with that symbol. Emits "changed" on success.
 *
 * Preconditions:
 *  - @self is a valid list (else: critical, FALSE returned); @symbol is NULL,
 *    NUL-terminated, or at least 16 readable bytes.
 * Postconditions:
 *  - on TRUE: the entry is freed (pointers from get_entry() are invalid), the
 *    entries after it shift down by one, the list is dirty and "changed" has
 *    been emitted once.
 *  - on FALSE: the list is unchanged, nothing is emitted.
 * Thread affinity: the thread that owns the list.
 * Ownership: @symbol is borrowed for the call only.
 *
 * Returns: TRUE if an entry was removed; FALSE if @symbol was not present (or
 *   is not a valid symbol).
 */
gboolean stk_watchlist_remove(StkWatchlist *self, const char *symbol);

/**
 * stk_watchlist_move:
 * @self: (not nullable): the list; must satisfy STK_IS_WATCHLIST().
 * @from: current zero-based position of the entry.
 * @to: zero-based position the entry has afterwards; the entries in between
 *   shift by one.
 *
 * Moves one entry. Emits "changed" on success.
 *
 * Preconditions:
 *  - @self is a valid list (else: critical, FALSE returned).
 * Postconditions:
 *  - on TRUE: the entry is at index @to, the entries in between shifted by
 *    one, the list is dirty and "changed" has been emitted once; pointers
 *    from get_entry() still point to the same entries but at other indices.
 *  - on FALSE: the list is unchanged, nothing is emitted.
 * Thread affinity: the thread that owns the list.
 * Ownership: none (no pointers).
 *
 * Returns: TRUE if the order changed; FALSE (list unchanged) if either index
 *   is out of range or @from == @to.
 */
gboolean stk_watchlist_move(StkWatchlist *self, guint from, guint to);

/**
 * stk_watchlist_load:
 * @self: (not nullable): the list; must satisfy STK_IS_WATCHLIST().
 * @n_dropped: (out) (optional): receives how many stored entries were
 *   discarded (invalid symbol, kind, name or shape; duplicates; entries beyond
 *   STK_WATCHLIST_MAX_ENTRIES). Set to 0 first, right after the checks of @self and @error, so
 *   it is 0 after every failure except those two argument checks, which leave it untouched.
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
 * Known limit (not enforced): the file is bounded only by
 * STK_WATCHLIST_MAX_FILE_BYTES (4 MiB), and json-glib builds a node tree for
 * the whole text before any entry is validated, so a hostile 4 MiB file of
 * tiny elements costs memory proportional to its element count for the
 * duration of the load. No further cap is applied.
 *
 * Preconditions:
 *  - @self is a valid list (else: critical, FALSE returned and @n_dropped is
 *    left untouched); @error is NULL or points to a NULL #GError pointer
 *    (else: critical, FALSE, @n_dropped untouched); the stored path is non-empty
 *    (guaranteed unless stk_watchlist_new() was given "", which violates its precondition:
 *    the call then logs at least one critical, returns FALSE, leaves @error untouched and
 *    sets *@n_dropped to 0; the list is emptied and clean, and "changed" is emitted once if
 *    it was non-empty).
 *  - the caller owns the list (see the file header: at start-up this is the
 *    GTask worker thread that the application hands the list to).
 * Postconditions:
 *  - on TRUE: the list holds the valid entries, *@n_dropped is the number
 *    of dropped entries, the list is clean; "changed" has been emitted once if
 *    the list was or became non-empty (in the calling thread, which may be
 *    that worker thread).
 *  - on FALSE: see above; the list is empty (except the PENDING case).
 * Thread affinity: the thread that owns the list; the application calls it
 *   once at start-up on a GTask worker thread (src/ui/stk-app.c load_thread).
 * Ownership: @n_dropped and @error are filled for the caller; nothing is
 *   retained.
 *
 * Returns: TRUE if the file was loaded (possibly with dropped entries).
 */
gboolean stk_watchlist_load(StkWatchlist *self, guint *n_dropped, GError **error);

/**
 * stk_watchlist_is_dirty:
 * @self: (not nullable): the list; must satisfy STK_IS_WATCHLIST().
 *
 * Preconditions:
 *  - @self is a valid list (else: critical, FALSE returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the list.
 * Ownership: none (no pointers).
 *
 * Returns: TRUE iff add()/remove()/move() changed the list since the last
 *   stk_watchlist_load() (successful or not) or successful save. A save that
 *   was started before the latest edit does not clear the flag.
 */
gboolean stk_watchlist_is_dirty(StkWatchlist *self);

/**
 * stk_watchlist_save:
 * @self: (not nullable): the list; must satisfy STK_IS_WATCHLIST().
 * @error: (nullable): return location for a GError (G_FILE_ERROR domain, or
 *   G_IO_ERROR / G_IO_ERROR_PENDING).
 *
 * Writes the list with stk_file_write_atomic(): on failure the previous file
 * is untouched. SYNCHRONOUS, includes fsync: for start-up, shutdown and tests;
 * the UI must use stk_watchlist_save_async(). Clears the dirty flag on
 * success. Fails with G_IO_ERROR_PENDING (nothing written) while an
 * asynchronous save is running.
 *
 * Preconditions:
 *  - @self is a valid list and is owned by the calling thread; @error is NULL
 *    or points to a NULL #GError pointer (else: critical, FALSE); the stored path is
 *    non-empty (guaranteed unless stk_watchlist_new() was given "", which violates its
 *    precondition: at least one critical is logged, FALSE is returned, @error is untouched).
 * Postconditions:
 *  - on TRUE: the file holds the list and the list is clean.
 *  - on FALSE: @error (if non-NULL) is set; the dirty flag and the previous
 *    file are unchanged.
 * Thread affinity: the thread that owns the list.
 * Ownership: @error is filled for the caller; nothing is retained.
 *
 * Returns: TRUE if the file was replaced.
 */
gboolean stk_watchlist_save(StkWatchlist *self, GError **error);

/**
 * stk_watchlist_save_async:
 * @self: (not nullable): the list; must satisfy STK_IS_WATCHLIST().
 * @cancellable: (nullable): honoured only until the write starts; a
 *   cancelled save writes nothing and reports G_IO_ERROR_CANCELLED.
 * @callback: (nullable) (scope async): invoked in the thread-default main
 *   context of the calling thread.
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
 *
 * Preconditions:
 *  - @self is a valid list owned by the calling thread (else: critical,
 *    nothing happens and @callback is never called); the stored path is non-empty
 *    (guaranteed unless stk_watchlist_new() was given "", which violates its precondition:
 *    the worker then fails without producing a #GError, GLib logs at least one critical and
 *    @callback is never called); the calling thread
 *    iterates a main context that will run @callback.
 * Postconditions:
 *  - exactly one call of @callback (if given) follows, from the main loop,
 *    with a result for stk_watchlist_save_finish(); the worker touches only
 *    the private snapshot and the immutable path of @self.
 * Thread affinity: the thread that owns the list.
 * Ownership: the task holds a reference on @self until the callback has run;
 *   @cancellable is referenced by the task; @user_data is passed through
 *   untouched and stays owned by the caller.
 */
void stk_watchlist_save_async(StkWatchlist *self, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data);

/**
 * stk_watchlist_save_finish:
 * @self: (not nullable): the list; must satisfy STK_IS_WATCHLIST().
 * @result: (not nullable): the result passed to the callback of
 *   stk_watchlist_save_async().
 * @error: (nullable): return location for a GError.
 *
 * Collects the outcome of stk_watchlist_save_async().
 *
 * Preconditions:
 *  - @self is a valid list; @result was produced by
 *    stk_watchlist_save_async() on @self (else: critical, FALSE); call it
 *    once, in the callback.
 * Postconditions:
 *  - on FALSE after a runtime failure: @error (if non-NULL) is set (G_IO_ERROR
 *    / G_IO_ERROR_PENDING or _CANCELLED, or G_FILE_ERROR_*).
 * Thread affinity: the thread that runs the callback (the thread that called
 *   stk_watchlist_save_async()).
 * Ownership: @result stays owned by the caller (the async machinery); @error
 *   is filled for the caller.
 *
 * Returns: TRUE if the file was replaced.
 */
gboolean stk_watchlist_save_finish(StkWatchlist *self, GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* STK_WATCHLIST_H */
