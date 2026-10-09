/* Offline unit tests for the catalog module (loader + shipped data). */
#include <glib.h>
#include <string.h>

#include "stk-catalog.h"
#include "stk-types.h"

static const StkCatalog *
shared_catalog(void)
{
  static StkCatalog *catalog = NULL;
  if (catalog == NULL) {
    g_autoptr(GError) err = NULL;
    catalog = stk_catalog_load_default(&err);
    g_assert_no_error(err);
    g_assert_nonnull(catalog);
  }
  return catalog;
}

static gboolean
has_symbol(const StkCatalogEntry *const *list, gsize n, const char *symbol)
{
  for (gsize i = 0; i < n; i++)
    if (strcmp(list[i]->symbol, symbol) == 0)
      return TRUE;
  return FALSE;
}

/* ------------------------------------------------------------------------ */
/* shipped data                                                             */
/* ------------------------------------------------------------------------ */

static void
test_default_shape(void)
{
  gsize n = 0;
  const StkCatalogEntry *e = stk_catalog_entries(shared_catalog(), &n);
  g_assert_nonnull(e);
  g_assert_cmpuint(n, >=, 80);

  gsize n_etf = 0, n_fund = 0;
  for (gsize i = 0; i < n; i++) {
    g_assert_true(stk_symbol_is_valid(e[i].symbol));
    for (const char *p = e[i].symbol; *p != '\0'; p++)
      g_assert_false(g_ascii_islower(*p));
    g_assert_cmpstr(e[i].name, !=, "");
    g_assert_nonnull(e[i].category);
    g_assert_true(g_utf8_validate(e[i].name, -1, NULL));
    g_assert_true(g_utf8_validate(e[i].category, -1, NULL));
    g_assert_true(e[i].kind == STK_KIND_ETF || e[i].kind == STK_KIND_FUND);
    if (e[i].kind == STK_KIND_ETF) {
      /* no partial name-based guesses: Fidelity's own vocabulary has no bare "Equity" */
      g_assert_cmpstr(e[i].category, !=, "Equity");
      g_assert_cmpstr(e[i].category, !=, "Fixed Income");
      n_etf++;
    } else
      n_fund++;
    if (i > 0)
      g_assert_cmpint(strcmp(e[i - 1].symbol, e[i].symbol), <, 0);
  }
  g_assert_cmpuint(n_etf, >=, 80);
  g_assert_cmpuint(n_fund, >, 0);
}

static void
test_default_membership(void)
{
  const StkCatalog *c = shared_catalog();
  static const char *const etfs[] = {"FBTC", "FETH", "FSOL", "FBCG", "FDVV", "FBND"};
  static const char *const funds[] = {"FXAIX", "FZROX", "FZILX", "FNILX", "FZIPX", "FSKAX"};
  static const char *const absent[] = {"FLCG", "FIS",  "FNF",  "FDBC", "FSST",
                                       "FDWM", "FDHT", "FSBD", "FSLD", "FINA",
                                       /* Nasdaq reports no parseable price for these */
                                       "SPAXX", "FDRXX", "FMSNX"};

  for (gsize i = 0; i < G_N_ELEMENTS(etfs); i++) {
    const StkCatalogEntry *e = stk_catalog_lookup(c, etfs[i]);
    g_assert_nonnull(e);
    g_assert_cmpint(e->kind, ==, STK_KIND_ETF);
  }
  for (gsize i = 0; i < G_N_ELEMENTS(funds); i++) {
    const StkCatalogEntry *e = stk_catalog_lookup(c, funds[i]);
    g_assert_nonnull(e);
    g_assert_cmpint(e->kind, ==, STK_KIND_FUND);
  }
  for (gsize i = 0; i < G_N_ELEMENTS(absent); i++)
    g_assert_null(stk_catalog_lookup(c, absent[i]));
}

/* ------------------------------------------------------------------------ */
/* lookup                                                                   */
/* ------------------------------------------------------------------------ */

