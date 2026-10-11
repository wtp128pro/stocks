/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
#include "stk-watchlist.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "stk-fileio.h"

#define WATCHLIST_VERSION 1

/* Size of a buffer for an upper-cased symbol: the longest symbol accepted by
 * stk_symbol_is_valid() is 15 bytes, plus the NUL. */
#define SYMBOL_BUF_LEN 16
G_STATIC_ASSERT(SYMBOL_BUF_LEN >= 15 + 1);

struct _StkWatchlist {
  GObject parent_instance;
  char *path;
  GPtrArray *entries; /* StkWatchlistEntry*, freed with entry_free */
  guint64 gen;        /* bumped by every successful add/remove/move */
  guint64 saved_gen;  /* gen at the last load (successful or not) or successful save snapshot */
  gboolean save_in_flight;
};

/* Source tag of stk_watchlist_save_async() tasks; only its address is used. */
static char save_tag;

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/*
 * G_DEFINE_TYPE(StkWatchlist, stk_watchlist, G_TYPE_OBJECT) generates the following (checked with
 * gcc -E of this file with the project's compile flags):
 *  - GType stk_watchlist_get_type(void): returns the GType of StkWatchlist and registers it on the
 *    first call, inside a once-initialisation guard, by calling stk_watchlist_get_type_once().
 *    Thread affinity: any thread. Ownership: none (no pointers).
 *  - static GType stk_watchlist_get_type_once(void): performs the registration for
 *    stk_watchlist_get_type().
 *    Thread affinity: only via stk_watchlist_get_type(). Ownership: none (no pointers).
 *  - static void stk_watchlist_class_intern_init(gpointer klass): @klass is the class structure
 *    being initialised (borrowed, written; nothing is retained). It stores the parent class in
 *    stk_watchlist_parent_class, adjusts the private offset only if StkWatchlist_private_offset
 *    is non-zero, then calls stk_watchlist_class_init(). Called by GObject once per type, from
 *    the thread that first uses the type. Ownership: @klass belongs to GObject.
 *  - static gpointer stk_watchlist_parent_class: NULL until class_intern_init() runs; used by
 *    finalize() to chain up.
 *  - static gint StkWatchlist_private_offset: stays 0 (no G_ADD_PRIVATE is used).
 *  - static inline gpointer stk_watchlist_get_instance_private(StkWatchlist *self): returns self
 *    plus the private offset; unused here (G_GNUC_UNUSED). @self is borrowed. Thread affinity:
 *    any thread.
 */
G_DEFINE_TYPE(StkWatchlist, stk_watchlist, G_TYPE_OBJECT)

/**
 * entry_free:
 * @data: (not nullable): a StkWatchlistEntry created by append_entry() or by
 *   stk_watchlist_save_async()'s snapshot copy loop.
 *
 * GDestroyNotify of the entry arrays: frees the entry and its two strings.
 *
 * Preconditions:
 *  - @data is a heap-allocated StkWatchlistEntry whose symbol and name are
 *    g_malloc()ed strings (or NULL); the call sites are the GPtrArray free
 *    functions, which pass each element exactly once. @data is dereferenced
 *    and so must not be NULL.
 * Postconditions:
 *  - the entry and both strings are freed.
 * Thread affinity: any thread (the entry is owned by the freeing array; the
 *   snapshot array may be freed on the worker or the main thread).
 * Ownership: consumes @data.
 */
static void
entry_free(gpointer data)
{
  StkWatchlistEntry *e = data;
  g_free(e->symbol);
  g_free(e->name);
  g_free(e);
}

/**
 * stk_watchlist_finalize:
 * @object: the StkWatchlist being finalized.
 *
 * GObject finalize: frees the path and the entry array, then chains up.
 *
 * Preconditions:
 *  - @object is a StkWatchlist whose last reference was dropped; no save is
 *    running (the save tasks hold a reference on the list).
 * Postconditions:
 *  - all memory owned by the list is freed; the parent finalize has run.
 * Thread affinity: the thread that drops the last reference.
 * Ownership: the list owns its path and entries; both are released here.
 */
static void
stk_watchlist_finalize(GObject *object)
{
  StkWatchlist *self = STK_WATCHLIST(object);
  g_free(self->path);
  g_ptr_array_unref(self->entries);
  G_OBJECT_CLASS(stk_watchlist_parent_class)->finalize(object);
}

