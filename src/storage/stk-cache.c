/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
#include "stk-cache.h"

#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

#include "stk-fileio.h"

#define CACHE_VERSION 1
#define N_RANGES ((guint)STK_RANGE_1Y + 1)
#define MAX_CURRENCY_BYTES 8

/* Size of a buffer for an upper-cased symbol: the longest symbol accepted by
 * stk_symbol_is_valid() is 15 bytes, plus the NUL. */
#define SYMBOL_BUF_LEN 16
G_STATIC_ASSERT(SYMBOL_BUF_LEN >= 15 + 1);

/* Source tag of stk_cache_save_async() tasks; only its address is used. */
static char save_tag;

typedef struct {
  StkSeries *series; /* NULL when absent */
  gint64 fetched_ms;
} SeriesSlot;

/* Per-symbol data. Also used (without lru_link) for save snapshots. */
typedef struct {
  char *symbol; /* upper-case, valid */
  StkQuote *quote; /* NULL when absent */
  gint64 quote_ms;
  SeriesSlot slots[N_RANGES];
  GList *lru_link; /* link in StkCache.lru; NULL in snapshots */
  gsize n_points;  /* sum of the slots' n_points */
} Entry;

typedef struct {
  guint64 gen;
  GPtrArray *entries; /* Entry*, least recently used first */
  gsize n_points;
} Snapshot;

struct _StkCache {
  GObject parent_instance;
  char *path;
  GHashTable *by_symbol; /* symbol (owned by Entry) -> Entry*, owns the Entry */
  GQueue lru;            /* Entry*; head = least recently used */
  gsize total_points;
  guint64 gen;       /* bumped by every successful put */
  guint64 saved_gen; /* gen captured by the last successful load/save snapshot */
  gboolean save_in_flight;
};

/*
 * G_DEFINE_TYPE(StkCache, stk_cache, G_TYPE_OBJECT) generates the following (checked with gcc -E
 * of this file with the project's compile flags):
 *  - GType stk_cache_get_type(void): returns the GType of StkCache and registers it on the first
 *    call, inside a once-initialisation guard, by calling stk_cache_get_type_once().
 *    Thread affinity: any thread. Ownership: none (no pointers).
 *  - static GType stk_cache_get_type_once(void): performs the registration for
 *    stk_cache_get_type().
 *    Thread affinity: only via stk_cache_get_type(). Ownership: none (no pointers).
 *  - static void stk_cache_class_intern_init(gpointer klass): @klass is the class structure being
 *    initialised (borrowed, written; nothing is retained). It stores the parent class in
 *    stk_cache_parent_class, adjusts the private offset only if StkCache_private_offset is
 *    non-zero, then calls stk_cache_class_init(). Called by GObject once per type, from the
 *    thread that first uses the type. Ownership: @klass belongs to GObject.
 *  - static gpointer stk_cache_parent_class: NULL until class_intern_init() runs; used by
 *    finalize() to chain up.
 *  - static gint StkCache_private_offset: stays 0 (no G_ADD_PRIVATE is used).
 *  - static inline gpointer stk_cache_get_instance_private(StkCache *self): returns self plus the
 *    private offset; unused here (G_GNUC_UNUSED). @self is borrowed. Thread affinity: any
 *    thread.
 */
G_DEFINE_TYPE(StkCache, stk_cache, G_TYPE_OBJECT)

/* ------------------------------------------------------------------------ */
/* Entries and LRU                                                          */
/* ------------------------------------------------------------------------ */

/**
 * entry_free:
 * @data: an Entry created by find_or_create() or snapshot_take().
 *
 * GDestroyNotify of the symbol table and of the snapshot array: drops the
 * series references, frees the quote, the symbol and the entry.
 *
 * Preconditions:
 *  - @data is a non-NULL, heap-allocated Entry owned by the caller (the
 *    container that frees it passes each element exactly once); its series
 *    slots are NULL or hold one reference each.
 * Postconditions:
 *  - the entry and everything it owns is released.
 * Thread affinity: any thread (the snapshot may be freed on the worker or on
 *   the main thread; series references are atomic).
 * Ownership: consumes @data.
 */
static void
entry_free(gpointer data)
{
  Entry *e = data;
  for (guint i = 0; i < N_RANGES; i++)
    if (e->slots[i].series != NULL)
      stk_series_unref(e->slots[i].series);
  stk_quote_free(e->quote);
  g_free(e->symbol);
  g_free(e);
}

/**
 * stk_cache_finalize:
 * @object: the StkCache being finalized.
 *
 * GObject finalize: clears the LRU queue, releases the symbol table (which
 * frees the entries) and the path, then chains up.
 *
 * Preconditions:
 *  - @object is a StkCache whose last reference was dropped; no save is
 *    running (the save tasks hold a reference on the cache).
 * Postconditions:
 *  - all memory owned by the cache is freed; the parent finalize has run.
 * Thread affinity: the thread that drops the last reference.
 * Ownership: the cache owns its path, table and entries; all are released.
 */
static void
stk_cache_finalize(GObject *object)
{
  StkCache *self = STK_CACHE(object);
  g_queue_clear(&self->lru);
  g_hash_table_unref(self->by_symbol);
  g_free(self->path);
  G_OBJECT_CLASS(stk_cache_parent_class)->finalize(object);
}

/**
 * stk_cache_class_init:
 * @klass: the class structure being initialised.
 *
 * GObject class_init: installs finalize.
 *
 * Preconditions:
 *  - called once by the GType system.
 * Postconditions:
 *  - the finalize vfunc is set.
 * Thread affinity: the thread that first uses the type (GType serialises it).
 * Ownership: none (the class is owned by GType).
 */
static void
stk_cache_class_init(StkCacheClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = stk_cache_finalize;
}

/**
 * stk_cache_init:
 * @self: the instance being initialised.
 *
 * GObject instance init: creates the symbol table (owning its entries) and
 * initialises the LRU queue.
 *
 * Preconditions:
 *  - @self is a zero-initialised new instance.
 * Postconditions:
 *  - by_symbol is an empty table that frees entries with entry_free(); lru is
 *    an empty queue; path is still NULL (set by stk_cache_new()).
 * Thread affinity: the creating thread.
 * Ownership: the instance owns the new table.
 */
static void
stk_cache_init(StkCache *self)
{
  self->by_symbol = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, entry_free);
  g_queue_init(&self->lru);
}