static void
test_lookup(void)
{
  const StkCatalog *c = shared_catalog();
  gsize n = 0;
  const StkCatalogEntry *e = stk_catalog_entries(c, &n);

  /* every entry is found, by its own symbol and in lower case, at its address */
  for (gsize i = 0; i < n; i++) {
    g_assert_true(stk_catalog_lookup(c, e[i].symbol) == &e[i]);
    g_autofree char *lower = g_ascii_strdown(e[i].symbol, -1);
    g_assert_true(stk_catalog_lookup(c, lower) == &e[i]);
  }

  const StkCatalogEntry *fbcg = stk_catalog_lookup(c, "fbcg");
  g_assert_nonnull(fbcg);
  g_assert_cmpstr(fbcg->symbol, ==, "FBCG");
  g_assert_cmpstr(fbcg->name, ==, "Fidelity Blue Chip Growth ETF");

  g_assert_null(stk_catalog_lookup(c, NULL));
  g_assert_null(stk_catalog_lookup(c, ""));
  g_assert_null(stk_catalog_lookup(c, "ZZZZZZ"));
  g_assert_null(stk_catalog_lookup(c, "AAPL"));
  g_assert_null(stk_catalog_lookup(c, "FBCG "));
  g_assert_null(stk_catalog_lookup(c, "../FBCG"));
  g_assert_null(stk_catalog_lookup(c, "FBCG/x"));
  g_assert_null(stk_catalog_lookup(c, "THISSYMBOLISWAYTOOLONG"));
}

/* ------------------------------------------------------------------------ */
/* filter                                                                   */
/* ------------------------------------------------------------------------ */

static void
test_filter_kind(void)
{
  const StkCatalog *c = shared_catalog();
  gsize n_all = 0;
  stk_catalog_entries(c, &n_all);

  gsize n = 0, n_etf = 0, n_fund = 0;
  g_autofree const StkCatalogEntry **all = stk_catalog_filter(c, STK_CATALOG_FILTER_ALL, NULL, &n);
  g_assert_cmpuint(n, ==, n_all);
  g_assert_nonnull(all);

  g_autofree const StkCatalogEntry **etfs =
      stk_catalog_filter(c, STK_CATALOG_FILTER_ETF, "", &n_etf);
  g_autofree const StkCatalogEntry **funds =
      stk_catalog_filter(c, STK_CATALOG_FILTER_FUND, NULL, &n_fund);
  g_assert_cmpuint(n_etf + n_fund, ==, n_all);
  g_assert_cmpuint(n_etf, >=, 80);
  for (gsize i = 0; i < n_etf; i++)
    g_assert_cmpint(etfs[i]->kind, ==, STK_KIND_ETF);
  for (gsize i = 0; i < n_fund; i++)
    g_assert_cmpint(funds[i]->kind, ==, STK_KIND_FUND);
  g_assert_true(has_symbol(etfs, n_etf, "FBTC"));
  g_assert_false(has_symbol(etfs, n_etf, "FXAIX"));
  g_assert_true(has_symbol(funds, n_fund, "FXAIX"));
}

static void
test_filter_query(void)
{
  const StkCatalog *c = shared_catalog();
  gsize n = 0;

  /* name, any case */
  g_autofree const StkCatalogEntry **r =
      stk_catalog_filter(c, STK_CATALOG_FILTER_ALL, "bLuE cHiP", &n);
  g_assert_cmpuint(n, >=, 2);
  g_assert_true(has_symbol(r, n, "FBCG"));
  g_assert_true(has_symbol(r, n, "FBCV"));
  for (gsize i = 0; i < n; i++) {
    g_assert_nonnull(strstr(r[i]->name, "Blue Chip"));
    if (i > 0)
      g_assert_cmpint(strcmp(r[i - 1]->symbol, r[i]->symbol), <, 0);
  }

  /* symbol */
  g_autofree const StkCatalogEntry **sym = stk_catalog_filter(c, STK_CATALOG_FILTER_ALL, "fzr", &n);
  g_assert_true(has_symbol(sym, n, "FZROX"));

  /* kind and query combine */
  g_autofree const StkCatalogEntry **etf_only =
      stk_catalog_filter(c, STK_CATALOG_FILTER_ETF, "bitcoin", &n);
  g_assert_cmpuint(n, ==, 1);
  g_assert_cmpstr(etf_only[0]->symbol, ==, "FBTC");
  gsize n_fund = 0;
  g_assert_null(stk_catalog_filter(c, STK_CATALOG_FILTER_FUND, "bitcoin", &n_fund));
  g_assert_cmpuint(n_fund, ==, 0);

  /* category field */
  const StkCatalogEntry *fxaix = stk_catalog_lookup(c, "FXAIX");
  g_assert_cmpstr(fxaix->category, !=, "");
  g_autofree char *cat_query = g_ascii_strup(fxaix->category, -1);
  g_autofree const StkCatalogEntry **by_cat =
      stk_catalog_filter(c, STK_CATALOG_FILTER_FUND, cat_query, &n);
  g_assert_true(has_symbol(by_cat, n, "FXAIX"));

  /* no match, and fields are searched separately (never across a boundary) */
  g_assert_null(stk_catalog_filter(c, STK_CATALOG_FILTER_ALL, "no such fund xyz", &n));
  g_assert_cmpuint(n, ==, 0);
  g_assert_null(stk_catalog_filter(c, STK_CATALOG_FILTER_ALL, "FBCGFidelity", &n));
  g_assert_cmpuint(n, ==, 0);
  g_assert_null(stk_catalog_filter(c, STK_CATALOG_FILTER_ALL, "FBCG Fidelity", &n));
  g_assert_cmpuint(n, ==, 0);

  /* very long queries are fine */
  g_autofree char *long_q = g_strnfill(5000, 'x');
  g_assert_null(stk_catalog_filter(c, STK_CATALOG_FILTER_ALL, long_q, &n));
  g_assert_cmpuint(n, ==, 0);
}

