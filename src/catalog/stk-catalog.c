/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/* stk-catalog.c - see stk-catalog.h. */
#include "stk-catalog.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "stk-catalog-resources.h"

#define CATALOG_RESOURCE_FIDELITY "/io/github/wtp128pro/Stocks/catalog/fidelity_catalog.json"
#define CATALOG_RESOURCE_INDICES "/io/github/wtp128pro/Stocks/catalog/market_indices.json"
#define CATALOG_VERSION 1
/* STK symbols are at most 15 bytes (stk_symbol_is_valid); a buffer holding one needs the NUL too. */
#define SYMBOL_MAX_LEN 15
#define SYMBOL_BUF (SYMBOL_MAX_LEN + 1)
/* Upper bound for memchr() NUL scans of NUL-terminated text of no documented cap (no object is larger). */
#define TEXT_SCAN_MAX ((gsize)G_MAXSSIZE)

struct _StkCatalog {
  StkCatalogEntry *entries; /* sorted by symbol, strictly ascending */
  gsize n_entries;
  GStringChunk *strings; /* owns every string referenced by entries */
};

/**
 * entry_cmp:
 * @a: (not nullable): pointer to a #StkCatalogEntry.
 * @b: (not nullable): pointer to a #StkCatalogEntry.
 *
 * qsort() comparator: orders entries by symbol in byte order (the order of g_strcmp0()).
 *
 * Preconditions:
 *  - @a and @b point to entries whose symbol is a non-NULL NUL-terminated string. The only
 *    caller (catalog_from_documents()) sorts entries filled by parse_document(), whose
 *    symbol comes from g_string_chunk_insert().
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: nothing is retained.
 *
 * Returns: negative, zero or positive as the symbol of @a sorts before, equal to or after the
 *   symbol of @b.
 */
static int
entry_cmp(const void *a, const void *b)
{
  return g_strcmp0(((const StkCatalogEntry *)a)->symbol, ((const StkCatalogEntry *)b)->symbol);
}

/**
 * member_string:
 * @obj: (not nullable): JSON object to read.
 * @key: (not nullable): member name.
 *
 * Member @key of @obj if it is a JSON string, else NULL.
 *
 * Preconditions:
 *  - @obj is a valid #JsonObject and @key a NUL-terminated string (all callers pass literals
 *    and an object obtained from a parsed document).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (touches only @obj; the caller must not use @obj concurrently).
 * Ownership: nothing is retained; the returned string is owned by @obj.
 *
 * Returns: (transfer none) (nullable): the member's string, or NULL if the member is missing or
 *   not a string.
 */
static const char *
member_string(JsonObject *obj, const char *key)
{
  JsonNode *node = json_object_get_member(obj, key);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string(node);
}

/**
 * is_canonical_symbol:
 * @symbol: (not nullable): candidate ticker.
 *
 * Whether @symbol passes stk_symbol_is_valid() and contains no ASCII lower-case letter.
 *
 * Preconditions:
 *  - @symbol is a non-NULL NUL-terminated string (the only caller passes a member string of a
 *    parsed JSON document, after a NULL check).
 * Postconditions:
 *  - no state is changed. The scan loop only runs once stk_symbol_is_valid() proved 1..15 bytes
 *    followed by a NUL, so it stays inside that string.
 * Thread affinity: any thread (pure).
 * Ownership: nothing is retained.
 *
 * Returns: TRUE if @symbol is a valid, already upper-case symbol, FALSE otherwise.
 */
static gboolean
is_canonical_symbol(const char *symbol)
{
  if (!stk_symbol_is_valid(symbol))
    return FALSE;
  for (const char *p = symbol; *p != '\0'; p++)
    if (g_ascii_islower(*p))
      return FALSE;
  return TRUE;
}