/* Documented in stk-cache.h. */
StkCache *
stk_cache_new(const char *path)
{
  StkCache *self = g_object_new(STK_TYPE_CACHE, NULL);
  self->path = path != NULL ? g_strdup(path)
                            : g_build_filename(g_get_user_cache_dir(), "stocks", "cache.json", NULL);
  return self;
}

/* Documented in stk-cache.h. */
const char *
stk_cache_get_path(StkCache *self)
{
  g_return_val_if_fail(STK_IS_CACHE(self), NULL);
  return self->path;
}

/* Documented in stk-cache.h. */
guint
stk_cache_get_n_symbols(StkCache *self)
{
  g_return_val_if_fail(STK_IS_CACHE(self), 0);
  return g_hash_table_size(self->by_symbol);
}

/**
 * touch:
 * @self: the cache.
 * @e: an entry of @self.
 *
 * Makes @e the most recently used entry (moves its link to the queue tail).
 *
 * Preconditions:
 *  - @e is an entry of @self with a non-NULL lru_link that is currently in
 *    self->lru (true for every entry created by find_or_create(); snapshot
 *    entries are never passed here).
 * Postconditions:
 *  - @e is the tail of the LRU queue; no entry is added or removed.
 * Thread affinity: the thread that owns the cache.
 * Ownership: none transferred; the link stays owned by the queue.
 */
static void
touch(StkCache *self, Entry *e)
{
  if (e->lru_link != g_queue_peek_tail_link(&self->lru)) {
    g_queue_unlink(&self->lru, e->lru_link);
    g_queue_push_tail_link(&self->lru, e->lru_link);
  }
}

/**
 * evict_oldest:
 * @self: the cache.
 *
 * Removes the least recently used entry with all its data.
 *
 * Preconditions:
 *  - @self is a valid cache; every entry of by_symbol is in self->lru and
 *    total_points is the sum of the entries' n_points (so the subtraction
 *    cannot underflow); the call sites only call it while the table is
 *    non-empty, which makes the queue non-empty too.
 * Postconditions:
 *  - on TRUE: the head entry is removed from the queue and the table (and
 *    freed) and total_points is reduced by its points.
 *  - on FALSE (queue unexpectedly empty): nothing changed.
 * Thread affinity: the thread that owns the cache.
 * Ownership: the evicted entry is freed by the table.
 *
 * Returns: TRUE if an entry was evicted; FALSE if the queue was empty (the
 *   callers stop their loops then, instead of dereferencing NULL or spinning).
 */
static gboolean
evict_oldest(StkCache *self)
{
  Entry *e = g_queue_pop_head(&self->lru);
  if (e == NULL)
    return FALSE;
  self->total_points -= e->n_points;
  g_hash_table_remove(self->by_symbol, e->symbol); /* frees e */
  return TRUE;
}

/**
 * enforce_limits:
 * @self: the cache.
 *
 * Evicts least recently used symbols until the limits hold; the most recently
 * used symbol is never evicted by the points limit.
 *
 * Preconditions:
 *  - @self is a valid cache.
 * Postconditions:
 *  - at most STK_CACHE_MAX_SYMBOLS symbols are held, and either at most
 *    STK_CACHE_MAX_TOTAL_POINTS points or a single symbol.
 * Thread affinity: the thread that owns the cache.
 * Ownership: evicted entries are freed.
 */
static void
enforce_limits(StkCache *self)
{
  while (g_hash_table_size(self->by_symbol) > STK_CACHE_MAX_SYMBOLS && evict_oldest(self))
    ;
  while (self->total_points > STK_CACHE_MAX_TOTAL_POINTS && g_hash_table_size(self->by_symbol) > 1 &&
         evict_oldest(self))
    ;
}

/**
 * find_or_create:
 * @self: the cache.
 * @upper: (not nullable): valid, upper-case symbol.
 *
 * Returns the entry as most recently used, creating it when missing.
 *
 * Preconditions:
 *  - @self is a valid cache; @upper is NUL-terminated and valid.
 * Postconditions:
 *  - the entry for @upper exists and is the tail of the LRU queue; a new
 *    entry is empty (no quote, no series). The limits are NOT enforced here.
 * Thread affinity: the thread that owns the cache.
 * Ownership: @upper is borrowed (copied for a new entry); the entry is owned
 *   by the cache.
 *
 * Returns: (transfer none): the entry; never NULL.
 */
static Entry *
find_or_create(StkCache *self, const char *upper)
{
  Entry *e = g_hash_table_lookup(self->by_symbol, upper);
  if (e != NULL) {
    touch(self, e);
    return e;
  }
  e = g_new0(Entry, 1);
  e->symbol = g_strdup(upper);
  g_hash_table_insert(self->by_symbol, e->symbol, e);
  g_queue_push_tail(&self->lru, e);
  e->lru_link = g_queue_peek_tail_link(&self->lru);
  return e;
}

/**
 * clear_all:
 * @self: the cache.
 *
 * Removes every entry.
 *
 * Preconditions:
 *  - @self is a valid cache.
 * Postconditions:
 *  - the queue and table are empty, total_points is 0; the generation
 *    counters are not changed.
 * Thread affinity: the thread that owns the cache.
 * Ownership: all entries are freed (the queue is cleared first, so no entry's
 *   lru_link is used afterwards).
 */
