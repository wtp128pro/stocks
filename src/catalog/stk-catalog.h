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
 * Returns: (transfer full): the catalog, or NULL with @error set to
 *   STK_ERROR_PARSE (with several documents the message names the offending
 *   one as "document N", counting from 1; a duplicate message names the
 *   symbol). Release with stk_catalog_free().
 */
StkCatalog *stk_catalog_new_from_documents(const char *const *datas, const gsize *lens,
                                           guint n_docs, GError **error);

/** Frees @catalog and every entry/string obtained from it. NULL is allowed. */
void stk_catalog_free(StkCatalog *catalog);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(StkCatalog, stk_catalog_free)

/**
 * stk_catalog_entries:
 * @catalog: non-NULL catalog.
 * @n_entries: (out): receives the number of entries.
 *
 * Returns: (transfer none): the entries sorted strictly ascending by symbol
 *   (strcmp order). Read-only; valid until stk_catalog_free(). Never NULL.
 */
const StkCatalogEntry *stk_catalog_entries(const StkCatalog *catalog, gsize *n_entries);

/**
 * stk_catalog_lookup:
 * @catalog: non-NULL catalog.
 * @symbol: (nullable): ticker to find. Matched ASCII case-insensitively.
 *
 * Binary search; allocates nothing.
 *
 * Returns: (transfer none) (nullable): the entry, or NULL if @symbol is NULL,
 *   fails stk_symbol_is_valid() or is not in the catalog. Valid until
 *   stk_catalog_free().
 */
const StkCatalogEntry *stk_catalog_lookup(const StkCatalog *catalog, const char *symbol);

/**
 * stk_catalog_filter:
 * @catalog: non-NULL catalog.
 * @kind: which instrument class to keep.
 * @query: (nullable): text to look for; NULL or "" keeps everything. An entry
 *   matches when @query occurs as a contiguous substring of its symbol, its
 *   name or its category (each field on its own), ignoring ASCII case.
 *   Non-ASCII bytes are compared exactly. Surrounding whitespace is not
 *   trimmed. Queries of any length are accepted.
 * @n_results: (out): receives the number of results.
 *
 * One linear pass over the precomputed entries; the only allocation is the
 * returned array (sized for the whole catalog, filled for the first
 * *@n_results slots).
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
