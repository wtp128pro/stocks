#include "stk-cache.h"

#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

#include "stk-fileio.h"

#define CACHE_VERSION 1
#define N_RANGES ((guint)STK_RANGE_1Y + 1)
#define MAX_CURRENCY_BYTES 8

/* Per-symbol data. Also used (without lru_link) for save snapshots. */
/* Source tag of stk_cache_save_async() tasks; only its address is used. */
static char save_tag;

typedef struct {
  StkSeries *series; /* NULL when absent */
  gint64 fetched_ms;
} SeriesSlot;

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

G_DEFINE_TYPE(StkCache, stk_cache, G_TYPE_OBJECT)

/* ------------------------------------------------------------------------ */
/* Entries and LRU                                                          */
/* ------------------------------------------------------------------------ */

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

static void
stk_cache_finalize(GObject *object)
{
  StkCache *self = STK_CACHE(object);
  g_queue_clear(&self->lru);
  g_hash_table_unref(self->by_symbol);
  g_free(self->path);
  G_OBJECT_CLASS(stk_cache_parent_class)->finalize(object);
}

static void
stk_cache_class_init(StkCacheClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = stk_cache_finalize;
}

static void
stk_cache_init(StkCache *self)
{
  self->by_symbol = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, entry_free);
  g_queue_init(&self->lru);
}

StkCache *
stk_cache_new(const char *path)
{
  StkCache *self = g_object_new(STK_TYPE_CACHE, NULL);
  self->path = path != NULL ? g_strdup(path)
                            : g_build_filename(g_get_user_cache_dir(), "stocks", "cache.json", NULL);
  return self;
}

const char *
stk_cache_get_path(StkCache *self)
{
  g_return_val_if_fail(STK_IS_CACHE(self), NULL);
  return self->path;
}

guint
stk_cache_get_n_symbols(StkCache *self)
{
  g_return_val_if_fail(STK_IS_CACHE(self), 0);
  return g_hash_table_size(self->by_symbol);
}

static void
touch(StkCache *self, Entry *e)
{
  if (e->lru_link != g_queue_peek_tail_link(&self->lru)) {
    g_queue_unlink(&self->lru, e->lru_link);
    g_queue_push_tail_link(&self->lru, e->lru_link);
  }
}

static void
evict_oldest(StkCache *self)
{
  Entry *e = g_queue_pop_head(&self->lru);
  self->total_points -= e->n_points;
  g_hash_table_remove(self->by_symbol, e->symbol); /* frees e */
}

/* Evicts least recently used symbols until the limits hold; the most recently
 * used symbol is never evicted by the points limit. */
static void
enforce_limits(StkCache *self)
{
  while (g_hash_table_size(self->by_symbol) > STK_CACHE_MAX_SYMBOLS)
    evict_oldest(self);
  while (self->total_points > STK_CACHE_MAX_TOTAL_POINTS && g_hash_table_size(self->by_symbol) > 1)
    evict_oldest(self);
}

/* @upper: valid, upper-case. Returns the entry as most recently used,
 * creating it when missing. */
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

static void
clear_all(StkCache *self)
{
  g_queue_clear(&self->lru);
  g_hash_table_remove_all(self->by_symbol);
  self->total_points = 0;
}

/* @symbol must satisfy stk_symbol_is_valid() (so it is at most 15 bytes). */
static void
upcase_symbol(const char *symbol, char out[16])
{
  gsize i = 0;
  for (; symbol[i] != '\0'; i++)
    out[i] = g_ascii_toupper(symbol[i]);
  out[i] = '\0';
}

static gint64
age_since(gint64 fetched_ms)
{
  gint64 age = g_get_real_time() / 1000 - fetched_ms;
  return age > 0 ? age : 0;
}

/* ------------------------------------------------------------------------ */
/* Validation shared by put and load                                        */
/* ------------------------------------------------------------------------ */

static gboolean
name_is_valid(const char *name)
{
  return name != NULL && strlen(name) <= STK_CACHE_MAX_NAME_BYTES && g_utf8_validate(name, -1, NULL);
}

static gboolean
currency_is_valid(const char *c)
{
  if (c == NULL || c[0] == '\0' || strlen(c) > MAX_CURRENCY_BYTES)
    return FALSE;
  for (; *c != '\0'; c++)
    if (!g_ascii_isalnum(*c))
      return FALSE;
  return TRUE;
}

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