static void
clear_all(StkCache *self)
{
  g_queue_clear(&self->lru);
  g_hash_table_remove_all(self->by_symbol);
  self->total_points = 0;
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
 * age_since:
 * @fetched_ms: fetch time, ms since the Unix epoch.
 *
 * Milliseconds elapsed according to the system clock, clamped to >= 0.
 *
 * Preconditions:
 *  - @fetched_ms >= 0 (put_*() and the loader reject negative values), so the
 *    subtraction cannot overflow.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (reads the clock only).
 * Ownership: none (no pointers).
 *
 * Returns: now - @fetched_ms in ms, or 0 if that is negative.
 */
static gint64
age_since(gint64 fetched_ms)
{
  gint64 age = g_get_real_time() / 1000 - fetched_ms;
  return age > 0 ? age : 0;
}

/* ------------------------------------------------------------------------ */
/* Validation shared by put and load                                        */
/* ------------------------------------------------------------------------ */

/**
 * name_is_valid:
 * @name: (nullable): display name candidate.
 *
 * Checks the length bound with memchr() inside the first
 * STK_CACHE_MAX_NAME_BYTES + 1 bytes (never scans further, equivalent to
 * a length check against the bound) and then UTF-8 validity.
 *
 * Preconditions:
 *  - @name is NULL or NUL-terminated (callers: a caller's quote, or a JSON
 *    string).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: @name is borrowed for the call only.
 *
 * Returns: TRUE iff @name is non-NULL, at most STK_CACHE_MAX_NAME_BYTES bytes
 *   long and valid UTF-8.
 */
static gboolean
name_is_valid(const char *name)
{
  return name != NULL && memchr(name, '\0', STK_CACHE_MAX_NAME_BYTES + 1) != NULL &&
         g_utf8_validate(name, -1, NULL);
}

/**
 * currency_is_valid:
 * @c: (nullable): currency code candidate.
 *
 * Preconditions:
 *  - @c is NULL or NUL-terminated.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: @c is borrowed for the call only.
 *
 * Returns: TRUE iff @c is 1..MAX_CURRENCY_BYTES ASCII letters or digits. The
 *   length is bounded with memchr() inside the first MAX_CURRENCY_BYTES + 1
 *   bytes, so no more is read.
 */
static gboolean
currency_is_valid(const char *c)
{
  if (c == NULL || c[0] == '\0' || memchr(c, '\0', MAX_CURRENCY_BYTES + 1) == NULL)
    return FALSE;
  for (; *c != '\0'; c++)
    if (!g_ascii_isalnum(*c))
      return FALSE;
  return TRUE;
}

/**
 * quote_is_storable:
 * @q: (not nullable): quote to check.
 *
 * Preconditions:
 *  - @q is non-NULL (call site: stk_cache_put_quote() checks it); its strings
 *    are NULL or NUL-terminated.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: @q is borrowed for the call only.
 *
 * Returns: TRUE iff the symbol, kind, name, currency, price/change/change_pct
 *   (NAN allowed, infinity not) and as_of_ms satisfy the requirements of
 *   stk_cache_put_quote().
 */
static gboolean
quote_is_storable(const StkQuote *q)
{
  return q->symbol != NULL && stk_symbol_is_valid(q->symbol) && (guint)q->kind <= (guint)STK_KIND_INDEX &&
         name_is_valid(q->name) && currency_is_valid(q->currency) && !isinf(q->price) &&
         !isinf(q->change) && !isinf(q->change_pct) && q->as_of_ms >= 0;
}

/* ------------------------------------------------------------------------ */
/* Put / get                                                                */
/* ------------------------------------------------------------------------ */

/* Documented in stk-cache.h. */
gboolean
stk_cache_put_quote(StkCache *self, const StkQuote *quote, gint64 fetched_at_ms)
{
  g_return_val_if_fail(STK_IS_CACHE(self), FALSE);
  if (quote == NULL || fetched_at_ms < 0 || !quote_is_storable(quote))
    return FALSE;

  char upper[SYMBOL_BUF_LEN];
  upcase_symbol(quote->symbol, upper);
  Entry *e = find_or_create(self, upper);
  stk_quote_free(e->quote);
  e->quote = stk_quote_copy(quote);
  g_free(e->quote->symbol);
  e->quote->symbol = g_strdup(upper);
  e->quote_ms = fetched_at_ms;
  self->gen++;
  enforce_limits(self);
  return TRUE;
}

/* Documented in stk-cache.h. */
StkQuote *
stk_cache_get_quote(StkCache *self, const char *symbol, gint64 *age_ms)
{
  g_return_val_if_fail(STK_IS_CACHE(self), NULL);
  if (!stk_symbol_is_valid(symbol))
    return NULL;
  char upper[SYMBOL_BUF_LEN];
  upcase_symbol(symbol, upper);
  Entry *e = g_hash_table_lookup(self->by_symbol, upper);
  if (e == NULL || e->quote == NULL)
    return NULL;
  touch(self, e);
  if (age_ms != NULL)
    *age_ms = age_since(e->quote_ms);
  return stk_quote_copy(e->quote);
}

/* Documented in stk-cache.h. */
gboolean
stk_cache_put_series(StkCache *self, const char *symbol, StkSeries *series, gint64 fetched_at_ms)
{
  g_return_val_if_fail(STK_IS_CACHE(self), FALSE);
  if (series == NULL || !stk_symbol_is_valid(symbol) || fetched_at_ms < 0 ||
      (guint)series->range >= N_RANGES || series->n_points > STK_CACHE_MAX_SERIES_POINTS)
    return FALSE;

  char upper[SYMBOL_BUF_LEN];
  upcase_symbol(symbol, upper);
  Entry *e = find_or_create(self, upper);
  SeriesSlot *slot = &e->slots[series->range];
  if (slot->series != NULL) {
    e->n_points -= slot->series->n_points;
    self->total_points -= slot->series->n_points;
    stk_series_unref(slot->series);
  }
  slot->series = stk_series_ref(series);
  slot->fetched_ms = fetched_at_ms;
  e->n_points += series->n_points;
  self->total_points += series->n_points;
  self->gen++;
  enforce_limits(self);
  return TRUE;
}

/* Documented in stk-cache.h. */
StkSeries *
stk_cache_get_series(StkCache *self, const char *symbol, StkRange range, gint64 *age_ms)
{
  g_return_val_if_fail(STK_IS_CACHE(self), NULL);
  if (!stk_symbol_is_valid(symbol) || (guint)range >= N_RANGES)
    return NULL;
  char upper[SYMBOL_BUF_LEN];
  upcase_symbol(symbol, upper);
  Entry *e = g_hash_table_lookup(self->by_symbol, upper);
  if (e == NULL || e->slots[range].series == NULL)
    return NULL;
  touch(self, e);
  if (age_ms != NULL)
    *age_ms = age_since(e->slots[range].fetched_ms);
  return stk_series_ref(e->slots[range].series);
}

/* Documented in stk-cache.h. */
gboolean
stk_cache_is_dirty(StkCache *self)
{
  g_return_val_if_fail(STK_IS_CACHE(self), FALSE);
  return self->gen != self->saved_gen;
}

/* ------------------------------------------------------------------------ */
/* Serialisation (runs on the caller's or a worker thread; touches only the */
/* snapshot)                                                                */
/* ------------------------------------------------------------------------ */

/**
 * snapshot_free:
 * @snap: the snapshot to free.
 *
 * Preconditions:
 *  - @snap was returned by snapshot_take() and is freed exactly once.
 * Postconditions:
 *  - the entry array (and through entry_free() every private copy and series
 *    reference) and the snapshot are freed.
 * Thread affinity: any thread (series references are atomic).
 * Ownership: consumes @snap.
 */
static void
snapshot_free(Snapshot *snap)
{
  g_ptr_array_unref(snap->entries);
  g_free(snap);
}

/* Generated g_autoptr(Snapshot) support: calls snapshot_free(). */
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Snapshot, snapshot_free)