/**
 * stk_watchlist_class_init:
 * @klass: the class structure being initialised.
 *
 * GObject class_init: installs finalize and registers the "changed" signal
 * (no parameters, run last).
 *
 * Preconditions:
 *  - called once by the GType system.
 * Postconditions:
 *  - the finalize vfunc is set and signals[SIGNAL_CHANGED] is registered.
 * Thread affinity: the thread that first uses the type (GType serialises it).
 * Ownership: none (the class is owned by GType).
 */
static void
stk_watchlist_class_init(StkWatchlistClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = stk_watchlist_finalize;
  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
                                         G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
}

/**
 * stk_watchlist_init:
 * @self: the instance being initialised.
 *
 * GObject instance init: creates the empty entry array.
 *
 * Preconditions:
 *  - @self is a zero-initialised new instance.
 * Postconditions:
 *  - entries is an empty array that frees its elements with entry_free();
 *    path is still NULL (set by stk_watchlist_new()).
 * Thread affinity: the creating thread.
 * Ownership: the instance owns the new array.
 */
static void
stk_watchlist_init(StkWatchlist *self)
{
  self->entries = g_ptr_array_new_with_free_func(entry_free);
}

/* Documented in stk-watchlist.h. */
StkWatchlist *
stk_watchlist_new(const char *path)
{
  StkWatchlist *self = g_object_new(STK_TYPE_WATCHLIST, NULL);
  self->path = path != NULL
                 ? g_strdup(path)
                 : g_build_filename(g_get_user_config_dir(), "stocks", "watchlist.json", NULL);
  return self;
}

/* Documented in stk-watchlist.h. */
const char *
stk_watchlist_get_path(StkWatchlist *self)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), NULL);
  return self->path;
}

/* Documented in stk-watchlist.h. */
guint
stk_watchlist_get_n_entries(StkWatchlist *self)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), 0);
  return self->entries->len;
}

/* Documented in stk-watchlist.h. */
const StkWatchlistEntry *
stk_watchlist_get_entry(StkWatchlist *self, guint index)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), NULL);
  return index < self->entries->len ? g_ptr_array_index(self->entries, index) : NULL;
}

/**
 * upcase_symbol:
 * @symbol: (not nullable): ticker.
 * @out: (out) (array fixed-size=16): buffer of SYMBOL_BUF_LEN bytes. (A C
 *   array parameter decays to a pointer, so the size is NOT visible to the
 *   compiler here; it is enforced by the loop bound below.)
 *
 * Writes the ASCII-upper-cased @symbol and a NUL into @out. At most
 * SYMBOL_BUF_LEN - 1 bytes are copied, so @out can never be overrun even if
 * the symbol were longer; a longer symbol would be silently truncated.
 *
 * Preconditions:
 *  - @symbol is NUL-terminated and satisfies stk_symbol_is_valid() (1..15
 *    bytes; every call site validates it first, so the truncation cannot
 *    happen); @out has room for SYMBOL_BUF_LEN bytes (every call site passes
 *    a local char[SYMBOL_BUF_LEN]).
 * Postconditions:
 *  - @out holds the upper-cased symbol followed by a NUL; @symbol is not
 *    modified.
 * Thread affinity: any thread (touches only @out).
 * Ownership: both pointers are borrowed for the call only.
 */
static void
upcase_symbol(const char *symbol, char out[SYMBOL_BUF_LEN])
{
  gsize i = 0;
  for (; i < SYMBOL_BUF_LEN - 1 && symbol[i] != '\0'; i++)
    out[i] = g_ascii_toupper(symbol[i]);
  out[i] = '\0';
}

/**
 * find_upper:
 * @self: the list.
 * @upper: (not nullable): upper-case, valid symbol.
 *
 * Linear search for the entry whose symbol equals @upper (byte comparison;
 * both strings are NUL-terminated: entry symbols are g_strdup()ed validated
 * symbols and @upper was built by upcase_symbol()).
 *
 * Preconditions:
 *  - @self is a valid list; @upper is NUL-terminated.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the list.
 * Ownership: @upper is borrowed for the call only.
 *
 * Returns: the zero-based index of the match (at most
 *   STK_WATCHLIST_MAX_ENTRIES - 1, so it fits a gint), or -1 if absent.
 */