/**
 * make_search_blob:
 * @strings: (not nullable): the catalog's string arena that receives the blob.
 * @scratch: (not nullable): work buffer, reused between calls; its content is overwritten.
 * @symbol: (not nullable): entry symbol.
 * @name: (not nullable): entry name.
 * @category: (not nullable): entry category.
 *
 * Builds the folded search blob "symbol\0name\0category\0" (each field ASCII-lower-cased, each
 * followed by a NUL) for one entry in the catalog's string arena.
 *
 * Preconditions:
 *  - @symbol, @name and @category are non-NULL NUL-terminated strings (parse_document() passes
 *    the arena copies it just made with g_string_chunk_insert() / g_string_chunk_insert_const()).
 * Postconditions:
 *  - the blob (exactly three NUL-terminated fields) is stored in @strings; @scratch holds a
 *    copy of it. This blob layout is what entry_matches() relies on.
 * Thread affinity: any thread (touches only its arguments).
 * Ownership: the blob is owned by @strings; nothing else is retained.
 *
 * Returns: (transfer none): the blob, valid until @strings is freed.
 */
static const char *
make_search_blob(GStringChunk *strings, GString *scratch, const char *symbol,
                 const char *name, const char *category)
{
  const char *fields[3] = {symbol, name, category};
  g_string_set_size(scratch, 0);
  for (int i = 0; i < 3; i++) {
    for (const char *p = fields[i]; *p != '\0'; p++)
      g_string_append_c(scratch, g_ascii_tolower(*p));
    g_string_append_c(scratch, '\0');
  }
  return g_string_chunk_insert_len(strings, scratch->str, (gssize)scratch->len);
}

/**
 * parse_error:
 * @error: (nullable): return location for a #GError, may be NULL.
 * @label: (nullable): names the document in the message; NULL for none.
 * @fmt: (not nullable): printf-style format of the message text.
 * @...: arguments for @fmt.
 *
 * Sets @error to STK_ERROR_PARSE with the text "catalog: [<label>: ]<message>".
 *
 * Preconditions:
 *  - @fmt is a literal matching @... (checked by G_GNUC_PRINTF); @label is NULL or
 *    NUL-terminated; @error is NULL or points to a NULL #GError pointer (every caller passes
 *    the error argument of a public function that checked this).
 * Postconditions:
 *  - if @error is non-NULL, *@error is a new error in domain STK_ERROR, code STK_ERROR_PARSE.
 * Thread affinity: any thread.
 * Ownership: @label and @fmt are only read; the new #GError belongs to the caller.
 *
 * Returns: void (the format attribute is written before the name, so doc tools may expect it).
 */
static void G_GNUC_PRINTF(3, 4)
parse_error(GError **error, const char *label, const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  g_autofree char *msg = g_strdup_vprintf(fmt, args);
  va_end(args);
  g_set_error(error, STK_ERROR, STK_ERROR_PARSE, "catalog: %s%s%s", label != NULL ? label : "",
              label != NULL ? ": " : "", msg);
}

/**
 * parse_document:
 * @data: (nullable): JSON text, @len bytes, not required to be NUL-terminated.
 * @len: length of @data in bytes.
 * @label: (nullable): document name for error messages.
 * @entries: (not nullable): array of #StkCatalogEntry that receives the items.
 * @strings: (not nullable): string arena that owns every string the new entries reference.
 * @scratch: (not nullable): work buffer for make_search_blob().
 * @error: (nullable): return location for a #GError, may be NULL.
 *
 * Parses one catalog document and appends its items to @entries (strings go to @strings).
 * Nothing is sorted or checked for duplicates here.
 *
 * Preconditions:
 *  - if @data is non-NULL it points to at least @len readable bytes; @entries, @strings and
 *    @scratch are valid and used by the caller only.
 *  - @data is meant to be trusted (compiled-in or caller-built) text: only the cap of
 *    G_MAXSSIZE bytes is enforced, with no length cap on names/categories. Invalid UTF-8 is
 *    rejected by json-glib and reported as STK_ERROR_PARSE ("JSON data must be UTF-8 encoded");
 *    a \u0000 escape inside a name truncates it at that point (probe: "AB\u0000CD" and
 *    "AB\u0000" are stored as "AB"); a name that starts with \u0000 becomes empty and is
 *    rejected as STK_ERROR_PARSE ("missing or empty name").
 * Postconditions:
 *  - on TRUE every item of the document was appended with fully initialised fields.
 *  - on FALSE @error is STK_ERROR_PARSE (a NULL @data or @len of 0 gives "empty document"),
 *    @entries may hold the items appended before the problem and is discarded by the caller.
 *  - json_parser_load_from_data() returns FALSE only with its error set, so reading
 *    its message is safe.
 * Thread affinity: any thread.
 * Ownership: nothing passed in is retained except the strings copied into @strings.
 *
 * Returns: TRUE if the whole document was accepted, FALSE with @error set otherwise.
 */
