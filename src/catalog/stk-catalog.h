/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-catalog.h - the built-in catalog: Fidelity ETFs / mutual funds plus the
 * major US market indices.
 *
 * The catalog is data/fidelity_catalog.json and data/market_indices.json
 * compiled into the binary as GResources (no file or network access at
 * runtime). Both documents are parsed once into ONE immutable, compact array
 * sorted by symbol; every query afterwards is a binary search or a linear scan
 * over that array without parsing again.
 *
 * Thread-safety: a StkCatalog is never modified after creation, so any number
 * of threads may call the query functions on the same instance concurrently.
 * Creating and freeing it must not race with queries.
 */
#ifndef STK_CATALOG_H
#define STK_CATALOG_H

#include <glib.h>

#include "stk-types.h"

G_BEGIN_DECLS

/**
 * StkCatalogEntry: one catalog row. All strings are owned by the StkCatalog
 * that hands the entry out and stay valid until stk_catalog_free().
 */
typedef struct {
  const char *symbol;   /**< upper-case ticker; passes stk_symbol_is_valid() */
  const char *name;     /**< display name, non-empty                        */
  const char *category; /**< category text, may be ""                       */
  StkKind kind;         /**< STK_KIND_ETF, STK_KIND_FUND or STK_KIND_INDEX, never STOCK */

  /*< private >*/
  const char *search; /* folded "symbol\0name\0category\0" used by filter */
} StkCatalogEntry;

/** Opaque, immutable catalog. */
typedef struct _StkCatalog StkCatalog;

/** Which instrument classes stk_catalog_filter() lets through. */
typedef enum {
  STK_CATALOG_FILTER_ALL,
  STK_CATALOG_FILTER_ETF,  /**< entries with kind STK_KIND_ETF   */
  STK_CATALOG_FILTER_FUND, /**< entries with kind STK_KIND_FUND  */
  STK_CATALOG_FILTER_INDEX /**< entries with kind STK_KIND_INDEX */
} StkCatalogKindFilter;

/**
 * stk_catalog_load_default:
 * @error: (out) (optional): receives the failure.
 *
 * Parses the two catalogs compiled into the binary (fidelity_catalog.json,
 * then market_indices.json, both in the schema of stk_catalog_new_from_data())
 * and merges them into ONE catalog sorted by symbol. A symbol that occurs
 * twice, in one file or across the two files, rejects the whole catalog. Does
 * no network or filesystem access. Call it once and share the result.
 *
 * Preconditions:
 *  - @error is NULL or points to a NULL #GError pointer (checked: critical and NULL otherwise).
 * Postconditions:
 *  - on success a new catalog owned by the caller; on failure NULL, nothing is leaked and, if
 *    @error is non-NULL, *@error is set.
 * Thread affinity: any thread (no GTK or main-loop use).
 * Ownership: the caller owns the returned catalog; the embedded resources are not retained.
 *
 * Returns: (transfer full): the catalog, or NULL with @error set
 *   (STK_ERROR_NOT_FOUND if a resource is missing, STK_ERROR_PARSE if a
 *   file's content is invalid (message names that file) or a symbol occurs
 *   twice (message names the symbol)). Release with stk_catalog_free().
 */
StkCatalog *stk_catalog_load_default(GError **error);