static gint
find_upper(StkWatchlist *self, const char *upper)
{
  for (guint i = 0; i < self->entries->len; i++) {
    const StkWatchlistEntry *e = g_ptr_array_index(self->entries, i);
    if (g_str_equal(e->symbol, upper))
      return (gint)i;
  }
  return -1;
}

/* Documented in stk-watchlist.h. */
gint
stk_watchlist_index_of(StkWatchlist *self, const char *symbol)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), -1);
  if (!stk_symbol_is_valid(symbol))
    return -1;
  char upper[SYMBOL_BUF_LEN];
  upcase_symbol(symbol, upper);
  return find_upper(self, upper);
}

/* Documented in stk-watchlist.h. */
gboolean
stk_watchlist_contains(StkWatchlist *self, const char *symbol)
{
  return stk_watchlist_index_of(self, symbol) >= 0;
}

/**
 * kind_is_valid:
 * @kind: any value.
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: TRUE iff @kind is one of the StkKind values (STK_KIND_INDEX is the
 *   last one).
 */
static gboolean
kind_is_valid(StkKind kind)
{
  return (guint)kind <= (guint)STK_KIND_INDEX;
}

/**
 * name_is_valid:
 * @name: (nullable): display name candidate.
 *
 * Checks the length bound with memchr() inside the first
 * STK_WATCHLIST_MAX_NAME_BYTES + 1 bytes (never scans further, equivalent to
 * a length check against the bound) and then UTF-8 validity.
 *
 * Preconditions:
 *  - @name is NULL or NUL-terminated (call site: append_entry() passes the
 *    caller's name or a JSON string).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: @name is borrowed for the call only.
 *
 * Returns: TRUE iff @name is non-NULL, at most STK_WATCHLIST_MAX_NAME_BYTES
 *   bytes long and valid UTF-8.
 */
static gboolean
name_is_valid(const char *name)
{
  return name != NULL && memchr(name, '\0', STK_WATCHLIST_MAX_NAME_BYTES + 1) != NULL &&
         g_utf8_validate(name, -1, NULL);
}

/**
 * append_entry:
 * @self: the list.
 * @symbol: (nullable): ticker to add.
 * @name: (nullable): display name; NULL means "".
 * @kind: kind of the instrument.
 *
 * Appends without emitting. Returns FALSE on any validation failure (invalid
 * symbol, kind or name, list full, duplicate).
 *
 * Preconditions:
 *  - @self is a valid list; @symbol and @name are NULL or NUL-terminated.
 * Postconditions:
 *  - on TRUE: a new entry (upper-cased symbol, copied name) is the last
 *    element and the generation counter is incremented;
 *    on FALSE: the list is unchanged.
 * Thread affinity: the thread that owns the list.
 * Ownership: @symbol and @name are borrowed and copied; the entry is owned by
 *   the list.
 *
 * Returns: TRUE if the entry was appended.
 */
static gboolean
append_entry(StkWatchlist *self, const char *symbol, const char *name, StkKind kind)
{
  if (!stk_symbol_is_valid(symbol) || !kind_is_valid(kind))
    return FALSE;
  if (name == NULL)
    name = "";
  if (!name_is_valid(name) || self->entries->len >= STK_WATCHLIST_MAX_ENTRIES)
    return FALSE;

  char upper[SYMBOL_BUF_LEN];
  upcase_symbol(symbol, upper);
  if (find_upper(self, upper) >= 0)
    return FALSE;
  StkWatchlistEntry *e = g_new(StkWatchlistEntry, 1);
  e->symbol = g_strdup(upper);
  e->name = g_strdup(name);
  e->kind = kind;
  e->range = STK_RANGE_1D;
  e->show_points = FALSE;
  g_ptr_array_add(self->entries, e);
  self->gen++;
  return TRUE;
}

/* Documented in stk-watchlist.h. */
gboolean
stk_watchlist_add(StkWatchlist *self, const char *symbol, const char *name, StkKind kind)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), FALSE);
  if (!append_entry(self, symbol, name, kind))
    return FALSE;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