static gboolean
parse_document(const char *data, gsize len, const char *label, GArray *entries,
               GStringChunk *strings, GString *scratch, GError **error)
{
  if (data == NULL || len == 0) {
    parse_error(error, label, "empty document");
    return FALSE;
  }
  if (len > (gsize)G_MAXSSIZE) {
    parse_error(error, label, "document too large");
    return FALSE;
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) json_err = NULL;
  if (!json_parser_load_from_data(parser, data, (gssize)len, &json_err)) {
    parse_error(error, label, "invalid JSON: %s", json_err->message);
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
    parse_error(error, label, "root is not an object");
    return FALSE;
  }
  JsonObject *root_obj = json_node_get_object(root);

  JsonNode *version = json_object_get_member(root_obj, "version");
  if (version == NULL || !JSON_NODE_HOLDS_VALUE(version) ||
      json_node_get_value_type(version) != G_TYPE_INT64 ||
      json_node_get_int(version) != CATALOG_VERSION) {
    parse_error(error, label, "unsupported or missing \"version\" (expected %d)", CATALOG_VERSION);
    return FALSE;
  }

  JsonNode *items_node = json_object_get_member(root_obj, "items");
  if (items_node == NULL || !JSON_NODE_HOLDS_ARRAY(items_node)) {
    parse_error(error, label, "\"items\" is missing or not an array");
    return FALSE;
  }
  JsonArray *items = json_node_get_array(items_node);
  guint n_items = json_array_get_length(items);
  if (n_items == 0) {
    parse_error(error, label, "\"items\" is empty");
    return FALSE;
  }

  for (guint i = 0; i < n_items; i++) {
    JsonNode *node = json_array_get_element(items, i);
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node)) {
      parse_error(error, label, "item %u is not an object", i);
      return FALSE;
    }
    JsonObject *obj = json_node_get_object(node);
    const char *symbol = member_string(obj, "symbol");
    const char *name = member_string(obj, "name");
    const char *kind_str = member_string(obj, "kind");
    const char *category = member_string(obj, "category");
    StkKind kind = STK_KIND_STOCK;

    if (symbol == NULL || !is_canonical_symbol(symbol)) {
      parse_error(error, label, "item %u: missing or invalid symbol", i);
      return FALSE;
    }
    if (name == NULL || name[0] == '\0') {
      parse_error(error, label, "item %u (%s): missing or empty name", i, symbol);
      return FALSE;
    }
    if (category == NULL) {
      parse_error(error, label, "item %u (%s): missing category", i, symbol);
      return FALSE;
    }
    if (kind_str == NULL || !stk_kind_from_string(kind_str, &kind) ||
        kind == STK_KIND_STOCK) {
      parse_error(error, label, "item %u (%s): kind must be \"etf\", \"fund\" or \"index\"", i,
                  symbol);
      return FALSE;
    }

    StkCatalogEntry e;
    e.symbol = g_string_chunk_insert(strings, symbol);
    e.name = g_string_chunk_insert(strings, name);
    /* few distinct categories: store each text once */
    e.category = g_string_chunk_insert_const(strings, category);
    e.kind = kind;
    e.search = make_search_blob(strings, scratch, e.symbol, e.name, e.category);
    g_array_append_val(entries, e);
  }
  return TRUE;
}