/**
 * snapshot_take:
 * @self: the cache.
 *
 * Takes a private copy of the cache for saving: quotes are deep-copied, series
 * are shared by taking one reference each (their points are never modified
 * after being stored), entries are in LRU order (least recently used first).
 *
 * Preconditions:
 *  - @self is a valid cache owned by the calling thread.
 * Postconditions:
 *  - the snapshot records the generation and the total point count of @self
 *    (at most STK_CACHE_MAX_TOTAL_POINTS: put_*() and the loader keep the
 *    limits); the cache is not modified.
 * Thread affinity: the thread that owns the cache.
 * Ownership: the caller owns the returned snapshot (free with
 *   snapshot_free()).
 *
 * Returns: (transfer full): the new snapshot.
 */
static Snapshot *
snapshot_take(StkCache *self)
{
  Snapshot *snap = g_new0(Snapshot, 1);
  snap->gen = self->gen;
  snap->n_points = self->total_points;
  snap->entries = g_ptr_array_new_full(g_queue_get_length(&self->lru), entry_free);
  for (GList *l = self->lru.head; l != NULL; l = l->next) {
    const Entry *src = l->data;
    Entry *e = g_new0(Entry, 1);
    e->symbol = g_strdup(src->symbol);
    e->quote = src->quote != NULL ? stk_quote_copy(src->quote) : NULL;
    e->quote_ms = src->quote_ms;
    for (guint i = 0; i < N_RANGES; i++) {
      e->slots[i].series = src->slots[i].series != NULL ? stk_series_ref(src->slots[i].series) : NULL;
      e->slots[i].fetched_ms = src->slots[i].fetched_ms;
    }
    g_ptr_array_add(snap->entries, e);
  }
  return snap;
}

/**
 * append_double:
 * @out: the document being built.
 * @v: any double.
 *
 * Appends the shortest of %.15g/%.16g/%.17g that reads back bit-identical;
 * "null" if not finite (JSON has no NaN/Infinity); "-0.0" for negative zero.
 *
 * Preconditions:
 *  - @out is a valid #GString not used by another thread meanwhile.
 * Postconditions:
 *  - one JSON number or null has been appended; the buffer of at least
 *    G_ASCII_DTOSTR_BUF_SIZE bytes is large enough for every formatted value.
 * Thread affinity: any thread (touches only @out).
 * Ownership: @out stays owned by the caller.
 */
static void
append_double(GString *out, double v)
{
  if (!isfinite(v)) {
    g_string_append(out, "null");
    return;
  }
  if (v == 0.0 && signbit(v)) {
    /* "-0" would be read back as the integer 0, losing the sign. */
    g_string_append(out, "-0.0");
    return;
  }
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(buf, sizeof buf, "%.15g", v);
  if (g_ascii_strtod(buf, NULL) != v) {
    g_ascii_formatd(buf, sizeof buf, "%.16g", v);
    if (g_ascii_strtod(buf, NULL) != v)
      g_ascii_formatd(buf, sizeof buf, "%.17g", v);
  }
  g_string_append(out, buf);
}

/**
 * append_series:
 * @out: the document being built.
 * @slot: a slot whose series is non-NULL.
 *
 * Appends the JSON object of one series: fetch time, previous close and the
 * parallel arrays of times and values.
 *
 * Preconditions:
 *  - @out is a valid #GString not used by another thread meanwhile;
 *    @slot->series is non-NULL (call site: snapshot_serialize() checks it).
 * Postconditions:
 *  - the object has been appended; the series is not modified.
 * Thread affinity: any thread (touches only @out and reads the snapshot).
 * Ownership: both pointers are borrowed for the call only.
 */
static void
append_series(GString *out, const SeriesSlot *slot)
{
  const StkSeries *s = slot->series;
  g_string_append_printf(out, "{\"fetched_at_ms\":%" G_GINT64_FORMAT ",\"prev_close\":", slot->fetched_ms);
  append_double(out, s->prev_close);
  g_string_append(out, ",\"t\":[");
  for (gsize i = 0; i < s->n_points; i++) {
    if (i > 0)
      g_string_append_c(out, ',');
    g_string_append_printf(out, "%" G_GINT64_FORMAT, s->points[i].t_ms);
  }
  g_string_append(out, "],\"v\":[");
  for (gsize i = 0; i < s->n_points; i++) {
    if (i > 0)
      g_string_append_c(out, ',');
    append_double(out, s->points[i].v);
  }
  g_string_append(out, "]}");
}

/**
 * append_quote:
 * @out: the document being built.
 * @e: an entry whose quote is non-NULL.
 *
 * Appends the JSON object of the entry's quote. Strings go through
 * stk_json_append_string().
 *
 * Preconditions:
 *  - @out is a valid #GString not used by another thread meanwhile;
 *    @e->quote is non-NULL with non-NULL name and currency (call site:
 *    snapshot_serialize() checks the quote; put_quote() and the loader
 *    validated the strings).
 * Postconditions:
 *  - the object has been appended; the entry is not modified.
 * Thread affinity: any thread (touches only @out and reads the snapshot).
 * Ownership: both pointers are borrowed for the call only.
 */
static void
append_quote(GString *out, const Entry *e)
{
  const StkQuote *q = e->quote;
  g_string_append_printf(out, "{\"fetched_at_ms\":%" G_GINT64_FORMAT ",\"name\":", e->quote_ms);
  stk_json_append_string(out, q->name);
  g_string_append_printf(out, ",\"kind\":\"%s\",\"price\":", stk_kind_to_string(q->kind));
  append_double(out, q->price);
  g_string_append(out, ",\"change\":");
  append_double(out, q->change);
  g_string_append(out, ",\"change_pct\":");
  append_double(out, q->change_pct);
  g_string_append_printf(out, ",\"as_of_ms\":%" G_GINT64_FORMAT ",\"is_realtime\":%s,\"market_open\":%s,\"currency\":",
                         q->as_of_ms, q->is_realtime ? "true" : "false",
                         q->market_open ? "true" : "false");
  stk_json_append_string(out, q->currency);
  g_string_append_c(out, '}');
}

