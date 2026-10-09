/* stk-catalog.c - see stk-catalog.h. */
#include "stk-catalog.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "stk-catalog-resources.h"

#define CATALOG_RESOURCE_PATH "/org/stocks/catalog/fidelity_catalog.json"
#define CATALOG_VERSION 1
/* STK symbols are at most 15 bytes (stk_symbol_is_valid). */
#define SYMBOL_BUF 16

struct _StkCatalog {
  StkCatalogEntry *entries; /* sorted by symbol, strictly ascending */
  gsize n_entries;
  GStringChunk *strings; /* owns every string referenced by entries */
};

static int
entry_cmp(const void *a, const void *b)
{
  return strcmp(((const StkCatalogEntry *)a)->symbol, ((const StkCatalogEntry *)b)->symbol);
}

/* Member @key of @obj if it is a JSON string, else NULL. */
static const char *
member_string(JsonObject *obj, const char *key)
{
  JsonNode *node = json_object_get_member(obj, key);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string(node);
}

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

/*
 * Builds the folded search blob "symbol\0name\0category\0" for one entry in the
 * catalog's string arena. @scratch is reused between calls.
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

static void
parse_error(GError **error, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

static void
parse_error(GError **error, const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  g_autofree char *msg = g_strdup_vprintf(fmt, args);
  va_end(args);
  g_set_error(error, STK_ERROR, STK_ERROR_PARSE, "catalog: %s", msg);
}

StkCatalog *
stk_catalog_new_from_data(const char *data, gsize len, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);

  if (data == NULL || len == 0) {
    parse_error(error, "empty document");
    return NULL;
  }
  if (len > (gsize)G_MAXSSIZE) {
    parse_error(error, "document too large");
    return NULL;
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) json_err = NULL;
  if (!json_parser_load_from_data(parser, data, (gssize)len, &json_err)) {
    parse_error(error, "invalid JSON: %s", json_err->message);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
    parse_error(error, "root is not an object");
    return NULL;
  }
  JsonObject *root_obj = json_node_get_object(root);

  JsonNode *version = json_object_get_member(root_obj, "version");
  if (version == NULL || !JSON_NODE_HOLDS_VALUE(version) ||
      json_node_get_value_type(version) != G_TYPE_INT64 ||
      json_node_get_int(version) != CATALOG_VERSION) {
    parse_error(error, "unsupported or missing \"version\" (expected %d)", CATALOG_VERSION);
    return NULL;
  }

  JsonNode *items_node = json_object_get_member(root_obj, "items");
  if (items_node == NULL || !JSON_NODE_HOLDS_ARRAY(items_node)) {
    parse_error(error, "\"items\" is missing or not an array");
    return NULL;
  }
  JsonArray *items = json_node_get_array(items_node);
  guint n_items = json_array_get_length(items);
  if (n_items == 0) {
    parse_error(error, "\"items\" is empty");
    return NULL;
  }

  g_autoptr(GStringChunk) strings = g_string_chunk_new(8192);
  g_autoptr(GString) scratch = g_string_sized_new(256);
  g_autofree StkCatalogEntry *entries = g_new0(StkCatalogEntry, n_items);

  for (guint i = 0; i < n_items; i++) {
    JsonNode *node = json_array_get_element(items, i);
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node)) {
      parse_error(error, "item %u is not an object", i);
      return NULL;
    }
    JsonObject *obj = json_node_get_object(node);
    const char *symbol = member_string(obj, "symbol");
    const char *name = member_string(obj, "name");
    const char *kind_str = member_string(obj, "kind");
    const char *category = member_string(obj, "category");
    StkKind kind = STK_KIND_STOCK;

    if (symbol == NULL || !is_canonical_symbol(symbol)) {
      parse_error(error, "item %u: missing or invalid symbol", i);
      return NULL;
    }
    if (name == NULL || name[0] == '\0') {
      parse_error(error, "item %u (%s): missing or empty name", i, symbol);
      return NULL;
    }
    if (category == NULL) {
      parse_error(error, "item %u (%s): missing category", i, symbol);
      return NULL;
    }
    if (kind_str == NULL || !stk_kind_from_string(kind_str, &kind) ||
        kind == STK_KIND_STOCK) {
      parse_error(error, "item %u (%s): kind must be \"etf\" or \"fund\"", i, symbol);
      return NULL;
    }

    StkCatalogEntry *e = &entries[i];
    e->symbol = g_string_chunk_insert(strings, symbol);
    e->name = g_string_chunk_insert(strings, name);
    /* few distinct categories: store each text once */
    e->category = g_string_chunk_insert_const(strings, category);
    e->kind = kind;
    e->search = make_search_blob(strings, scratch, e->symbol, e->name, e->category);
  }

  qsort(entries, n_items, sizeof *entries, entry_cmp);
  for (guint i = 1; i < n_items; i++) {
    if (strcmp(entries[i - 1].symbol, entries[i].symbol) == 0) {
      parse_error(error, "duplicate symbol %s", entries[i].symbol);
      return NULL;
    }
  }

  StkCatalog *catalog = g_new0(StkCatalog, 1);
  catalog->entries = g_steal_pointer(&entries);
  catalog->n_entries = n_items;
  catalog->strings = g_steal_pointer(&strings);
  return catalog;
}