/* Documented in stk-watchlist.h. */
gboolean
stk_watchlist_remove(StkWatchlist *self, const char *symbol)
{
  gint i = stk_watchlist_index_of(self, symbol);
  if (i < 0)
    return FALSE;
  g_ptr_array_remove_index(self->entries, (guint)i);
  self->gen++;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

/* Documented in stk-watchlist.h. */
gboolean
stk_watchlist_move(StkWatchlist *self, guint from, guint to)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), FALSE);
  guint n = self->entries->len;
  if (from >= n || to >= n || from == to)
    return FALSE;

  /* In bounds: from != to and both < n, so from + 1 <= to < n resp.
   * to + 1 <= from < n, and the byte counts are (|to - from|) pointers. */
  gpointer *pdata = self->entries->pdata;
  gpointer moved = pdata[from];
  if (from < to)
    memmove(&pdata[from], &pdata[from + 1], (to - from) * sizeof(gpointer));
  else
    memmove(&pdata[to + 1], &pdata[to], (from - to) * sizeof(gpointer));
  pdata[to] = moved;
  self->gen++;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

/* Documented in stk-watchlist.h. */
gboolean
stk_watchlist_set_view(StkWatchlist *self, const char *symbol, StkRange range, gboolean show_points)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), FALSE);
  if (range < STK_RANGE_1D || range > STK_RANGE_1Y)
    return FALSE;
  gint i = stk_watchlist_index_of(self, symbol);
  if (i < 0)
    return FALSE;
  StkWatchlistEntry *e = g_ptr_array_index(self->entries, (guint)i);
  show_points = !!show_points;
  if (e->range == range && e->show_points == show_points)
    return FALSE;
  e->range = range;
  e->show_points = show_points;
  self->gen++;
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Persistence                                                              */
/* ------------------------------------------------------------------------ */

/**
 * pending_error:
 *
 * Builds the error returned while an asynchronous save is running.
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - a new GError (G_IO_ERROR / G_IO_ERROR_PENDING) exists; nothing else
 *    changes.
 * Thread affinity: any thread (pure).
 * Ownership: no pointer parameters; the caller owns the returned error.
 *
 * Returns: (transfer full): the new error.
 */
static GError *
pending_error(void)
{
  return g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PENDING, "watchlist: an asynchronous save is running");
}

/**
 * fill_from_json:
 * @self: the list; must be empty.
 * @root: (nullable): root node of the parsed document.
 * @dropped: (not nullable) (inout): counter incremented once per dropped
 *   array element.
 * @error: (nullable): return location for a #GError, may be NULL.
 *
 * Fills @self from parsed JSON: checks the top-level object, the "version"
 * and the "entries" array and appends every valid element with
 * append_entry().
 *
 * Preconditions:
 *  - @self is empty; @root is NULL or a node owned by a live JsonParser;
 *    @dropped points to a local (call site: stk_watchlist_load()); @error is
 *    NULL or points to a NULL #GError pointer.
 * Postconditions:
 *  - on TRUE: every valid element has been appended in file order and
 *    *@dropped counts the others (no more than the array length, so no
 *    overflow).
 *  - on FALSE: @error (if non-NULL) is set (STK_ERROR / STK_ERROR_PARSE);
 *    nothing was appended; *@dropped is untouched.
 * Thread affinity: the thread that owns the list.
 * Ownership: @root is borrowed; nothing is retained.
 *
 * Returns: TRUE if the document has the expected shape and version.
 */