/**
 * catalog_from_documents:
 * @datas: (not nullable): @n_docs JSON texts; an individual NULL entry is rejected by
 *   parse_document() as "empty document".
 * @lens: (not nullable): @n_docs lengths in bytes.
 * @labels: (nullable): @n_docs document names for error messages, or NULL.
 * @n_docs: number of documents; at least 1.
 * @error: (nullable): return location for a #GError, may be NULL.
 *
 * Builds one catalog from @n_docs documents: every item of every document goes into the single
 * sorted entries array, and a symbol occurring twice (in one document or across documents)
 * rejects the whole catalog. @labels only decorate error messages; without them documents are
 * called "document N" when there is more than one.
 *
 * Preconditions:
 *  - @datas and @lens have @n_docs elements; @labels, if given, too. Callers: the public
 *    constructors, which check @error, and (for the arrays) stk_catalog_load_default().
 * Postconditions:
 *  - on success the catalog's entries are sorted by symbol in byte order, strictly ascending.
 *  - on failure nothing is leaked and @error is STK_ERROR_PARSE.
 * Thread affinity: any thread.
 * Ownership: inputs are only read (not retained); the result belongs to the caller.
 *
 * Returns: (transfer full) (nullable): the new catalog, or NULL with @error set.
 */
static StkCatalog *
catalog_from_documents(const char *const *datas, const gsize *lens, const char *const *labels,
                       guint n_docs, GError **error)
{
  g_autoptr(GStringChunk) strings = g_string_chunk_new(8192);
  g_autoptr(GString) scratch = g_string_sized_new(256);
  g_autoptr(GArray) entries = g_array_new(FALSE, FALSE, sizeof(StkCatalogEntry));

  for (guint d = 0; d < n_docs; d++) {
    g_autofree char *numbered = NULL;
    const char *label = labels != NULL ? labels[d] : NULL;
    if (label == NULL && n_docs > 1)
      label = numbered = g_strdup_printf("document %u", d + 1);
    if (!parse_document(datas[d], lens[d], label, entries, strings, scratch, error))
      return NULL;
  }

  qsort(entries->data, entries->len, sizeof(StkCatalogEntry), entry_cmp);
  const StkCatalogEntry *sorted = (const StkCatalogEntry *)(void *)entries->data;
  for (guint i = 1; i < entries->len; i++) {
    if (g_str_equal(sorted[i - 1].symbol, sorted[i].symbol)) {
      parse_error(error, NULL, "duplicate symbol %s", sorted[i].symbol);
      return NULL;
    }
  }

  StkCatalog *catalog = g_new0(StkCatalog, 1);
  catalog->n_entries = entries->len;
  catalog->entries = (StkCatalogEntry *)(void *)g_array_free(g_steal_pointer(&entries), FALSE);
  catalog->strings = g_steal_pointer(&strings);
  return catalog;
}

/* Documented in stk-catalog.h. */
StkCatalog *
stk_catalog_new_from_data(const char *data, gsize len, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  return catalog_from_documents(&data, &len, NULL, 1, error);
}

/* Documented in stk-catalog.h. */
StkCatalog *
stk_catalog_new_from_documents(const char *const *datas, const gsize *lens, guint n_docs,
                               GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  g_return_val_if_fail(datas != NULL && lens != NULL && n_docs > 0, NULL);
  return catalog_from_documents(datas, lens, NULL, n_docs, error);
}