/* ------------------------------------------------------------------------ */
/* parser                                                                   */
/* ------------------------------------------------------------------------ */

#define ITEM(sym, name, kind, cat) \
  "{\"symbol\":\"" sym "\",\"name\":\"" name "\",\"kind\":\"" kind "\",\"category\":\"" cat "\"}"
#define DOC(items) "{\"version\":1,\"items\":[" items "]}"

static void
expect_parse_error(const char *json)
{
  g_autoptr(GError) err = NULL;
  StkCatalog *c = stk_catalog_new_from_data(json, json != NULL ? strlen(json) : 0, &err);
  g_assert_null(c);
  g_assert_error(err, STK_ERROR, STK_ERROR_PARSE);
}

static void
test_parse_ok_and_sorting(void)
{
  const char *json = DOC(ITEM("ZZZ", "Zed Fund", "fund", "Large Blend") ","
                         ITEM("AAA", "Alpha \\u00c9TF", "etf", "") ","
                         ITEM("MMM.B", "Mid ETF", "etf", "Active Equity"));
  g_autoptr(GError) err = NULL;
  g_autoptr(StkCatalog) c = stk_catalog_new_from_data(json, strlen(json), &err);
  g_assert_no_error(err);
  g_assert_nonnull(c);

  gsize n = 0;
  const StkCatalogEntry *e = stk_catalog_entries(c, &n);
  g_assert_cmpuint(n, ==, 3);
  g_assert_cmpstr(e[0].symbol, ==, "AAA");
  g_assert_cmpstr(e[1].symbol, ==, "MMM.B");
  g_assert_cmpstr(e[2].symbol, ==, "ZZZ");
  g_assert_cmpstr(e[0].name, ==, "Alpha \xc3\x89TF");
  g_assert_cmpstr(e[0].category, ==, "");
  g_assert_cmpint(e[2].kind, ==, STK_KIND_FUND);
  g_assert_true(stk_catalog_lookup(c, "mmm.b") == &e[1]);

  /* the folded search blob is ASCII-only folding: non-ASCII matches exactly */
  g_autofree const StkCatalogEntry **hit = stk_catalog_filter(c, STK_CATALOG_FILTER_ALL, "\xc3\x89tf", &n);
  g_assert_cmpuint(n, ==, 1);
  g_assert_cmpstr(hit[0]->symbol, ==, "AAA");

  /* unknown extra members and a length-delimited (non-terminated) buffer */
  const char *extra = "{\"version\":1,\"generated\":\"x\",\"items\":[" ITEM("QQQ", "Q", "etf", "c") "]}JUNK";
  g_autoptr(StkCatalog) c2 = stk_catalog_new_from_data(extra, strlen(extra) - 4, &err);
  g_assert_no_error(err);
  g_assert_nonnull(c2);
}