static gboolean
fill_from_json(StkWatchlist *self, JsonNode *root, guint *dropped, GError **error)
{
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "watchlist: top level is not an object");
    return FALSE;
  }
  JsonObject *obj = json_node_get_object(root);
  JsonNode *vnode = json_object_get_member(obj, "version");
  if (vnode == NULL || !JSON_NODE_HOLDS_VALUE(vnode) ||
      json_node_get_value_type(vnode) != G_TYPE_INT64) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "watchlist: missing or invalid version");
    return FALSE;
  }
  gint64 version = json_node_get_int(vnode);
  if (version != WATCHLIST_VERSION) {
    g_set_error(error, STK_ERROR, STK_ERROR_PARSE, "watchlist: unsupported version %" G_GINT64_FORMAT,
                version);
    return FALSE;
  }
  JsonNode *enode = json_object_get_member(obj, "entries");
  if (enode == NULL || !JSON_NODE_HOLDS_ARRAY(enode)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "watchlist: missing entries array");
    return FALSE;
  }

  JsonArray *arr = json_node_get_array(enode);
  guint n = json_array_get_length(arr);
  for (guint i = 0; i < n; i++) {
    JsonNode *item = json_array_get_element(arr, i);
    gboolean ok = FALSE;
    if (JSON_NODE_HOLDS_OBJECT(item)) {
      JsonObject *o = json_node_get_object(item);
      JsonNode *sym = json_object_get_member(o, "symbol");
      JsonNode *nm = json_object_get_member(o, "name");
      JsonNode *kd = json_object_get_member(o, "kind");
      StkKind kind;
      if (sym != NULL && nm != NULL && kd != NULL && JSON_NODE_HOLDS_VALUE(sym) &&
          JSON_NODE_HOLDS_VALUE(nm) && JSON_NODE_HOLDS_VALUE(kd) &&
          json_node_get_value_type(sym) == G_TYPE_STRING &&
          json_node_get_value_type(nm) == G_TYPE_STRING &&
          json_node_get_value_type(kd) == G_TYPE_STRING &&
          stk_kind_from_string(json_node_get_string(kd), &kind))
        ok = append_entry(self, json_node_get_string(sym), json_node_get_string(nm), kind);
      if (ok) {
        /* Optional view settings; a missing or malformed value keeps the default. */
        StkWatchlistEntry *last = g_ptr_array_index(self->entries, self->entries->len - 1);
        JsonNode *rg = json_object_get_member(o, "range");
        JsonNode *pt = json_object_get_member(o, "points");
        StkRange range;
        if (rg != NULL && JSON_NODE_HOLDS_VALUE(rg) &&
            json_node_get_value_type(rg) == G_TYPE_STRING &&
            stk_range_from_string(json_node_get_string(rg), &range))
          last->range = range;
        if (pt != NULL && JSON_NODE_HOLDS_VALUE(pt) && json_node_get_value_type(pt) == G_TYPE_BOOLEAN)
          last->show_points = json_node_get_boolean(pt);
      }
    }
    if (!ok)
      (*dropped)++;
  }
  return TRUE;
}

/* Documented in stk-watchlist.h. */
gboolean
stk_watchlist_load(StkWatchlist *self, guint *n_dropped, GError **error)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  if (n_dropped != NULL)
    *n_dropped = 0;
  if (self->save_in_flight) {
    g_propagate_error(error, pending_error());
    return FALSE;
  }

  gboolean had_entries = self->entries->len > 0;
  g_ptr_array_set_size(self->entries, 0);

  stk_file_remove_stale_temps(self->path);
  g_autofree char *text = NULL;
  gsize len = 0;
  gboolean ok = stk_file_read_bounded(self->path, STK_WATCHLIST_MAX_FILE_BYTES, &text, &len, error);
  guint dropped = 0;
  if (ok && !stk_json_check_strict(text, len, STK_JSON_MAX_DEPTH)) {
    g_set_error(error, STK_ERROR, STK_ERROR_PARSE,
                "watchlist: not strict JSON or nested deeper than %d", STK_JSON_MAX_DEPTH);
    ok = FALSE;
  }
  if (ok) {
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) perr = NULL;
    if (!json_parser_load_from_data(parser, text, (gssize)len, &perr)) {
      g_set_error(error, STK_ERROR, STK_ERROR_PARSE, "watchlist: %s", perr->message);
      ok = FALSE;
    } else {
      ok = fill_from_json(self, json_parser_get_root(parser), &dropped, error);
    }
  }

  if (!ok)
    g_ptr_array_set_size(self->entries, 0);
  else if (n_dropped != NULL)
    *n_dropped = dropped;
  self->saved_gen = self->gen;
  if (had_entries || self->entries->len > 0)
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return ok;
}