/**
 * snapshot_serialize:
 * @snap: the snapshot to write.
 * @len: (not nullable) (out): receives the document length in bytes.
 * @error: (nullable): return location for a #GError, may be NULL.
 *
 * Serialises the snapshot to the JSON document; fails only if it would exceed
 * the load limit.
 * The initial allocation
 * hint is computed in gsize (g_string_sized_new() takes a gsize): n_points is
 * at most STK_CACHE_MAX_TOTAL_POINTS and entries->len at most
 * STK_CACHE_MAX_SYMBOLS (both enforced by enforce_limits() before any
 * snapshot is taken), so the sum cannot overflow.
 *
 * Size bound: at most STK_CACHE_MAX_TOTAL_POINTS points, each at most 20 (time) + 25 (value) + 2
 * (commas) bytes, plus < 4 KiB per entry for symbol, quote (a 256-byte name escapes to at most
 * 1536 bytes) and per-series framing, with at most STK_CACHE_MAX_SYMBOLS entries: below 13 MiB.
 *
 * Preconditions:
 *  - @snap comes from snapshot_take() and is not modified meanwhile; every
 *    entry has a valid symbol and, if a quote is present, valid strings.
 * Postconditions:
 *  - on non-NULL: *@len is the length of the returned text.
 *  - on NULL: @error (if non-NULL) is set (G_IO_ERROR /
 *    G_IO_ERROR_MESSAGE_TOO_LARGE); *@len is untouched; nothing leaks.
 * Thread affinity: any thread (touches only the snapshot and the result).
 * Ownership: @snap is borrowed; the caller owns the returned buffer.
 *
 * Returns: (transfer full) (nullable): the NUL-terminated document (g_free()),
 *   or NULL if it exceeds STK_CACHE_MAX_FILE_BYTES.
 */
static char *
snapshot_serialize(const Snapshot *snap, gsize *len, GError **error)
{
  GString *out = g_string_sized_new(512 + snap->n_points * 24 + (gsize)snap->entries->len * 400);
  g_string_append_printf(out, "{\"version\":%d,\"entries\":[", CACHE_VERSION);
  for (guint i = 0; i < snap->entries->len; i++) {
    const Entry *e = g_ptr_array_index(snap->entries, i);
    if (i > 0)
      g_string_append_c(out, ',');
    g_string_append(out, "{\"symbol\":");
    stk_json_append_string(out, e->symbol);
    if (e->quote != NULL) {
      g_string_append(out, ",\"quote\":");
      append_quote(out, e);
    }
    gboolean first = TRUE;
    for (guint r = 0; r < N_RANGES; r++) {
      if (e->slots[r].series == NULL)
        continue;
      g_string_append(out, first ? ",\"series\":{" : ",");
      first = FALSE;
      g_string_append_printf(out, "\"%s\":", stk_range_to_string((StkRange)r));
      append_series(out, &e->slots[r]);
    }
    if (!first)
      g_string_append_c(out, '}');
    g_string_append_c(out, '}');
  }
  g_string_append(out, "]}\n");

  if (out->len > STK_CACHE_MAX_FILE_BYTES) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE, "cache: serialised size exceeds limit");
    g_string_free(out, TRUE);
    return NULL;
  }
  *len = out->len;
  return g_string_free(out, FALSE);
}

/**
 * snapshot_write:
 * @snap: the snapshot to write.
 * @path: (not nullable): destination file.
 * @error: (nullable): return location for a #GError, may be NULL.
 *
 * Serialises @snap and stores it with stk_file_write_atomic().
 *
 * Preconditions:
 *  - @snap comes from snapshot_take() and is not modified meanwhile; @path is
 *    non-empty and NUL-terminated; @error is NULL or points to a NULL
 *    #GError pointer.
 * Postconditions:
 *  - on TRUE: the file at @path holds the document; on FALSE: @error (if
 *    non-NULL) is set and the previous file is intact; the temporary text
 *    buffer is freed either way.
 * Thread affinity: any thread (the save worker or the owner of the cache).
 * Ownership: @snap and @path are borrowed for the call only.
 *
 * Returns: TRUE if the file was replaced.
 */
static gboolean
snapshot_write(const Snapshot *snap, const char *path, GError **error)
{
  gsize len = 0;
  g_autofree char *text = snapshot_serialize(snap, &len, error);
  return text != NULL && stk_file_write_atomic(path, text, len, error);
}

/* ------------------------------------------------------------------------ */
/* Load                                                                     */
/* ------------------------------------------------------------------------ */

/**
 * node_get_i64:
 * @n: (nullable): JSON node.
 * @out: (out): receives the value.
 *
 * Preconditions:
 *  - @n is NULL or a node of a live parser; @out is non-NULL.
 * Postconditions:
 *  - on TRUE: *@out is the integer; on FALSE: *@out is untouched.
 * Thread affinity: any thread (touches only the node's reader state).
 * Ownership: @n is borrowed.
 *
 * Returns: TRUE iff @n is a JSON integer value.
 */
static gboolean
node_get_i64(JsonNode *n, gint64 *out)
{
  if (n == NULL || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64)
    return FALSE;
  *out = json_node_get_int(n);
  return TRUE;
}

/**
 * node_get_double:
 * @n: (nullable): JSON node.
 * @allow_null: whether a JSON null is acceptable (read as NAN).
 * @out: (out): receives the value.
 *
 * Reads a number (int or double) that is finite, or null -> NAN when
 * @allow_null.
 *
 * Preconditions:
 *  - @n is NULL or a node of a live parser; @out is non-NULL.
 * Postconditions:
 *  - *@out is written for a null node (NAN) and for a number node (the value
 *    even if it is not finite, in which case the result is FALSE); for any
 *    other node, or NULL, *@out is untouched. So *@out is meaningful only
 *    when the result is TRUE.
 * Thread affinity: any thread (touches only the node's reader state).
 * Ownership: @n is borrowed.
 *
 * Returns: TRUE iff @n is a finite number, or a null with @allow_null.
 */