gboolean
stk_cache_put_quote(StkCache *self, const StkQuote *quote, gint64 fetched_at_ms)
{
  g_return_val_if_fail(STK_IS_CACHE(self), FALSE);
  if (quote == NULL || fetched_at_ms < 0 || !quote_is_storable(quote))
    return FALSE;

  char upper[16];
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

StkQuote *
stk_cache_get_quote(StkCache *self, const char *symbol, gint64 *age_ms)
{
  g_return_val_if_fail(STK_IS_CACHE(self), NULL);
  if (!stk_symbol_is_valid(symbol))
    return NULL;
  char upper[16];
  upcase_symbol(symbol, upper);
  Entry *e = g_hash_table_lookup(self->by_symbol, upper);
  if (e == NULL || e->quote == NULL)
    return NULL;
  touch(self, e);
  if (age_ms != NULL)
    *age_ms = age_since(e->quote_ms);
  return stk_quote_copy(e->quote);
}

gboolean
stk_cache_put_series(StkCache *self, const char *symbol, StkSeries *series, gint64 fetched_at_ms)
{
  g_return_val_if_fail(STK_IS_CACHE(self), FALSE);
  if (series == NULL || !stk_symbol_is_valid(symbol) || fetched_at_ms < 0 ||
      (guint)series->range >= N_RANGES || series->n_points > STK_CACHE_MAX_SERIES_POINTS)
    return FALSE;

  char upper[16];
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

StkSeries *
stk_cache_get_series(StkCache *self, const char *symbol, StkRange range, gint64 *age_ms)
{
  g_return_val_if_fail(STK_IS_CACHE(self), NULL);
  if (!stk_symbol_is_valid(symbol) || (guint)range >= N_RANGES)
    return NULL;
  char upper[16];
  upcase_symbol(symbol, upper);
  Entry *e = g_hash_table_lookup(self->by_symbol, upper);
  if (e == NULL || e->slots[range].series == NULL)
    return NULL;
  touch(self, e);
  if (age_ms != NULL)
    *age_ms = age_since(e->slots[range].fetched_ms);
  return stk_series_ref(e->slots[range].series);
}

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

static void
snapshot_free(Snapshot *snap)
{
  g_ptr_array_unref(snap->entries);
  g_free(snap);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(Snapshot, snapshot_free)

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

/* Shortest of %.15g/%.16g/%.17g that reads back bit-identical; null if not
 * finite (JSON has no NaN/Infinity). */
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

/* Returns the document (g_free()); fails only if it would exceed the load limit.
 * Size bound: at most STK_CACHE_MAX_TOTAL_POINTS points, each at most 20 (time)
 * + 25 (value) + 2 (commas) bytes, plus < 4 KiB per entry for symbol, quote
 * (a 256-byte name escapes to at most 1536 bytes) and per-series framing, with
 * at most STK_CACHE_MAX_SYMBOLS entries: below 13 MiB. */
static char *
snapshot_serialize(const Snapshot *snap, gsize *len, GError **error)
{
  GString *out = g_string_sized_new(512 + snap->n_points * 24 + snap->entries->len * 400);
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

static gboolean
node_get_i64(JsonNode *n, gint64 *out)
{
  if (n == NULL || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64)
    return FALSE;
  *out = json_node_get_int(n);
  return TRUE;
}

/* Number (int or double) that is finite, or null -> NAN when @allow_null. */
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

static gboolean
node_get_bool(JsonNode *n, gboolean *out)
{
  if (n == NULL || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_BOOLEAN)
    return FALSE;
  *out = json_node_get_boolean(n);
  return TRUE;
}

static const char *
node_get_string(JsonNode *n)
{
  if (n == NULL || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string(n);
}

static JsonObject *
node_get_object(JsonNode *n)
{
  return n != NULL && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
}

/* Returns a valid quote for @symbol or NULL. */
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

/* Returns a valid series for @range or NULL. */
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

/* Adds one stored entry as most recently used; silently skips invalid parts
 * and symbols already in @seen (accepted earlier in this load). */
static void
load_entry(StkCache *self, GHashTable *seen, JsonNode *node)
{
  JsonObject *o = node_get_object(node);
  if (o == NULL)
    return;
  const char *symbol = node_get_string(json_object_get_member(o, "symbol"));
  if (!stk_symbol_is_valid(symbol))
    return;
  char upper[16];
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

static GError *
pending_error(void)
{
  return g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PENDING, "cache: an asynchronous save is running");
}

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

typedef struct {
  Snapshot *snap;
  GTask *outer; /* the task handed to the caller, completed in save_done() */
} SaveJob;

static void
save_job_free(SaveJob *job)
{
  snapshot_free(job->snap);
  g_object_unref(job->outer);
  g_free(job);
}

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

gboolean
stk_cache_save_finish(StkCache *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(STK_IS_CACHE(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  g_return_val_if_fail(g_async_result_is_tagged(result, &save_tag), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