StkCatalog *
stk_catalog_load_default(GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);

  g_autoptr(GError) res_err = NULL;
  g_autoptr(GBytes) bytes = g_resource_lookup_data(
      stk_catalog_get_resource(), CATALOG_RESOURCE_PATH, G_RESOURCE_LOOKUP_FLAGS_NONE, &res_err);
  if (bytes == NULL) {
    g_set_error(error, STK_ERROR, STK_ERROR_NOT_FOUND, "catalog resource missing: %s",
                res_err->message);
    return NULL;
  }
  gsize len = 0;
  const char *data = g_bytes_get_data(bytes, &len);
  return stk_catalog_new_from_data(data, len, error);
}

void
stk_catalog_free(StkCatalog *catalog)
{
  if (catalog == NULL)
    return;
  g_free(catalog->entries);
  g_string_chunk_free(catalog->strings);
  g_free(catalog);
}

const StkCatalogEntry *
stk_catalog_entries(const StkCatalog *catalog, gsize *n_entries)
{
  g_return_val_if_fail(catalog != NULL && n_entries != NULL, NULL);
  *n_entries = catalog->n_entries;
  return catalog->entries;
}

const StkCatalogEntry *
stk_catalog_lookup(const StkCatalog *catalog, const char *symbol)
{
  g_return_val_if_fail(catalog != NULL, NULL);
  if (!stk_symbol_is_valid(symbol))
    return NULL;

  char key[SYMBOL_BUF];
  gsize n = strlen(symbol); /* <= 15 once valid */
  for (gsize i = 0; i <= n; i++)
    key[i] = g_ascii_toupper(symbol[i]);

  gsize lo = 0, hi = catalog->n_entries;
  while (lo < hi) {
    gsize mid = lo + (hi - lo) / 2;
    int c = strcmp(catalog->entries[mid].symbol, key);
    if (c == 0)
      return &catalog->entries[mid];
    if (c < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return NULL;
}

/* TRUE if @query (qlen > 0, folded on the fly) occurs in the lower-cased @hay. */
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

static gboolean
entry_matches(const StkCatalogEntry *e, const char *query, gsize qlen)
{
  const char *field = e->search;
  for (int i = 0; i < 3; i++) {
    if (contains_folded(field, query, qlen))
      return TRUE;
    field += strlen(field) + 1;
  }
  return FALSE;
}

const StkCatalogEntry **
stk_catalog_filter(const StkCatalog *catalog, StkCatalogKindFilter kind, const char *query,
                   gsize *n_results)
{
  g_return_val_if_fail(catalog != NULL && n_results != NULL, NULL);

  gsize qlen = query != NULL ? strlen(query) : 0;
  const StkCatalogEntry **out = g_new(const StkCatalogEntry *, catalog->n_entries);
  gsize n = 0;

  for (gsize i = 0; i < catalog->n_entries; i++) {
    const StkCatalogEntry *e = &catalog->entries[i];
    if (kind == STK_CATALOG_FILTER_ETF && e->kind != STK_KIND_ETF)
      continue;
    if (kind == STK_CATALOG_FILTER_FUND && e->kind != STK_KIND_FUND)
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