static gboolean
node_get_double(JsonNode *n, gboolean allow_null, double *out)
{
  if (n == NULL)
    return FALSE;
  if (JSON_NODE_HOLDS_NULL(n)) {
    *out = NAN;
    return allow_null;
  }
  if (!JSON_NODE_HOLDS_VALUE(n))
    return FALSE;
  GType t = json_node_get_value_type(n);
  if (t != G_TYPE_INT64 && t != G_TYPE_DOUBLE)
    return FALSE;
  *out = json_node_get_double(n);
  return isfinite(*out);
}

/**
 * node_get_bool:
 * @n: (nullable): JSON node.
 * @out: (out): receives the value.
 *
 * Preconditions:
 *  - @n is NULL or a node of a live parser; @out is non-NULL.
 * Postconditions:
 *  - on TRUE: *@out is the boolean; on FALSE: *@out is untouched.
 * Thread affinity: any thread (touches only the node's reader state).
 * Ownership: @n is borrowed.
 *
 * Returns: TRUE iff @n is a JSON boolean value.
 */
static gboolean
node_get_bool(JsonNode *n, gboolean *out)
{
  if (n == NULL || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_BOOLEAN)
    return FALSE;
  *out = json_node_get_boolean(n);
  return TRUE;
}

/**
 * node_get_string:
 * @n: (nullable): JSON node.
 *
 * Preconditions:
 *  - @n is NULL or a node of a live parser.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (touches only the node's reader state).
 * Ownership: the returned string belongs to the node (and its parser); it is
 *   valid as long as the parser lives.
 *
 * Returns: (transfer none) (nullable): the string value, or NULL if @n is not
 *   a string value.
 */