/**
 * serialize_entries:
 * @entries: (element-type StkWatchlistEntry): the entries to write.
 * @len: (not nullable) (out): receives the document length in bytes.
 *
 * Serialises @entries (StkWatchlistEntry*). Strings go through
 * stk_json_append_string(), never JsonGenerator, so that no control byte is
 * ever written raw (the loader rejects those). Size: at most
 * STK_WATCHLIST_MAX_ENTRIES entries of < 2 KiB (a 256-byte name escapes to at
 * most 1536 bytes), far below STK_WATCHLIST_MAX_FILE_BYTES. The initial
 * allocation hint is computed in gsize; entries->len is at most
 * STK_WATCHLIST_MAX_ENTRIES because the list was filled only through append_entry()
 * (which enforces the cap) and the snapshot is an element-wise copy of it made in
 * stk_watchlist_save_async().
 *
 * Preconditions:
 *  - @entries holds valid entries (valid symbol, kind and name) and is not
 *    modified meanwhile (the worker uses the private snapshot).
 * Postconditions:
 *  - no state is changed; *@len is the length of the returned text.
 * Thread affinity: any thread (touches only @entries and the result).
 * Ownership: @entries is borrowed; the caller owns the returned buffer.
 *
 * Returns: (transfer full): the NUL-terminated document; free with g_free().
 */
static char *
serialize_entries(const GPtrArray *entries, gsize *len)
{
  GString *out = g_string_sized_new(64 + (gsize)entries->len * 128);
  g_string_append_printf(out, "{\n  \"version\": %d,\n  \"entries\": [", WATCHLIST_VERSION);
  for (guint i = 0; i < entries->len; i++) {
    const StkWatchlistEntry *e = g_ptr_array_index(entries, i);
    g_string_append(out, i > 0 ? ",\n    {\"symbol\": " : "\n    {\"symbol\": ");
    stk_json_append_string(out, e->symbol);
    g_string_append(out, ", \"name\": ");
    stk_json_append_string(out, e->name);
    g_string_append(out, ", \"kind\": ");
    stk_json_append_string(out, stk_kind_to_string(e->kind));
    g_string_append(out, ", \"range\": ");
    stk_json_append_string(out, stk_range_to_string(e->range));
    g_string_append(out, e->show_points ? ", \"points\": true" : ", \"points\": false");
    g_string_append_c(out, '}');
  }
  g_string_append(out, entries->len > 0 ? "\n  ]\n}\n" : "]\n}\n");
  *len = out->len;
  return g_string_free(out, FALSE);
}

/* Documented in stk-watchlist.h. */
gboolean
stk_watchlist_is_dirty(StkWatchlist *self)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), FALSE);
  return self->gen != self->saved_gen;
}

/* Documented in stk-watchlist.h. */
gboolean
stk_watchlist_save(StkWatchlist *self, GError **error)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  if (self->save_in_flight) {
    g_propagate_error(error, pending_error());
    return FALSE;
  }

  gsize len = 0;
  g_autofree char *text = serialize_entries(self->entries, &len);
  if (!stk_file_write_atomic(self->path, text, len, error))
    return FALSE;
  self->saved_gen = self->gen;
  return TRUE;
}

/* Data of one asynchronous save; owned by the inner GTask as task data. */
typedef struct {
  GPtrArray *entries; /* deep copy, owned */
  guint64 gen;
  GTask *outer; /* the task handed to the caller, completed in save_done() */
} SaveJob;

/**
 * save_job_free:
 * @job: the job to free.
 *
 * GDestroyNotify of the inner task's data: frees the snapshot copy, drops the
 * reference on the outer task and frees the job.
 *
 * Preconditions:
 *  - @job was created by stk_watchlist_save_async() and is freed exactly once
 *    (by the inner GTask, after both save_worker() and save_done() are done
 *    with it).
 * Postconditions:
 *  - the snapshot, the job and the reference on the outer task are released.
 * Thread affinity: any thread (the inner task may be finalised on the worker
 *   or on the thread that ran save_done()).
 * Ownership: consumes @job and its snapshot; releases the outer task.
 */
static void
save_job_free(SaveJob *job)
{
  g_ptr_array_unref(job->entries);
  g_object_unref(job->outer);
  g_free(job);
}