static void
test_parse_rejects(void)
{
  expect_parse_error(NULL);
  expect_parse_error("");
  expect_parse_error("not json");
  expect_parse_error("{\"version\":1,\"items\":[");
  expect_parse_error("[]");
  expect_parse_error("\"x\"");
  expect_parse_error("{\"items\":[" ITEM("AAA", "A", "etf", "c") "]}");              /* no version */
  expect_parse_error("{\"version\":2,\"items\":[" ITEM("AAA", "A", "etf", "c") "]}"); /* version */
  expect_parse_error("{\"version\":\"1\",\"items\":[" ITEM("AAA", "A", "etf", "c") "]}");
  expect_parse_error("{\"version\":1}");
  expect_parse_error("{\"version\":1,\"items\":{}}");
  expect_parse_error(DOC(""));                                                       /* empty */
  expect_parse_error(DOC("1"));
  expect_parse_error(DOC("null"));
  expect_parse_error(DOC(ITEM("AAA", "A", "stock", "c")));                           /* kind */
  expect_parse_error(DOC(ITEM("AAA", "A", "ETF", "c")));
  expect_parse_error(DOC(ITEM("AAA", "A", "", "c")));
  expect_parse_error(DOC(ITEM("", "A", "etf", "c")));                                /* symbol */
  expect_parse_error(DOC(ITEM("a/b", "A", "etf", "c")));
  expect_parse_error(DOC(ITEM("../x", "A", "etf", "c")));
  expect_parse_error(DOC(ITEM("A B", "A", "etf", "c")));
  expect_parse_error(DOC(ITEM("aaa", "A", "etf", "c")));                             /* lower */
  expect_parse_error(DOC(ITEM("AAAAAAAAAAAAAAAA", "A", "etf", "c")));                /* 16 */
  expect_parse_error(DOC(ITEM("AAA", "", "etf", "c")));                              /* name */
  expect_parse_error(DOC(ITEM("AAA", "A", "etf", "c") "," ITEM("AAA", "B", "fund", "d"))); /* dup */
  expect_parse_error(DOC("{\"name\":\"A\",\"kind\":\"etf\",\"category\":\"c\"}"));   /* no symbol */
  expect_parse_error(DOC("{\"symbol\":\"AAA\",\"kind\":\"etf\",\"category\":\"c\"}")); /* no name */
  expect_parse_error(DOC("{\"symbol\":\"AAA\",\"name\":\"A\",\"category\":\"c\"}"));  /* no kind */
  expect_parse_error(DOC("{\"symbol\":\"AAA\",\"name\":\"A\",\"kind\":\"etf\"}"));    /* no category */
  expect_parse_error(DOC("{\"symbol\":\"AAA\",\"name\":7,\"kind\":\"etf\",\"category\":\"c\"}"));
  expect_parse_error(DOC("{\"symbol\":\"AAA\",\"name\":\"A\",\"kind\":\"etf\",\"category\":null}"));
  expect_parse_error(DOC("{\"symbol\":[\"AAA\"],\"name\":\"A\",\"kind\":\"etf\",\"category\":\"c\"}"));
  /* one bad item poisons the whole document */
  expect_parse_error(DOC(ITEM("AAA", "A", "etf", "c") "," ITEM("BBB", "B", "bond", "c")));

  /* error message names the problem and the domain is set */
  g_autoptr(GError) err = NULL;
  const char *bad = DOC(ITEM("aaa", "A", "etf", "c"));
  g_assert_null(stk_catalog_new_from_data(bad, strlen(bad), &err));
  g_assert_nonnull(strstr(err->message, "item 0"));

  /* a NULL error location is fine */
  g_assert_null(stk_catalog_new_from_data("nope", 4, NULL));
}

static void
test_free_null(void)
{
  stk_catalog_free(NULL);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/catalog/default/shape", test_default_shape);
  g_test_add_func("/catalog/default/membership", test_default_membership);
  g_test_add_func("/catalog/lookup", test_lookup);
  g_test_add_func("/catalog/filter/kind", test_filter_kind);
  g_test_add_func("/catalog/filter/query", test_filter_query);
  g_test_add_func("/catalog/parse/ok-and-sorting", test_parse_ok_and_sorting);
  g_test_add_func("/catalog/parse/rejects", test_parse_rejects);
  g_test_add_func("/catalog/free-null", test_free_null);
  return g_test_run();
}