static const char *
node_get_string(JsonNode *n)
{
  if (n == NULL || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string(n);
}

/**
 * node_get_object:
 * @n: (nullable): JSON node.
 *
 * Preconditions:
 *  - @n is NULL or a node of a live parser.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (touches only the node's reader state).
 * Ownership: the returned object belongs to the node.
 *
 * Returns: (transfer none) (nullable): the object, or NULL if @n is not an
 *   object.
 */
static JsonObject *
node_get_object(JsonNode *n)
{
  return n != NULL && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
}

/**
 * parse_quote:
 * @symbol: (not nullable): upper-case, valid symbol of the entry.
 * @node: (nullable): JSON node of the "quote" member.
 * @fetched_ms: (out): receives the fetch time.
 *
 * Builds a valid quote for @symbol from the stored object, or NULL.
 *
 * Preconditions:
 *  - @symbol is valid (stk_quote_new() requires it); @node is NULL or a node
 *    of a live parser; @fetched_ms is non-NULL.
 * Postconditions:
 *  - on non-NULL: *@fetched_ms is the stored fetch time (>= 0).
 *  - on NULL: *@fetched_ms is written only if the "fetched_at_ms" member was
 *    an integer, and may then be negative or belong to a quote rejected for
 *    another reason; callers must not use it (load_entry() only keeps it
 *    together with a non-NULL quote).
 * Thread affinity: any thread (touches only the parsed tree).
 * Ownership: the caller owns the returned quote (stk_quote_free()); @node is
 *   borrowed.
 *
 * Returns: (transfer full) (nullable): the quote, or NULL if the object is
 *   missing or any field is invalid.
 */
static StkQuote *
parse_quote(const char *symbol, JsonNode *node, gint64 *fetched_ms)
{
  JsonObject *o = node_get_object(node);
  if (o == NULL)
    return NULL;
  const char *name = node_get_string(json_object_get_member(o, "name"));
  const char *kind_s = node_get_string(json_object_get_member(o, "kind"));
  const char *currency = node_get_string(json_object_get_member(o, "currency"));
  StkKind kind;
  double price, change, change_pct;
  gint64 as_of;
  gboolean realtime, open;
  if (!node_get_i64(json_object_get_member(o, "fetched_at_ms"), fetched_ms) || *fetched_ms < 0 ||
      !name_is_valid(name) || !stk_kind_from_string(kind_s, &kind) || !currency_is_valid(currency) ||
      !node_get_double(json_object_get_member(o, "price"), TRUE, &price) ||
      !node_get_double(json_object_get_member(o, "change"), TRUE, &change) ||
      !node_get_double(json_object_get_member(o, "change_pct"), TRUE, &change_pct) ||
      !node_get_i64(json_object_get_member(o, "as_of_ms"), &as_of) || as_of < 0 ||
      !node_get_bool(json_object_get_member(o, "is_realtime"), &realtime) ||
      !node_get_bool(json_object_get_member(o, "market_open"), &open))
    return NULL;

  StkQuote *q = stk_quote_new(symbol, name, kind, currency);
  q->price = price;
  q->change = change;
  q->change_pct = change_pct;
  q->as_of_ms = as_of;
  q->is_realtime = realtime;
  q->market_open = open;
  return q;
}

/**
 * parse_series:
 * @range: the range of the series; a valid StkRange.
 * @node: (nullable): JSON node of one "series" member.
 * @fetched_ms: (out): receives the fetch time.
 *
 * Builds a valid series for @range from the stored object, or NULL: both
 * arrays must have the same length of at most STK_CACHE_MAX_SERIES_POINTS,
 * times must be integers and values finite numbers, and the points must pass
 * stk_series_append() (ascending times).
 *
 * Preconditions:
 *  - @range < N_RANGES (call site: loop over r < N_RANGES); @node is NULL or
 *    a node of a live parser; @fetched_ms is non-NULL.
 * Postconditions:
 *  - on non-NULL: *@fetched_ms is the stored fetch time (>= 0).
 *  - on NULL: *@fetched_ms is written only if the "fetched_at_ms" member was
 *    an integer, and may then be negative; callers must not use it. A series
 *    under construction is released.
 * Thread affinity: any thread (touches only the parsed tree).
 * Ownership: the caller owns the returned series (one reference); @node is
 *   borrowed.
 *
 * Returns: (transfer full) (nullable): the series, or NULL if invalid.
 */
static StkSeries *
parse_series(StkRange range, JsonNode *node, gint64 *fetched_ms)
{
  JsonObject *o = node_get_object(node);
  if (o == NULL)
    return NULL;
  JsonNode *tn = json_object_get_member(o, "t");
  JsonNode *vn = json_object_get_member(o, "v");
  double prev_close;
  if (!node_get_i64(json_object_get_member(o, "fetched_at_ms"), fetched_ms) || *fetched_ms < 0 ||
      !node_get_double(json_object_get_member(o, "prev_close"), TRUE, &prev_close) || tn == NULL ||
      vn == NULL || !JSON_NODE_HOLDS_ARRAY(tn) || !JSON_NODE_HOLDS_ARRAY(vn))
    return NULL;
  JsonArray *ta = json_node_get_array(tn);
  JsonArray *va = json_node_get_array(vn);
  guint n = json_array_get_length(ta);
  if (n != json_array_get_length(va) || n > STK_CACHE_MAX_SERIES_POINTS)
    return NULL;

  StkSeries *s = stk_series_new(range, prev_close, n);
  for (guint i = 0; i < n; i++) {
    gint64 t;
    double v;
    if (!node_get_i64(json_array_get_element(ta, i), &t) ||
        !node_get_double(json_array_get_element(va, i), FALSE, &v) || !stk_series_append(s, t, v)) {
      stk_series_unref(s);
      return NULL;
    }
  }
  return s;
}

/**
 * load_entry:
 * @self: the cache.
 * @seen: (element-type utf8 void): symbols accepted earlier in this load.
 * @node: (nullable): JSON node of one array element.
 *
 * Adds one stored entry as most recently used; silently skips invalid parts
 * and symbols already in @seen (accepted earlier in this load). An entry
 * with neither a valid quote nor a valid series is skipped. For a rejected
 * quote or series the fetch time that parse_quote()/parse_series() may have
 * written is stored next to a NULL quote/series, where nothing reads it
 * (get_*() and the serialiser test the quote/series pointer first).
 *
 * Preconditions:
 *  - @self is a valid cache; @seen is the table owned by load_document();
 *    @node is NULL or a node of a live parser.
 * Postconditions:
 *  - on acceptance: the entry exists as the most recently used one, holds the
 *    parsed quote and series, its points are counted in n_points and
 *    total_points, and the symbol has been added to @seen. The limits are
 *    enforced by the caller afterwards.
 *  - otherwise: the cache and @seen are unchanged; nothing leaks.
 * Thread affinity: the thread that owns the cache.
 * Ownership: @node is borrowed; parsed objects move into the cache entry.
 */
static void
load_entry(StkCache *self, GHashTable *seen, JsonNode *node)
{
  JsonObject *o = node_get_object(node);
  if (o == NULL)
    return;
  const char *symbol = node_get_string(json_object_get_member(o, "symbol"));
  if (!stk_symbol_is_valid(symbol))
    return;
  char upper[SYMBOL_BUF_LEN];
  upcase_symbol(symbol, upper);
  if (g_hash_table_contains(seen, upper))
    return;

  gint64 quote_ms = 0;
  JsonNode *qn = json_object_get_member(o, "quote");
  StkQuote *quote = qn != NULL ? parse_quote(upper, qn, &quote_ms) : NULL;

  SeriesSlot slots[N_RANGES] = {{NULL, 0}};
  gboolean any_series = FALSE;
  JsonObject *so = node_get_object(json_object_get_member(o, "series"));
  if (so != NULL) {
    for (guint r = 0; r < N_RANGES; r++) {
      JsonNode *sn = json_object_get_member(so, stk_range_to_string((StkRange)r));
      if (sn != NULL) {
        slots[r].series = parse_series((StkRange)r, sn, &slots[r].fetched_ms);
        any_series |= slots[r].series != NULL;
      }
    }
  }
  if (quote == NULL && !any_series)
    return;

  g_hash_table_add(seen, g_strdup(upper));
  Entry *e = find_or_create(self, upper);
  e->quote = quote;
  e->quote_ms = quote_ms;
  for (guint r = 0; r < N_RANGES; r++) {
    e->slots[r] = slots[r];
    if (slots[r].series != NULL) {
      e->n_points += slots[r].series->n_points;
      self->total_points += slots[r].series->n_points;
    }
  }
}

/**
 * load_document:
 * @self: the (empty) cache.
 * @root: (nullable): root node of the parsed document.
 * @error: (nullable): return location for a #GError, may be NULL.
 *
 * Checks the top-level object, "version" and "entries" and offers every
 * element to load_entry(), enforcing the limits after each one.
 *
 * Preconditions:
 *  - @self is empty and valid; @root is NULL or a node owned by a live
 *    JsonParser; @error is NULL or points to a NULL #GError pointer.
 * Postconditions:
 *  - on TRUE: the cache holds the valid entries within the limits.
 *  - on FALSE: @error (if non-NULL) is set (STK_ERROR / STK_ERROR_PARSE);
 *    nothing was added.
 * Thread affinity: the thread that owns the cache.
 * Ownership: @root is borrowed; nothing is retained.
 *
 * Returns: TRUE if the document has the expected shape and version.
 */
static gboolean
load_document(StkCache *self, JsonNode *root, GError **error)
{
  JsonObject *obj = node_get_object(root);
  JsonNode *vn = obj != NULL ? json_object_get_member(obj, "version") : NULL;
  gint64 version;
  if (obj == NULL || !node_get_i64(vn, &version)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "cache: missing or invalid version");
    return FALSE;
  }
  if (version != CACHE_VERSION) {
    g_set_error(error, STK_ERROR, STK_ERROR_PARSE, "cache: unsupported version %" G_GINT64_FORMAT, version);
    return FALSE;
  }
  JsonNode *en = json_object_get_member(obj, "entries");
  if (en == NULL || !JSON_NODE_HOLDS_ARRAY(en)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "cache: missing entries array");
    return FALSE;
  }

  JsonArray *arr = json_node_get_array(en);
  guint n = json_array_get_length(arr);
  /* Stored least recently used first. Every element is offered and
   * enforce_limits() evicts the surplus least recently used symbols, so that
   * invalid or duplicate elements never use up one of the MAX_SYMBOLS slots.
   * Duplicates are recognised against @seen, which keeps every symbol accepted
   * earlier in this load even after its entry was evicted (at most one string
   * per array element, so bounded by the file size cap): the first accepted
   * occurrence always wins. Memory of the cache itself stays bounded: each
   * accepted symbol is evicted as soon as it is the 201st. */
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < n; i++) {
    load_entry(self, seen, json_array_get_element(arr, i));
    enforce_limits(self);
  }
  return TRUE;
}

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
  return g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PENDING, "cache: an asynchronous save is running");
}