/**
 * save_worker:
 * @task: the inner task.
 * @source: (type StkWatchlist): the list the task was created for.
 * @task_data: the SaveJob.
 * @cancellable: (nullable): unused; cancellation is read through @task.
 *
 * GTaskThreadFunc: serialises the private snapshot and writes it atomically
 * to the list's path. Returns the cancellation error instead if the task was
 * cancelled before it started.
 *
 * Preconditions:
 *  - runs on a GTask worker thread; @source is kept alive by @task; its path
 *    is never modified after construction (so reading it here is race free);
 *    @task_data is the SaveJob that nothing else mutates meanwhile.
 * Postconditions:
 *  - @task is completed exactly once: TRUE, the cancellation error or the
 *    error of stk_file_write_atomic().
 * Thread affinity: worker thread only.
 * Ownership: @task_data stays owned by the task; the error from the write is
 *   handed to the task.
 */
static void
save_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  (void)cancellable;
  if (g_task_return_error_if_cancelled(task))
    return;
  const SaveJob *job = task_data;
  gsize len = 0;
  g_autofree char *text = serialize_entries(job->entries, &len);
  GError *error = NULL;
  if (stk_file_write_atomic(STK_WATCHLIST(source)->path, text, len, &error))
    g_task_return_boolean(task, TRUE);
  else
    g_task_return_error(task, error);
}

/**
 * save_done:
 * @source: (type StkWatchlist): the list.
 * @result: the finished inner task.
 * @user_data: unused.
 *
 * GAsyncReadyCallback of the inner task, run in the main context of the
 * thread that called stk_watchlist_save_async(): clears save_in_flight, marks
 * the list saved as of the snapshot generation on success, and completes the
 * caller's outer task with the same outcome.
 *
 * Preconditions:
 *  - @source is the list and @result its inner task, whose task data is the
 *    SaveJob; runs in the thread that owns the list.
 * Postconditions:
 *  - save_in_flight is FALSE; on success saved_gen is the generation of the
 *    snapshot (the list stays dirty if edited since); the outer task has
 *    been completed once.
 * Thread affinity: the thread that owns the list (main thread).
 * Ownership: the error obtained from the inner task is handed to the outer
 *   task; nothing else is retained.
 */
static void
save_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)user_data;
  StkWatchlist *self = STK_WATCHLIST(source);
  GTask *inner = G_TASK(result);
  const SaveJob *job = g_task_get_task_data(inner);
  GTask *outer = job->outer;
  guint64 gen = job->gen;

  GError *error = NULL;
  gboolean ok = g_task_propagate_boolean(inner, &error);
  self->save_in_flight = FALSE;
  if (ok) {
    self->saved_gen = gen;
    g_task_return_boolean(outer, TRUE);
  } else {
    g_task_return_error(outer, error);
  }
}

/* Documented in stk-watchlist.h. */
void
stk_watchlist_save_async(StkWatchlist *self, GCancellable *cancellable, GAsyncReadyCallback callback,
                         gpointer user_data)
{
  g_return_if_fail(STK_IS_WATCHLIST(self));

  GTask *outer = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(outer, &save_tag);
  /* A cancellation that arrives after the write started must not turn a
   * successful write into an error: only save_worker() honours it. */
  g_task_set_check_cancellable(outer, FALSE);
  if (self->save_in_flight) {
    g_task_return_error(outer, pending_error());
    g_object_unref(outer);
    return;
  }

  self->save_in_flight = TRUE;
  SaveJob *job = g_new0(SaveJob, 1);
  job->entries = g_ptr_array_new_full(self->entries->len, entry_free);
  for (guint i = 0; i < self->entries->len; i++) {
    const StkWatchlistEntry *src = g_ptr_array_index(self->entries, i);
    StkWatchlistEntry *copy = g_new(StkWatchlistEntry, 1);
    copy->symbol = g_strdup(src->symbol);
    copy->name = g_strdup(src->name);
    copy->kind = src->kind;
    copy->range = src->range;
    copy->show_points = src->show_points;
    g_ptr_array_add(job->entries, copy);
  }
  job->gen = self->gen;
  job->outer = outer;
  GTask *inner = g_task_new(self, cancellable, save_done, NULL);
  g_task_set_check_cancellable(inner, FALSE);
  g_task_set_task_data(inner, job, (GDestroyNotify)save_job_free);
  g_task_run_in_thread(inner, save_worker);
  g_object_unref(inner);
}

/* Documented in stk-watchlist.h. */
gboolean
stk_watchlist_save_finish(StkWatchlist *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  g_return_val_if_fail(g_async_result_is_tagged(result, &save_tag), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