/**
 * stk_catalog_new_from_data:
 * @data: (nullable): JSON text, @len bytes (not required to be NUL-terminated).
 * @len: length of @data in bytes.
 * @error: (out) (optional): receives the failure.
 *
 * Parses a single catalog document:
 *   {"version": 1, "items": [{"symbol","name","kind","category"}, ...]}
 * Other members (e.g. "generated", "sources") are ignored. The document is
 * accepted only if:
 *   - it is valid JSON whose root is an object with "version" equal to 1;
 *   - "items" is a non-empty array of objects, each having the string members
 *     "symbol", "name" (non-empty), "kind" ("etf", "fund" or "index") and
 *     "category";
 *   - every symbol satisfies stk_symbol_is_valid() and is already upper-case;
 *   - no symbol occurs twice.
 * Items may appear in any order; the catalog is sorted by symbol (byte order).
 *
 * Preconditions:
 *  - @error is NULL or points to a NULL #GError pointer (checked: critical and NULL otherwise).
 *  - @data is NULL (then rejected as "empty document") or points to at least @len readable
 *    bytes.
 *  - @data is TRUSTED text (compiled-in data or data the program built itself): the only
 *    byte-size limit is G_MAXSSIZE bytes and names and categories have no length limit. Invalid
 *    UTF-8 is rejected by json-glib and reported as STK_ERROR_PARSE ("JSON data must be UTF-8
 *    encoded"); a \u0000 escape inside a name truncates it at that point (probe:
 *    "AB\u0000CD" and "AB\u0000" are stored as "AB"); a name that starts with \u0000 becomes
 *    empty and is rejected as STK_ERROR_PARSE ("missing or empty name"). In this tree only the
 *    test programs call this function; the application loads its catalog with
 *    stk_catalog_load_default(). Do
 *    not pass untrusted network or file data without validating it first.
 * Postconditions:
 *  - on success a new catalog owned by the caller; @data is not retained (all text is copied).
 *  - on failure NULL; nothing is leaked and, if @error is non-NULL, *@error is set
 *    (@data NULL or @len 0 gives "empty document").
 * Thread affinity: any thread.
 * Ownership: the caller keeps @data; the caller owns the returned catalog.
 *
 * Returns: (transfer full): the catalog, or NULL with @error set to
 *   STK_ERROR_PARSE (message names the offending item). Never crashes on bad
 *   input. Release with stk_catalog_free().
 */
StkCatalog *stk_catalog_new_from_data(const char *data, gsize len, GError **error);

/**
 * stk_catalog_new_from_documents:
 * @datas: @n_docs JSON texts, each non-NULL for a valid catalog.
 * @lens: @n_docs lengths in bytes (texts need not be NUL-terminated).
 * @n_docs: number of documents, at least 1.
 * @error: (out) (optional): receives the failure.
 *
 * Parses every document like stk_catalog_new_from_data() and merges all items
 * into ONE catalog sorted by symbol; this is what stk_catalog_load_default()
 * does with its two files. Each document must be valid on its own, and a
 * symbol occurring twice, within one document or across documents, rejects
 * the whole catalog.
 *
 * Preconditions:
 *  - @error is NULL or points to a NULL #GError pointer, and @datas, @lens are non-NULL and
 *    @n_docs is at least 1 (all checked: critical and NULL otherwise).
 *  - @datas and @lens have @n_docs elements. An individual NULL @datas[i] (or a length of 0)
 *    is not a programming error: it is rejected as "empty document" (STK_ERROR_PARSE).
 *  - every non-NULL @datas[i] points to @lens[i] readable bytes.
 *  - the same TRUSTED-data precondition as stk_catalog_new_from_data() applies; only the test
 *    programs call this function in this tree. Invalid UTF-8 is rejected by json-glib and
 *    reported as STK_ERROR_PARSE ("JSON data must be UTF-8 encoded");
 *    a \u0000 escape inside a name truncates it at that point (probe: "AB\u0000CD" and
 *    "AB\u0000" are stored as "AB"); a name that starts with \u0000 becomes empty and is
 *    rejected as STK_ERROR_PARSE ("missing or empty name").
 * Postconditions:
 *  - on success a new catalog owned by the caller; no input is retained.
 *  - on failure NULL; nothing is leaked and, if @error is non-NULL, *@error is set.
 * Thread affinity: any thread.
 * Ownership: the caller keeps the arrays and texts; the caller owns the returned catalog.
 *
 * Returns: (transfer full): the catalog, or NULL with @error set to
 *   STK_ERROR_PARSE (with several documents the message names the offending
 *   one as "document N", counting from 1; a duplicate message names the
 *   symbol). Release with stk_catalog_free().
 */
StkCatalog *stk_catalog_new_from_documents(const char *const *datas, const gsize *lens,
                                           guint n_docs, GError **error);

/**
 * stk_catalog_free:
 * @catalog: (nullable) (transfer full): the catalog to free; NULL is a no-op.
 *
 * Frees @catalog and every entry/string obtained from it.
 *
 * Preconditions:
 *  - @catalog is NULL or came from a stk_catalog_* constructor and is not freed twice.
 *  - no other thread is querying @catalog; no entry or string taken from it is used afterwards.
 * Postconditions:
 *  - the catalog, its entries array and its string arena are released.
 * Thread affinity: any thread, but not concurrently with queries on the same catalog.
 * Ownership: takes ownership of @catalog.
 */