/**
 * lookup_resource:
 * @path: (not nullable): GResource path of the embedded document.
 * @error: (nullable): return location for a #GError, may be NULL.
 *
 * Looks up one embedded document; sets NOT_FOUND and returns NULL if missing.
 *
 * Preconditions:
 *  - @path is a NUL-terminated string (the caller passes the CATALOG_RESOURCE_* literals).
 * Postconditions:
 *  - on failure @error is STK_ERROR_NOT_FOUND, "catalog resource missing: <reason>".
 *    g_resource_lookup_data() returns NULL only with its error set, so reading that
 *    error's message is safe.
 * Thread affinity: any thread (GResource lookup is thread-safe).
 * Ownership: @path is only read.
 *
 * Returns: (transfer full) (nullable): the document bytes (release with g_bytes_unref()), or
 *   NULL with @error set.
 */
static GBytes *
lookup_resource(const char *path, GError **error)
{
  g_autoptr(GError) res_err = NULL;
  GBytes *bytes = g_resource_lookup_data(stk_catalog_get_resource(), path,
                                         G_RESOURCE_LOOKUP_FLAGS_NONE, &res_err);
  if (bytes == NULL)
    g_set_error(error, STK_ERROR, STK_ERROR_NOT_FOUND, "catalog resource missing: %s",
                res_err->message);
  return bytes;
}

/* Documented in stk-catalog.h. */
StkCatalog *
stk_catalog_load_default(GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);

  static const char *const paths[] = {CATALOG_RESOURCE_FIDELITY, CATALOG_RESOURCE_INDICES};
  static const char *const labels[] = {"fidelity_catalog.json", "market_indices.json"};
  GBytes *bytes[G_N_ELEMENTS(paths)] = {NULL};
  const char *datas[G_N_ELEMENTS(paths)];
  gsize lens[G_N_ELEMENTS(paths)];
  StkCatalog *catalog = NULL;
  guint n_loaded = 0;

  for (; n_loaded < G_N_ELEMENTS(paths); n_loaded++) {
    bytes[n_loaded] = lookup_resource(paths[n_loaded], error);
    if (bytes[n_loaded] == NULL)
      goto out;
    datas[n_loaded] = g_bytes_get_data(bytes[n_loaded], &lens[n_loaded]);
  }
  catalog = catalog_from_documents(datas, lens, labels, G_N_ELEMENTS(paths), error);

out:
  for (guint i = 0; i < n_loaded; i++)
    g_bytes_unref(bytes[i]);
  return catalog;
}

/* Documented in stk-catalog.h. */
void
stk_catalog_free(StkCatalog *catalog)
{
  if (catalog == NULL)
    return;
  g_free(catalog->entries);
  g_string_chunk_free(catalog->strings);
  g_free(catalog);
}

/* Documented in stk-catalog.h. */
const StkCatalogEntry *
stk_catalog_entries(const StkCatalog *catalog, gsize *n_entries)
{
  g_return_val_if_fail(catalog != NULL && n_entries != NULL, NULL);
  *n_entries = catalog->n_entries;
  return catalog->entries;
}