/* Documented in stk-cache.h. */
gboolean
stk_cache_load(StkCache *self, GError **error)
{
  g_return_val_if_fail(STK_IS_CACHE(self), FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  if (self->save_in_flight) {
    g_propagate_error(error, pending_error());
    return FALSE;
  }

  clear_all(self);
  stk_file_remove_stale_temps(self->path);
  g_autofree char *text = NULL;
  gsize len = 0;
  gboolean ok = stk_file_read_bounded(self->path, STK_CACHE_MAX_FILE_BYTES, &text, &len, error);
  if (ok && !stk_json_check_strict(text, len, STK_JSON_MAX_DEPTH)) {
    g_set_error(error, STK_ERROR, STK_ERROR_PARSE,
                "cache: not strict JSON or nested deeper than %d", STK_JSON_MAX_DEPTH);
    ok = FALSE;
  }
  if (ok) {
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) perr = NULL;
    if (!json_parser_load_from_data(parser, text, (gssize)len, &perr)) {
      g_set_error(error, STK_ERROR, STK_ERROR_PARSE, "cache: %s", perr->message);
      ok = FALSE;
    } else {
      ok = load_document(self, json_parser_get_root(parser), error);
    }
  }
  if (!ok)
    clear_all(self);
  self->saved_gen = self->gen;
  return ok;
}

/* ------------------------------------------------------------------------ */
/* Save                                                                     */
/* ------------------------------------------------------------------------ */

/* Documented in stk-cache.h. */
gboolean
stk_cache_save(StkCache *self, GError **error)
{
  g_return_val_if_fail(STK_IS_CACHE(self), FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  if (self->save_in_flight) {
    g_propagate_error(error, pending_error());
    return FALSE;
  }

  g_autoptr(Snapshot) snap = snapshot_take(self);
  if (!snapshot_write(snap, self->path, error))
    return FALSE;
  self->saved_gen = snap->gen;
  return TRUE;
}

/* Data of one asynchronous save; owned by the inner GTask as task data. */
typedef struct {
  Snapshot *snap;
  GTask *outer; /* the task handed to the caller, completed in save_done() */
} SaveJob;

/**
 * save_job_free:
 * @job: the job to free.
 *
 * GDestroyNotify of the inner task's data: frees the snapshot, drops the
 * reference on the outer task and frees the job.
 *
 * Preconditions:
 *  - @job was created by stk_cache_save_async() and is freed exactly once (by
 *    the inner GTask, after both save_worker() and save_done() are done with
 *    it).
 * Postconditions:
 *  - the snapshot, the job and the reference on the outer task are released.
 * Thread affinity: any thread (the inner task may be finalised on the worker
 *   or on the thread that ran save_done()).
 * Ownership: consumes @job and its snapshot; releases the outer task.
 */
static void
save_job_free(SaveJob *job)
{
  snapshot_free(job->snap);
  g_object_unref(job->outer);
  g_free(job);
}

/**
 * save_worker:
 * @task: the inner task.
 * @source: (type StkCache): the cache the task was created for.
 * @task_data: the SaveJob.
 * @cancellable: (nullable): unused; cancellation is read through @task.
 *
 * GTaskThreadFunc: serialises the private snapshot and writes it atomically
 * to the cache's path. Returns the cancellation error instead if the task was
 * cancelled before it started.
 *
 * Preconditions:
 *  - runs on a GTask worker thread; @source is kept alive by @task; its path
 *    is never modified after construction (so reading it here is race free);
 *    @task_data is the SaveJob whose snapshot nothing else mutates meanwhile
 *    (the shared series are only read).
 * Postconditions:
 *  - @task is completed exactly once: TRUE, the cancellation error or the
 *    error of snapshot_write().
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
  GError *error = NULL;
  if (snapshot_write(job->snap, STK_CACHE(source)->path, &error))
    g_task_return_boolean(task, TRUE);
  else
    g_task_return_error(task, error);
}

/**
 * save_done:
 * @source: (type StkCache): the cache.
 * @result: the finished inner task.
 * @user_data: unused.
 *
 * GAsyncReadyCallback of the inner task, run in the main context of the
 * thread that called stk_cache_save_async(): clears save_in_flight, marks the
 * cache saved as of the snapshot generation on success, and completes the
 * caller's outer task with the same outcome.
 *
 * Preconditions:
 *  - @source is the cache and @result its inner task, whose task data is the
 *    SaveJob; runs in the thread that owns the cache.
 * Postconditions:
 *  - save_in_flight is FALSE; on success saved_gen is the generation of the
 *    snapshot (the cache stays dirty if put_*() ran since); the outer task
 *    has been completed once.
 * Thread affinity: the thread that owns the cache (main thread).
 * Ownership: the error obtained from the inner task is handed to the outer
 *   task; nothing else is retained.
 */
static void
save_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)user_data;
  StkCache *self = STK_CACHE(source);
  GTask *inner = G_TASK(result);
  const SaveJob *job = g_task_get_task_data(inner);
  GTask *outer = job->outer;
  guint64 gen = job->snap->gen;

  GError *error = NULL;
  gboolean ok = g_task_propagate_boolean(inner, &error);
  self->save_in_flight = FALSE;
  if (ok)
    self->saved_gen = gen;

  if (ok)
    g_task_return_boolean(outer, TRUE);
  else
    g_task_return_error(outer, error);
}

/* Documented in stk-cache.h. */
void
stk_cache_save_async(StkCache *self, GCancellable *cancellable, GAsyncReadyCallback callback,
                     gpointer user_data)
{
  g_return_if_fail(STK_IS_CACHE(self));

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
  job->snap = snapshot_take(self);
  job->outer = outer;
  GTask *inner = g_task_new(self, cancellable, save_done, NULL);
  g_task_set_check_cancellable(inner, FALSE);
  g_task_set_task_data(inner, job, (GDestroyNotify)save_job_free);
  g_task_run_in_thread(inner, save_worker);
  g_object_unref(inner);
}

/* Documented in stk-cache.h. */
gboolean
stk_cache_save_finish(StkCache *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(STK_IS_CACHE(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  g_return_val_if_fail(g_async_result_is_tagged(result, &save_tag), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