void stk_catalog_free(StkCatalog *catalog);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StkCatalog, stk_catalog_free)

/**
 * stk_catalog_entries:
 * @catalog: (not nullable): the catalog; non-NULL (checked: critical and NULL otherwise).
 * @n_entries: (out) (not nullable): receives the number of entries; non-NULL (checked as
 *   for @catalog).
 *
 * Gives access to the whole sorted entry array.
 *
 * Preconditions:
 *  - @catalog is a live catalog; @n_entries is writable.
 * Postconditions:
 *  - *@n_entries is set on success; nothing is allocated. On a failed precondition it is
 *    untouched.
 * Thread affinity: any thread (read-only).
 * Ownership: the array stays owned by @catalog.
 *
 * Returns: (transfer none): the entries sorted strictly ascending by symbol
 *   (byte order). Read-only; valid until stk_catalog_free(). Never NULL.
 */
const StkCatalogEntry *stk_catalog_entries(const StkCatalog *catalog, gsize *n_entries);

/**
 * stk_catalog_lookup:
 * @catalog: (not nullable): the catalog; non-NULL (checked: critical and NULL otherwise).
 * @symbol: (nullable): ticker to find. Matched ASCII case-insensitively.
 *
 * Binary search; allocates nothing.
 *
 * Preconditions:
 *  - @catalog is a live catalog.
 *  - @symbol is NULL, a NUL-terminated string, or at least 16 readable bytes (only the first
 *    16 bytes are examined, see stk_symbol_is_valid()).
 * Postconditions:
 *  - no state is changed. The key is upper-cased into a local 16-byte buffer only after
 *    stk_symbol_is_valid() proved at most 15 bytes plus NUL.
 * Thread affinity: any thread (read-only).
 * Ownership: @symbol is only read; the returned entry is owned by @catalog.
 *
 * Returns: (transfer none) (nullable): the entry, or NULL if @symbol is NULL,
 *   fails stk_symbol_is_valid() or is not in the catalog. Valid until
 *   stk_catalog_free().
 */
const StkCatalogEntry *stk_catalog_lookup(const StkCatalog *catalog, const char *symbol);

/**
 * stk_catalog_filter:
 * @catalog: (not nullable): the catalog; non-NULL (checked: critical and NULL otherwise).
 * @kind: which instrument class to keep.
 * @query: (nullable): text to look for; NULL or "" keeps everything. An entry
 *   matches when @query occurs as a contiguous substring of its symbol, its
 *   name or its category (each field on its own), ignoring ASCII case.
 *   Non-ASCII bytes are compared exactly. Surrounding whitespace is not
 *   trimmed. Queries of any length are accepted.
 * @n_results: (out) (not nullable): receives the number of results; non-NULL (checked:
 *   critical and NULL otherwise).
 *
 * One linear pass over the precomputed entries; the only allocation is one array sized for
 * the whole catalog, filled for the first *@n_results slots; it is returned, or freed
 * again when nothing matches.
 *
 * Preconditions:
 *  - @catalog is a live catalog; @n_results is writable.
 *  - @query is NULL or NUL-terminated. No length cap is applied; the cost is one NUL scan of
 *    the query plus a substring search of every field whose cost grows with the field length
 *    times the query length.
 * Postconditions:
 *  - *@n_results is set on success. The entries stay owned by @catalog. When nothing matches
 *    the result is NULL and no allocation outlives the call (the internal array is freed);
 *    otherwise the only allocation that outlives the call is the returned array.
 * Thread affinity: any thread (read-only).
 * Ownership: @query is only read; the caller owns the returned array (not the entries).
 *
 * Returns: (transfer container) (nullable): pointers to the matching entries
 *   in symbol order, or NULL when nothing matches (*@n_results == 0). Free the
 *   array with g_free(); the entries stay owned by @catalog.
 */
const StkCatalogEntry **stk_catalog_filter(const StkCatalog *catalog,
                                           StkCatalogKindFilter kind,
                                           const char *query, gsize *n_results);

G_END_DECLS

#endif /* STK_CATALOG_H */