/* Documented in stk-catalog.h. */
const StkCatalogEntry *
stk_catalog_lookup(const StkCatalog *catalog, const char *symbol)
{
  g_return_val_if_fail(catalog != NULL, NULL);
  if (!stk_symbol_is_valid(symbol))
    return NULL;

  /* Bounded length: stk_symbol_is_valid() proved a NUL within SYMBOL_BUF bytes, so n <= 15. */
  const char *nul = memchr(symbol, '\0', SYMBOL_BUF);
  if (nul == NULL)
    return NULL;
  char key[SYMBOL_BUF];
  gsize n = (gsize)(nul - symbol);
  for (gsize i = 0; i <= n; i++)
    key[i] = g_ascii_toupper(symbol[i]);

  gsize lo = 0, hi = catalog->n_entries;
  while (lo < hi) {
    gsize mid = lo + (hi - lo) / 2;
    int c = g_strcmp0(catalog->entries[mid].symbol, key);
    if (c == 0)
      return &catalog->entries[mid];
    if (c < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return NULL;
}

/**
 * contains_folded:
 * @hay: (not nullable): lower-cased NUL-terminated text to search.
 * @query: (not nullable): text to look for, at least @qlen readable bytes, none of the first
 *   @qlen bytes a NUL.
 * @qlen: number of bytes of @query to use; greater than 0.
 *
 * TRUE if @query (folded on the fly) occurs in the lower-cased @hay.
 *
 * Preconditions:
 *  - @hay is NUL-terminated and @query holds @qlen non-NUL bytes. Because of the latter the
 *    comparison stops at the NUL of @hay, so no read passes it. @qlen is computed by
 *    stk_catalog_filter() from a bounded NUL scan of @query and passed unchanged through
 *    entry_matches().
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: nothing is retained.
 *
 * Returns: TRUE if @hay contains @query ignoring ASCII case, FALSE otherwise.
 */
static gboolean
contains_folded(const char *hay, const char *query, gsize qlen)
{
  for (; *hay != '\0'; hay++) {
    gsize j = 0;
    while (j < qlen && hay[j] == g_ascii_tolower(query[j]))
      j++;
    if (j == qlen)
      return TRUE;
  }
  return FALSE;
}

/**
 * entry_matches:
 * @e: (not nullable): entry of a catalog built by catalog_from_documents().
 * @query: (not nullable): text to look for, with @qlen non-NUL bytes.
 * @qlen: length of @query in bytes; greater than 0.
 *
 * Whether @query occurs in the symbol, the name or the category of @e (each on its own).
 *
 * Preconditions:
 *  - @e->search is the blob made by make_search_blob(): three NUL-terminated fields. Entries
 *    are created only by parse_document(), so every entry reachable through a #StkCatalog
 *    has it.
 * Postconditions:
 *  - no state is changed. The walk visits at most three fields and stops at a missing NUL.
 * Thread affinity: any thread (read-only).
 * Ownership: nothing is retained.
 *
 * Returns: TRUE if any of the three fields contains @query ignoring ASCII case, else FALSE.
 */
static gboolean
entry_matches(const StkCatalogEntry *e, const char *query, gsize qlen)
{
  const char *field = e->search;
  for (int i = 0; i < 3; i++) {
    if (contains_folded(field, query, qlen))
      return TRUE;
    const char *end = memchr(field, '\0', TEXT_SCAN_MAX);
    if (end == NULL)
      return FALSE;
    field = end + 1;
  }
  return FALSE;
}

/* Documented in stk-catalog.h. */
const StkCatalogEntry **
stk_catalog_filter(const StkCatalog *catalog, StkCatalogKindFilter kind, const char *query,
                   gsize *n_results)
{
  g_return_val_if_fail(catalog != NULL && n_results != NULL, NULL);

  gsize qlen = 0;
  if (query != NULL) {
    const char *qend = memchr(query, '\0', TEXT_SCAN_MAX);
    if (qend == NULL) {
      *n_results = 0;
      return NULL;
    }
    qlen = (gsize)(qend - query);
  }
  const StkCatalogEntry **out = g_new(const StkCatalogEntry *, catalog->n_entries);
  gsize n = 0;

  for (gsize i = 0; i < catalog->n_entries; i++) {
    const StkCatalogEntry *e = &catalog->entries[i];
    if (kind == STK_CATALOG_FILTER_ETF && e->kind != STK_KIND_ETF)
      continue;
    if (kind == STK_CATALOG_FILTER_FUND && e->kind != STK_KIND_FUND)
      continue;
    if (kind == STK_CATALOG_FILTER_INDEX && e->kind != STK_KIND_INDEX)
      continue;
    if (qlen > 0 && !entry_matches(e, query, qlen))
      continue;
    out[n++] = e;
  }

  *n_results = n;
  if (n == 0) {
    g_free(out);
    return NULL;
  }
  return out;
}
