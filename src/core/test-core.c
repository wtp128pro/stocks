/* Offline unit tests for the core module (types, parse, market). */
#include <glib.h>
#include <math.h>

#include "stk-market.h"
#include "stk-parse.h"
#include "stk-types.h"

/* ------------------------------------------------------------------------ */
/* types                                                                    */
/* ------------------------------------------------------------------------ */

static void
test_error_domain(void)
{
  g_autoptr(GError) err = g_error_new(STK_ERROR, STK_ERROR_RATE_LIMITED, "slow down");
  g_assert_true(g_error_matches(err, STK_ERROR, STK_ERROR_RATE_LIMITED));
  g_assert_cmpstr(g_quark_to_string(STK_ERROR), ==, "stk-error-quark");
  g_assert_cmpint(STK_ERROR_NETWORK, ==, 0);
  g_assert_cmpint(STK_ERROR_TIMEOUT, !=, STK_ERROR_HTTP);
  g_assert_cmpint(STK_ERROR_PARSE, !=, STK_ERROR_NOT_FOUND);
  g_assert_cmpint(STK_ERROR_INVALID_ARG, !=, STK_ERROR_NETWORK);
}

static void
test_enum_strings(void)
{
  for (int k = STK_KIND_STOCK; k <= STK_KIND_FUND; k++) {
    StkKind back = STK_KIND_STOCK;
    g_assert_true(stk_kind_from_string(stk_kind_to_string((StkKind)k), &back));
    g_assert_cmpint(back, ==, k);
  }
  for (int r = STK_RANGE_1D; r <= STK_RANGE_1Y; r++) {
    StkRange back = STK_RANGE_1D;
    g_assert_true(stk_range_from_string(stk_range_to_string((StkRange)r), &back));
    g_assert_cmpint(back, ==, r);
  }
  g_assert_cmpstr(stk_kind_to_string(STK_KIND_FUND), ==, "fund");
  g_assert_cmpstr(stk_range_to_string(STK_RANGE_1Y), ==, "1Y");

  StkKind kind = STK_KIND_ETF;
  g_assert_false(stk_kind_from_string(NULL, &kind));
  g_assert_false(stk_kind_from_string("", &kind));
  g_assert_false(stk_kind_from_string("ETF", &kind)); /* case-sensitive */
  g_assert_cmpint(kind, ==, STK_KIND_ETF);            /* untouched on failure */

  StkRange range = STK_RANGE_3M;
  g_assert_false(stk_range_from_string(NULL, &range));
  g_assert_false(stk_range_from_string("12M", &range));
  g_assert_false(stk_range_from_string("1d", &range));
  g_assert_cmpint(range, ==, STK_RANGE_3M);
}

static void
test_series(void)
{
  g_autoptr(StkSeries) s = stk_series_new(STK_RANGE_1D, NAN, 0);
  g_assert_cmpint(s->range, ==, STK_RANGE_1D);
  g_assert_true(isnan(s->prev_close));
  g_assert_cmpuint(s->n_points, ==, 0);
  g_assert_null(s->points);

  g_assert_true(stk_series_append(s, 1000, 1.5));
  g_assert_true(stk_series_append(s, 1000, 1.6)); /* equal time allowed */
  g_assert_true(stk_series_append(s, 2000, -3.0));
  g_assert_cmpuint(s->n_points, ==, 3);
  g_assert_cmpint(s->points[2].t_ms, ==, 2000);
  g_assert_cmpfloat(s->points[2].v, ==, -3.0);

  /* rejected samples leave the series untouched */
  g_assert_false(stk_series_append(s, 1999, 1.0));
  g_assert_false(stk_series_append(s, 3000, NAN));
  g_assert_false(stk_series_append(s, 3000, INFINITY));
  g_assert_false(stk_series_append(s, 3000, -INFINITY));
  g_assert_cmpuint(s->n_points, ==, 3);

  /* growth past the initial allocation keeps data intact */
  for (gint64 i = 0; i < 1000; i++)
    g_assert_true(stk_series_append(s, 3000 + i, (double)i));
  g_assert_cmpuint(s->n_points, ==, 1003);
  g_assert_cmpfloat(s->points[1002].v, ==, 999.0);
  g_assert_cmpfloat(s->points[0].v, ==, 1.5);
}

static void
test_series_prev_close_and_refs(void)
{
  g_autoptr(StkSeries) a = stk_series_new(STK_RANGE_1M, 12.5, 4);
  g_assert_cmpfloat(a->prev_close, ==, 12.5);
  g_assert_true(stk_series_append(a, 1, 1.0));

  StkSeries *b = stk_series_ref(a);
  g_assert_true(b == a);
  stk_series_unref(b); /* a still alive */
  g_assert_cmpuint(a->n_points, ==, 1);

  g_autoptr(StkSeries) inf = stk_series_new(STK_RANGE_1Y, INFINITY, 0);
  g_assert_true(isnan(inf->prev_close));
}

static void
test_quote(void)
{
  g_autoptr(StkQuote) q = stk_quote_new("FBND", NULL, STK_KIND_ETF, NULL);
  g_assert_cmpstr(q->symbol, ==, "FBND");
  g_assert_cmpstr(q->name, ==, "");
  g_assert_cmpstr(q->currency, ==, "USD");
  g_assert_cmpint(q->kind, ==, STK_KIND_ETF);
  g_assert_true(isnan(q->price));
  g_assert_true(isnan(q->change));
  g_assert_true(isnan(q->change_pct));
  g_assert_cmpint(q->as_of_ms, ==, 0);
  g_assert_false(q->is_realtime);
  g_assert_false(q->market_open);

  q->price = 51.25;
  q->change = -0.04;
  q->change_pct = -0.08;
  q->as_of_ms = 123456789;
  q->is_realtime = TRUE;
  q->market_open = TRUE;

  g_autoptr(StkQuote) c = stk_quote_copy(q);
  g_assert_true(c != q);
  g_assert_true(c->symbol != q->symbol);
  g_assert_cmpstr(c->symbol, ==, "FBND");
  g_assert_cmpstr(c->currency, ==, "USD");
  g_assert_cmpfloat(c->price, ==, 51.25);
  g_assert_cmpfloat(c->change, ==, -0.04);
  g_assert_cmpint(c->as_of_ms, ==, 123456789);
  g_assert_true(c->is_realtime && c->market_open);

  g_autoptr(StkQuote) named = stk_quote_new("AAPL", "Apple Inc.", STK_KIND_STOCK, "EUR");
  g_assert_cmpstr(named->name, ==, "Apple Inc.");
  g_assert_cmpstr(named->currency, ==, "EUR");

  g_autoptr(StkQuote) lower = stk_quote_new("fbnd.x", NULL, STK_KIND_ETF, NULL);
  g_assert_cmpstr(lower->symbol, ==, "FBND.X");
  g_autoptr(StkQuote) lower_copy = stk_quote_copy(lower);
  g_assert_cmpstr(lower_copy->symbol, ==, "FBND.X");

  stk_quote_free(NULL); /* allowed */
}

static void
test_series_points_when_empty(void)
{
  /* Documented: points may be non-NULL while empty; n_points is the test. */
  g_autoptr(StkSeries) pre = stk_series_new(STK_RANGE_1D, NAN, 8);
  g_assert_cmpuint(pre->n_points, ==, 0);
  g_assert_nonnull(pre->points);
  g_autoptr(StkSeries) none = stk_series_new(STK_RANGE_1D, NAN, 0);
  g_assert_cmpuint(none->n_points, ==, 0);
  g_assert_true(stk_series_append(pre, 5, 1.0));
  g_assert_cmpuint(pre->n_points, ==, 1);
  g_assert_cmpint(pre->points[0].t_ms, ==, 5);
}

static void
test_symbol_validation(void)
{
  static const char *const good[] = {
    "AAPL", "brk.b", "^GSPC", "EURUSD=X", "BF-B", "A", "123456789012345", "A.B-C=D^E",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(good); i++)
    g_assert_true(stk_symbol_is_valid(good[i]));

  static const char *const bad[] = {
    "", "1234567890123456", "../../config", "a/b", "a b", "A\nB", "%2e", "?x", "#", "é", "..",
    ".", ".A", "-A", "=A", "A\\B", "A&B", "A\tB", "A\x7f", "AB\xc3\xa9", "A%2F",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++)
    g_assert_false(stk_symbol_is_valid(bad[i]));
  g_assert_false(stk_symbol_is_valid(NULL));

  g_autoptr(StkQuote) q = stk_quote_new("brk.b", NULL, STK_KIND_STOCK, NULL);
  g_assert_cmpstr(q->symbol, ==, "BRK.B");

  static const char *const rejected[] = { "../../config", "a/b", "", "é" };
  for (gsize i = 0; i < G_N_ELEMENTS(rejected); i++) {
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*stk_symbol_is_valid*");
    g_assert_null(stk_quote_new(rejected[i], NULL, STK_KIND_STOCK, NULL));
    g_test_assert_expected_messages();
  }
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*stk_symbol_is_valid*");
  g_assert_null(stk_quote_new(NULL, NULL, STK_KIND_STOCK, NULL));
  g_test_assert_expected_messages();
}

/* ------------------------------------------------------------------------ */
/* parse                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
  const char *text;
  StkParseResult result;
  double value;
} DoubleCase;

typedef StkParseResult (*DoubleParser)(const char *, double *);

static void
run_double_cases(DoubleParser parse, const DoubleCase *cases, gsize n)
{
  for (gsize i = 0; i < n; i++) {
    double v = 42.0;
    StkParseResult r = parse(cases[i].text, &v);
    if (r != cases[i].result)
      g_error("text '%s': expected result %d, got %d", cases[i].text ? cases[i].text : "(null)",
              cases[i].result, r);
    if (r == STK_PARSE_OK) {
      if (v != cases[i].value)
        g_error("text '%s': expected %.17g, got %.17g", cases[i].text, cases[i].value, v);
    } else if (!isnan(v)) {
      g_error("text '%s': output must be NAN on failure/NA", cases[i].text ? cases[i].text : "(null)");
    }
  }
}

#define OK(t, v) { t, STK_PARSE_OK, v }
#define NA(t) { t, STK_PARSE_NA, 0 }
#define BAD(t) { t, STK_PARSE_INVALID, 0 }

static void
test_parse_price(void)
{
  static const DoubleCase cases[] = {
    OK("$340.52", 340.52), OK("340.52", 340.52), OK("$0.00", 0.0), OK("0", 0.0),
    OK("$1,234.50", 1234.5), OK("$1,234,567.89", 1234567.89), OK("999", 999.0),
    OK("  $5.25 \n", 5.25), OK("007.5", 7.5), OK("1000", 1000.0),
    OK("$12,345", 12345.0), OK("0.000001", 0.000001),
    NA("N/A"), NA("n/a"), NA(" N/A "), NA("N/a"),
    BAD(NULL), BAD(""), BAD("   "), BAD("$"), BAD("abc"), BAD("NA"), BAD("N/A1"), BAD("N/ A"),
    BAD("-1.00"), BAD("-$1.00"), BAD("$-1.00"), BAD("+1.00"), BAD("+$1.00"), BAD("-0"),
    BAD("$$1"), BAD("1$"), BAD("$ 1"), BAD("1 2"), BAD("340.52x"), BAD("x340.52"),
    BAD("340.52%"), BAD("340.52 USD"), BAD("1."), BAD(".5"), BAD("."), BAD("1..5"),
    BAD("1.2.3"), BAD("1e5"), BAD("1E5"), BAD("0x10"), BAD("inf"), BAD("nan"),
    BAD("Infinity"), BAD("-inf"),
    BAD("1,23"), BAD("1,2345"), BAD(",123"), BAD("1,234,56"), BAD("1234,567"), BAD("12,34"),
    BAD("1,,234"), BAD("1,"), BAD("1,234,"), BAD("1.234,5"), BAD("1.2,345"), BAD("1,234.5,6"),
    BAD("\xef\xbc\x91\xef\xbc\x92"), /* full-width digits */
    BAD("1\xc2\xa0" "000"),          /* no-break space */
    BAD("\xe2\x82\xac" "5"),         /* euro sign */
    BAD("5\x01"),
  };
  run_double_cases(stk_parse_price, cases, G_N_ELEMENTS(cases));
}

static void
test_parse_price_length_limit(void)
{
  /* 63 characters is the limit */
  g_autofree char *ok63 = g_strnfill(63, '1');
  g_autofree char *bad64 = g_strnfill(64, '1');
  double v;
  g_assert_cmpint(stk_parse_price(ok63, &v), ==, STK_PARSE_OK);
  g_assert_true(isfinite(v));
  g_assert_cmpint(stk_parse_price(bad64, &v), ==, STK_PARSE_INVALID);
  g_assert_true(isnan(v));

  /* whitespace does not count against the limit */
  g_autofree char *padded = g_strdup_printf("   %s   ", ok63);
  g_assert_cmpint(stk_parse_price(padded, &v), ==, STK_PARSE_OK);

  g_autofree char *huge = g_strnfill(5000, '9');
  g_assert_cmpint(stk_parse_price(huge, &v), ==, STK_PARSE_INVALID);
}

static void
test_parse_change(void)
{
  static const DoubleCase cases[] = {
    OK("+0.24", 0.24), OK("-0.04", -0.04), OK("0.00", 0.0), OK("3.10", 3.10),
    OK("-$1.50", -1.5), OK("+$1,234.50", 1234.5), OK("$1.00", 1.0), OK("-1,000", -1000.0),
    OK("+0", 0.0), OK(" -0.5 ", -0.5),
    NA("N/A"), NA("n/a"), NA("  N/A"),
    BAD(NULL), BAD(""), BAD(" "), BAD("-"), BAD("+"), BAD("$"), BAD("-$"), BAD("+-1"),
    BAD("--1"), BAD("-+1"), BAD("$-1"), BAD("$+1"), BAD("- 1"), BAD("-1 0"), BAD("1%"),
    BAD("-0.04%"), BAD("-1."), BAD("-.5"), BAD("1x"), BAD("-1e2"), BAD("-inf"), BAD("+nan"),
    BAD("-1,23"), BAD("+1,2345"), BAD("-$$1"), BAD("N/A-1"),
  };
  run_double_cases(stk_parse_change, cases, G_N_ELEMENTS(cases));

  /* negative zero is normalised to +0.0 */
  double v;
  g_assert_cmpint(stk_parse_change("-0.00", &v), ==, STK_PARSE_OK);
  g_assert_cmpfloat(v, ==, 0.0);
  g_assert_false(signbit(v));
}

static void
test_parse_percent(void)
{
  static const DoubleCase cases[] = {
    OK("-0.04%", -0.04), OK("+0.24%", 0.24), OK("+0.24", 0.24), OK("1.5%", 1.5), OK("0%", 0.0),
    OK("0.00", 0.0), OK("12", 12.0), OK("-100%", -100.0), OK("1,234.5%", 1234.5), OK(" 3.2% ", 3.2),
    OK("-12.34", -12.34),
    NA("N/A"), NA("n/a"),
    BAD(NULL), BAD(""), BAD("%"), BAD("-%"), BAD("+"), BAD("-"), BAD("%1"), BAD("1%%"),
    BAD("1 %"), BAD("1%x"), BAD("%-1"), BAD("--1%"), BAD("+-1%"), BAD("$1%"), BAD("-$1%"),
    BAD("1."), BAD(".5%"), BAD("1e2%"), BAD("inf%"), BAD("nan"), BAD("-inf"), BAD("1,23%"),
    BAD("abc%"), BAD("1%2"),
  };
  run_double_cases(stk_parse_percent, cases, G_N_ELEMENTS(cases));

  double v;
  g_assert_cmpint(stk_parse_percent("-0.00%", &v), ==, STK_PARSE_OK);
  g_assert_false(signbit(v));
}

typedef struct {
  const char *text;
  StkParseResult result;
  guint64 value;
} VolumeCase;

static void
test_parse_volume(void)
{
  static const VolumeCase cases[] = {
    { "1,234", STK_PARSE_OK, 1234 },
    { "0", STK_PARSE_OK, 0 },
    { "48123456", STK_PARSE_OK, 48123456 },
    { "48,123,456", STK_PARSE_OK, 48123456 },
    { " 7 ", STK_PARSE_OK, 7 },
    { "999", STK_PARSE_OK, 999 },
    { "1,000", STK_PARSE_OK, 1000 },
    { "18446744073709551615", STK_PARSE_OK, G_MAXUINT64 },
    { "18,446,744,073,709,551,615", STK_PARSE_OK, G_MAXUINT64 },
    { "18446744073709551614", STK_PARSE_OK, G_MAXUINT64 - 1 },
    { "N/A", STK_PARSE_NA, 0 },
    { "n/a", STK_PARSE_NA, 0 },
    { NULL, STK_PARSE_INVALID, 0 },
    { "", STK_PARSE_INVALID, 0 },
    { "  ", STK_PARSE_INVALID, 0 },
    { "18446744073709551616", STK_PARSE_INVALID, 0 }, /* max + 1 */
    { "18446744073709551620", STK_PARSE_INVALID, 0 },
    { "99999999999999999999", STK_PARSE_INVALID, 0 },
    { "18,446,744,073,709,551,616", STK_PARSE_INVALID, 0 },
    { "-1", STK_PARSE_INVALID, 0 },
    { "+1", STK_PARSE_INVALID, 0 },
    { "$1", STK_PARSE_INVALID, 0 },
    { "1.0", STK_PARSE_INVALID, 0 },
    { "1.", STK_PARSE_INVALID, 0 },
    { "1%", STK_PARSE_INVALID, 0 },
    { "1e3", STK_PARSE_INVALID, 0 },
    { "1,23", STK_PARSE_INVALID, 0 },
    { "1,2345", STK_PARSE_INVALID, 0 },
    { ",123", STK_PARSE_INVALID, 0 },
    { "1,234,", STK_PARSE_INVALID, 0 },
    { "1 234", STK_PARSE_INVALID, 0 },
    { "12abc", STK_PARSE_INVALID, 0 },
    { "abc", STK_PARSE_INVALID, 0 },
    { "NA", STK_PARSE_INVALID, 0 },
    { "0x1F", STK_PARSE_INVALID, 0 },
    { "inf", STK_PARSE_INVALID, 0 },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    guint64 v = 77;
    StkParseResult r = stk_parse_volume(cases[i].text, &v);
    if (r != cases[i].result)
      g_error("volume '%s': expected result %d, got %d", cases[i].text ? cases[i].text : "(null)",
              cases[i].result, r);
    if (v != cases[i].value)
      g_error("volume '%s': expected %" G_GUINT64_FORMAT ", got %" G_GUINT64_FORMAT,
              cases[i].text ? cases[i].text : "(null)", cases[i].value, v);
  }
}

/* ------------------------------------------------------------------------ */
/* market                                                                   */
/* ------------------------------------------------------------------------ */

/* Epoch ms of a UTC wall-clock instant; independent of the code under test. */
static gint64
utc_ms(int y, int mo, int d, int h, int mi, int s)
{
  g_autoptr(GDateTime) dt = g_date_time_new_utc(y, mo, d, h, mi, s);
  g_assert_nonnull(dt);
  return g_date_time_to_unix(dt) * 1000;
}

static void
set_date(GDate *d, int y, int m, int day)
{
  g_date_clear(d, 1);
  g_date_set_dmy(d, (GDateDay)day, (GDateMonth)m, (GDateYear)y);
  g_assert_true(g_date_valid(d));
}

/* Collects "YYYY-MM-DD" of every day in [year] for which @pred holds. */
static char *
collect_days(int year, gboolean (*pred)(const GDate *))
{
  GString *out = g_string_new(NULL);
  GDate d;
  set_date(&d, year, 1, 1);
  while (g_date_get_year(&d) == year) {
    if (pred(&d))
      g_string_append_printf(out, "%s%04d-%02d-%02d", out->len ? " " : "", year,
                             g_date_get_month(&d), g_date_get_day(&d));
    g_date_add_days(&d, 1);
  }
  return g_string_free(out, FALSE);
}

static void
test_market_holidays_2026_2030(void)
{
  /* 2026-2028: NYSE published calendar (nyse.com/trade/hours-calendars).
   * 2029-2030: same rules applied by hand. */
  static const struct {
    int year;
    const char *holidays;
    const char *early;
  } expect[] = {
    { 2026,
      "2026-01-01 2026-01-19 2026-02-16 2026-04-03 2026-05-25 2026-06-19 2026-07-03 2026-09-07 "
      "2026-11-26 2026-12-25",
      "2026-11-27 2026-12-24" },
    { 2027,
      "2027-01-01 2027-01-18 2027-02-15 2027-03-26 2027-05-31 2027-06-18 2027-07-05 2027-09-06 "
      "2027-11-25 2027-12-24",
      "2027-11-26" },
    { 2028,
      "2028-01-17 2028-02-21 2028-04-14 2028-05-29 2028-06-19 2028-07-04 2028-09-04 2028-11-23 "
      "2028-12-25",
      "2028-07-03 2028-11-24" },
    { 2029,
      "2029-01-01 2029-01-15 2029-02-19 2029-03-30 2029-05-28 2029-06-19 2029-07-04 2029-09-03 "
      "2029-11-22 2029-12-25",
      "2029-07-03 2029-11-23 2029-12-24" },
    { 2030,
      "2030-01-01 2030-01-21 2030-02-18 2030-04-19 2030-05-27 2030-06-19 2030-07-04 2030-09-02 "
      "2030-11-28 2030-12-25",
      "2030-07-03 2030-11-29 2030-12-24" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(expect); i++) {
    g_autofree char *h = collect_days(expect[i].year, stk_market_is_holiday);
    g_autofree char *e = collect_days(expect[i].year, stk_market_is_early_close);
    g_assert_cmpstr(h, ==, expect[i].holidays);
    g_assert_cmpstr(e, ==, expect[i].early);
  }
}

static void
test_market_holiday_rules_other_years(void)
{
  /* Sunday holidays move to Monday, Saturday ones to Friday. */
  static const struct {
    int y, m, d;
    gboolean holiday;
  } cases[] = {
    { 2023, 1, 2, TRUE },   /* New Year Sunday -> Monday */
    { 2023, 1, 1, FALSE },
    { 2022, 12, 26, TRUE }, /* Christmas Sunday -> Monday */
    { 2022, 6, 20, TRUE },  /* Juneteenth Sunday -> Monday */
    { 2022, 12, 30, FALSE },
    { 2022, 7, 4, TRUE },
    { 2023, 4, 7, TRUE },   /* Good Friday 2023 */
    { 2024, 3, 29, TRUE },  /* Good Friday 2024 */
    { 2025, 4, 18, TRUE },  /* Good Friday 2025 */
    { 2025, 1, 20, TRUE },  /* MLK + Inauguration Day */
    { 2025, 11, 27, TRUE },
    { 2025, 11, 28, FALSE },/* half day, not a holiday */
    { 2027, 12, 31, FALSE },/* New Year 2028 Saturday: Friday Dec 31 trades */
    { 2027, 7, 2, FALSE },
    { 2026, 7, 2, FALSE },
    { 2026, 7, 4, FALSE },  /* Saturday itself is not reported */
    { 2026, 12, 26, FALSE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    GDate d;
    set_date(&d, cases[i].y, cases[i].m, cases[i].d);
    if (stk_market_is_holiday(&d) != cases[i].holiday)
      g_error("holiday mismatch %d-%02d-%02d (expected %d)", cases[i].y, cases[i].m, cases[i].d,
              cases[i].holiday);
  }

  /* early closes in earlier years */
  static const struct {
    int y, m, d;
    gboolean early;
  } early[] = {
    { 2023, 7, 3, TRUE }, { 2024, 7, 3, TRUE }, { 2025, 7, 3, TRUE }, { 2025, 11, 28, TRUE },
    { 2025, 12, 24, TRUE }, { 2024, 12, 24, TRUE }, { 2023, 11, 24, TRUE },
    { 2022, 7, 1, FALSE },  /* July 4 Monday: no early close */
    { 2022, 7, 3, FALSE },  /* Sunday */
    { 2023, 12, 24, FALSE },/* Sunday */
    { 2022, 12, 23, FALSE },
    { 2026, 7, 2, FALSE },  /* July 3 is the observed holiday */
    { 2026, 7, 3, FALSE },  /* holiday: not an early close */
    { 2027, 7, 2, FALSE },
    { 2027, 12, 23, FALSE },
    { 2026, 12, 25, FALSE },
    { 2026, 11, 26, FALSE }, /* Thanksgiving itself */
  };
  for (gsize i = 0; i < G_N_ELEMENTS(early); i++) {
    GDate d;
    set_date(&d, early[i].y, early[i].m, early[i].d);
    if (stk_market_is_early_close(&d) != early[i].early)
      g_error("early-close mismatch %d-%02d-%02d (expected %d)", early[i].y, early[i].m,
              early[i].d, early[i].early);
  }
}

static void
test_market_session_dates(void)
{
  GDate d;
  gint64 s = -1, e = -1;

  /* Winter (EST, UTC-5) regular day: Wed 2026-01-14 */
  set_date(&d, 2026, 1, 14);
  g_assert_true(stk_market_session(&d, &s, &e));
  g_assert_cmpint(s, ==, utc_ms(2026, 1, 14, 14, 30, 0));
  g_assert_cmpint(e, ==, utc_ms(2026, 1, 14, 21, 0, 0));

  /* Summer (EDT, UTC-4): Wed 2026-07-01 */
  set_date(&d, 2026, 7, 1);
  g_assert_true(stk_market_session(&d, &s, &e));
  g_assert_cmpint(s, ==, utc_ms(2026, 7, 1, 13, 30, 0));
  g_assert_cmpint(e, ==, utc_ms(2026, 7, 1, 20, 0, 0));

  /* DST starts Sun 2026-03-08: Fri 03-06 is EST, Mon 03-09 is EDT */
  set_date(&d, 2026, 3, 6);
  g_assert_true(stk_market_session(&d, &s, &e));
  g_assert_cmpint(s, ==, utc_ms(2026, 3, 6, 14, 30, 0));
  g_assert_cmpint(e, ==, utc_ms(2026, 3, 6, 21, 0, 0));
  set_date(&d, 2026, 3, 9);
  g_assert_true(stk_market_session(&d, &s, &e));
  g_assert_cmpint(s, ==, utc_ms(2026, 3, 9, 13, 30, 0));
  g_assert_cmpint(e, ==, utc_ms(2026, 3, 9, 20, 0, 0));

  /* DST ends Sun 2026-11-01: Fri 10-30 is EDT, Mon 11-02 is EST */
  set_date(&d, 2026, 10, 30);
  g_assert_true(stk_market_session(&d, &s, &e));
  g_assert_cmpint(s, ==, utc_ms(2026, 10, 30, 13, 30, 0));
  g_assert_cmpint(e, ==, utc_ms(2026, 10, 30, 20, 0, 0));
  set_date(&d, 2026, 11, 2);
  g_assert_true(stk_market_session(&d, &s, &e));
  g_assert_cmpint(s, ==, utc_ms(2026, 11, 2, 14, 30, 0));
  g_assert_cmpint(e, ==, utc_ms(2026, 11, 2, 21, 0, 0));

  /* Early closes at 13:00 local */
  set_date(&d, 2026, 11, 27); /* EST */
  g_assert_true(stk_market_session(&d, &s, &e));
  g_assert_cmpint(s, ==, utc_ms(2026, 11, 27, 14, 30, 0));
  g_assert_cmpint(e, ==, utc_ms(2026, 11, 27, 18, 0, 0));
  set_date(&d, 2028, 7, 3); /* EDT */
  g_assert_true(stk_market_session(&d, &s, &e));
  g_assert_cmpint(e, ==, utc_ms(2028, 7, 3, 17, 0, 0));
  set_date(&d, 2026, 12, 24);
  g_assert_true(stk_market_session(&d, &s, &e));
  g_assert_cmpint(e, ==, utc_ms(2026, 12, 24, 18, 0, 0));

  /* NULL outputs are allowed */
  set_date(&d, 2026, 1, 14);
  g_assert_true(stk_market_session(&d, NULL, NULL));

  /* No session: weekend, holidays; outputs untouched */
  s = e = 12345;
  const int closed[][3] = {
    { 2026, 1, 17 }, { 2026, 1, 18 }, /* Sat, Sun */
    { 2026, 1, 19 },                  /* MLK */
    { 2026, 4, 3 },                   /* Good Friday */
    { 2026, 7, 3 },                   /* Independence observed */
    { 2026, 11, 26 },                 /* Thanksgiving */
    { 2026, 12, 25 },
    { 2027, 6, 18 },                  /* Juneteenth observed */
    { 2027, 12, 24 },                 /* Christmas observed */
    { 2028, 1, 1 },                   /* Saturday, no observance */
  };
  for (gsize i = 0; i < G_N_ELEMENTS(closed); i++) {
    set_date(&d, closed[i][0], closed[i][1], closed[i][2]);
    g_assert_false(stk_market_session(&d, &s, &e));
  }
  g_assert_cmpint(s, ==, 12345);
  g_assert_cmpint(e, ==, 12345);

  /* Friday Dec 31 2027 trades (New Year 2028 is not observed) */
  set_date(&d, 2027, 12, 31);
  g_assert_true(stk_market_session(&d, &s, &e));
  g_assert_cmpint(s, ==, utc_ms(2027, 12, 31, 14, 30, 0));
}

static void
test_market_is_open_boundaries(void)
{
  /* Regular winter day */
  g_assert_false(stk_market_is_open(utc_ms(2026, 1, 14, 14, 29, 59) + 999));
  g_assert_true(stk_market_is_open(utc_ms(2026, 1, 14, 14, 30, 0)));
  g_assert_true(stk_market_is_open(utc_ms(2026, 1, 14, 20, 59, 59) + 999));
  g_assert_false(stk_market_is_open(utc_ms(2026, 1, 14, 21, 0, 0)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 1, 14, 3, 0, 0)));

  /* Weekend and Monday holiday */
  g_assert_false(stk_market_is_open(utc_ms(2026, 1, 17, 16, 0, 0)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 1, 18, 16, 0, 0)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 1, 19, 16, 0, 0)));
  g_assert_true(stk_market_is_open(utc_ms(2026, 1, 20, 16, 0, 0)));

  /* Good Friday, Independence observed, Thanksgiving, Christmas */
  g_assert_false(stk_market_is_open(utc_ms(2026, 4, 3, 16, 0, 0)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 7, 3, 16, 0, 0)));
  g_assert_true(stk_market_is_open(utc_ms(2026, 7, 2, 16, 0, 0)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 11, 26, 16, 0, 0)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 12, 25, 16, 0, 0)));

  /* Early close Fri 2026-11-27, 13:00 EST = 18:00Z */
  g_assert_true(stk_market_is_open(utc_ms(2026, 11, 27, 17, 59, 59) + 999));
  g_assert_false(stk_market_is_open(utc_ms(2026, 11, 27, 18, 0, 0)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 11, 27, 20, 0, 0)));
  /* Early close Thu 2026-12-24 */
  g_assert_true(stk_market_is_open(utc_ms(2026, 12, 24, 17, 0, 0)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 12, 24, 18, 0, 0)));
  /* Early close Mon 2028-07-03, 13:00 EDT = 17:00Z */
  g_assert_true(stk_market_is_open(utc_ms(2028, 7, 3, 16, 59, 59)));
  g_assert_false(stk_market_is_open(utc_ms(2028, 7, 3, 17, 0, 0)));
}

static void
test_market_dst_transitions(void)
{
  /* Fri 2026-03-06 is EST: open 14:30Z..21:00Z */
  g_assert_false(stk_market_is_open(utc_ms(2026, 3, 6, 14, 29, 59)));
  g_assert_true(stk_market_is_open(utc_ms(2026, 3, 6, 14, 30, 0)));
  g_assert_true(stk_market_is_open(utc_ms(2026, 3, 6, 20, 59, 59)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 3, 6, 21, 0, 0)));

  /* Sun 2026-03-08 DST begins (02:00 EST -> 03:00 EDT); Monday is EDT:
   * open 13:30Z..20:00Z. An EST-based implementation would say open at 14:00Z
   * and 20:30Z. */
  g_assert_false(stk_market_is_open(utc_ms(2026, 3, 8, 15, 0, 0)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 3, 9, 13, 29, 59)));
  g_assert_true(stk_market_is_open(utc_ms(2026, 3, 9, 13, 30, 0)));
  g_assert_true(stk_market_is_open(utc_ms(2026, 3, 9, 19, 59, 59)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 3, 9, 20, 0, 0)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 3, 9, 20, 30, 0)));

  /* Fri 2026-10-30 is EDT: 13:30Z..20:00Z */
  g_assert_false(stk_market_is_open(utc_ms(2026, 10, 30, 13, 29, 59)));
  g_assert_true(stk_market_is_open(utc_ms(2026, 10, 30, 13, 30, 0)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 10, 30, 20, 0, 0)));

  /* Sun 2026-11-01 DST ends (02:00 EDT -> 01:00 EST); Monday is EST:
   * open 14:30Z..21:00Z. */
  g_assert_false(stk_market_is_open(utc_ms(2026, 11, 1, 15, 0, 0)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 11, 2, 13, 30, 0)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 11, 2, 14, 29, 59)));
  g_assert_true(stk_market_is_open(utc_ms(2026, 11, 2, 14, 30, 0)));
  g_assert_true(stk_market_is_open(utc_ms(2026, 11, 2, 20, 59, 59)));
  g_assert_false(stk_market_is_open(utc_ms(2026, 11, 2, 21, 0, 0)));

  /* UTC date differs from NY date: 2026-01-15 03:00Z is Wed evening in NY */
  g_assert_false(stk_market_is_open(utc_ms(2026, 1, 15, 3, 0, 0)));
}

static void
test_market_local_date(void)
{
  GDate d;
  g_assert_true(stk_market_local_date(utc_ms(2026, 1, 15, 4, 59, 59) + 999, &d));
  g_assert_cmpint(g_date_get_day(&d), ==, 14);
  g_assert_cmpint(g_date_get_month(&d), ==, 1);
  g_assert_cmpint(g_date_get_year(&d), ==, 2026);
  g_assert_true(stk_market_local_date(utc_ms(2026, 1, 15, 5, 0, 0), &d));
  g_assert_cmpint(g_date_get_day(&d), ==, 15);

  /* EDT: midnight local = 04:00Z */
  g_assert_true(stk_market_local_date(utc_ms(2026, 7, 1, 3, 59, 59), &d));
  g_assert_cmpint(g_date_get_day(&d), ==, 30);
  g_assert_cmpint(g_date_get_month(&d), ==, 6);
  g_assert_true(stk_market_local_date(utc_ms(2026, 7, 1, 4, 0, 0), &d));
  g_assert_cmpint(g_date_get_day(&d), ==, 1);

  /* New Year in UTC is still Dec 31 in New York */
  g_assert_true(stk_market_local_date(utc_ms(2027, 1, 1, 2, 0, 0), &d));
  g_assert_cmpint(g_date_get_year(&d), ==, 2026);
  g_assert_cmpint(g_date_get_month(&d), ==, 12);
  g_assert_cmpint(g_date_get_day(&d), ==, 31);

  /* out of supported range */
  GDate untouched;
  set_date(&untouched, 2030, 5, 5);
  g_assert_false(stk_market_local_date(0, &untouched));
  g_assert_false(stk_market_local_date(-1, &untouched));
  g_assert_false(stk_market_local_date(utc_ms(2021, 12, 31, 12, 0, 0), &untouched));
  g_assert_false(stk_market_local_date(G_MAXINT64, &untouched));
  g_assert_false(stk_market_local_date(G_MININT64, &untouched));
  g_assert_cmpint(g_date_get_year(&untouched), ==, 2030);
}

static void
test_market_next_open(void)
{
  /* before the bell the same day */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 1, 14, 14, 0, 0)), ==,
                  utc_ms(2026, 1, 14, 14, 30, 0));
  /* strictly after: at the open instant and during the session -> next day */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 1, 14, 14, 29, 59) + 999), ==,
                  utc_ms(2026, 1, 14, 14, 30, 0));
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 1, 14, 14, 30, 0)), ==,
                  utc_ms(2026, 1, 15, 14, 30, 0));
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 1, 14, 18, 0, 0)), ==,
                  utc_ms(2026, 1, 15, 14, 30, 0));
  /* after the close */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 1, 14, 21, 0, 0)), ==,
                  utc_ms(2026, 1, 15, 14, 30, 0));
  /* late evening NY: UTC already next day, NY still Wed */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 1, 15, 3, 0, 0)), ==,
                  utc_ms(2026, 1, 15, 14, 30, 0));
  /* Friday evening -> Tuesday (Monday is MLK) */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 1, 16, 22, 0, 0)), ==,
                  utc_ms(2026, 1, 20, 14, 30, 0));
  /* Saturday and Sunday -> Monday */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 1, 24, 12, 0, 0)), ==,
                  utc_ms(2026, 1, 26, 14, 30, 0));
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 1, 25, 23, 0, 0)), ==,
                  utc_ms(2026, 1, 26, 14, 30, 0));
  /* Wed evening before Thanksgiving -> Friday early-close day */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 11, 25, 22, 0, 0)), ==,
                  utc_ms(2026, 11, 27, 14, 30, 0));
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 11, 26, 15, 0, 0)), ==,
                  utc_ms(2026, 11, 27, 14, 30, 0));
  /* Christmas Eve (early close) -> Mon Dec 28 (Dec 25 holiday, then weekend) */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 12, 24, 18, 30, 0)), ==,
                  utc_ms(2026, 12, 28, 14, 30, 0));
  /* Good Friday weekend */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 4, 2, 21, 0, 0)), ==,
                  utc_ms(2026, 4, 6, 13, 30, 0));
  /* July 3 2026 observed holiday: Thu 07-02 close -> Mon 07-06 */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 7, 2, 20, 30, 0)), ==,
                  utc_ms(2026, 7, 6, 13, 30, 0));
  /* DST: Friday before spring-forward -> Monday 13:30Z (EDT) */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 3, 6, 21, 30, 0)), ==,
                  utc_ms(2026, 3, 9, 13, 30, 0));
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 3, 8, 12, 0, 0)), ==,
                  utc_ms(2026, 3, 9, 13, 30, 0));
  /* DST: Friday before fall-back -> Monday 14:30Z (EST) */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 10, 30, 21, 0, 0)), ==,
                  utc_ms(2026, 11, 2, 14, 30, 0));
  /* Year boundary: New Year 2027 is a Friday holiday -> Mon Jan 4 */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2026, 12, 31, 22, 0, 0)), ==,
                  utc_ms(2027, 1, 4, 14, 30, 0));
  /* Longest closure in range: Fri 2027-12-24 holiday, Dec 25/26 weekend */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(2027, 12, 23, 22, 0, 0)), ==,
                  utc_ms(2027, 12, 27, 14, 30, 0));

  /* next_open is always strictly after now and is itself open */
  gint64 t = utc_ms(2026, 1, 1, 0, 0, 0);
  const gint64 end = utc_ms(2030, 12, 31, 0, 0, 0);
  for (; t < end; t += 7 * 3600 * 1000 + 13 * 60 * 1000) {
    gint64 n = stk_market_next_open_ms(t);
    g_assert_cmpint(n, >, t);
    g_assert_true(stk_market_is_open(n));
    g_assert_false(stk_market_is_open(n - 1));
  }

  /* out-of-range input yields -1 and a critical */
  g_test_expect_message("stocks", G_LOG_LEVEL_CRITICAL, "*should not be reached*");
  g_assert_cmpint(stk_market_next_open_ms(0), ==, -1);
  g_test_assert_expected_messages();

  /* next session beyond STK_MARKET_MAX_YEAR: quiet -1 (no critical; the test
   * harness fails on unexpected criticals) */
  g_assert_cmpint(stk_market_next_open_ms(utc_ms(STK_MARKET_MAX_YEAR, 12, 31, 22, 0, 0)), ==, -1);

  /* DST rules hold at the end of the supported range */
  GDate d;
  gint64 s, e;
  set_date(&d, STK_MARKET_MAX_YEAR, 7, 15); /* Thursday, EDT */
  g_assert_true(stk_market_session(&d, &s, &e));
  g_assert_cmpint(s, ==, utc_ms(STK_MARKET_MAX_YEAR, 7, 15, 13, 30, 0));
  set_date(&d, STK_MARKET_MAX_YEAR, 1, 14); /* Thursday, EST */
  g_assert_true(stk_market_session(&d, &s, &e));
  g_assert_cmpint(s, ==, utc_ms(STK_MARKET_MAX_YEAR, 1, 14, 14, 30, 0));
}

static void
test_market_preconditions(void)
{
  GDate d;
  set_date(&d, 2021, 12, 31); /* before STK_MARKET_MIN_YEAR */
  g_test_expect_message("stocks", G_LOG_LEVEL_CRITICAL, "*date_supported*");
  g_assert_false(stk_market_is_holiday(&d));
  g_test_assert_expected_messages();

  GDate invalid;
  g_date_clear(&invalid, 1);
  g_test_expect_message("stocks", G_LOG_LEVEL_CRITICAL, "*date_supported*");
  g_assert_false(stk_market_session(&invalid, NULL, NULL));
  g_test_assert_expected_messages();

  g_test_expect_message("stocks", G_LOG_LEVEL_CRITICAL, "*should not be reached*");
  g_assert_false(stk_market_is_open(-1));
  g_test_assert_expected_messages();
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/core/types/error-domain", test_error_domain);
  g_test_add_func("/core/types/enum-strings", test_enum_strings);
  g_test_add_func("/core/types/series", test_series);
  g_test_add_func("/core/types/series-refs", test_series_prev_close_and_refs);
  g_test_add_func("/core/types/quote", test_quote);
  g_test_add_func("/core/types/series-empty-points", test_series_points_when_empty);
  g_test_add_func("/core/types/symbol-validation", test_symbol_validation);

  g_test_add_func("/core/parse/price", test_parse_price);
  g_test_add_func("/core/parse/price-length", test_parse_price_length_limit);
  g_test_add_func("/core/parse/change", test_parse_change);
  g_test_add_func("/core/parse/percent", test_parse_percent);
  g_test_add_func("/core/parse/volume", test_parse_volume);

  g_test_add_func("/core/market/holidays-2026-2030", test_market_holidays_2026_2030);
  g_test_add_func("/core/market/holiday-rules", test_market_holiday_rules_other_years);
  g_test_add_func("/core/market/session-dates", test_market_session_dates);
  g_test_add_func("/core/market/is-open", test_market_is_open_boundaries);
  g_test_add_func("/core/market/dst", test_market_dst_transitions);
  g_test_add_func("/core/market/local-date", test_market_local_date);
  g_test_add_func("/core/market/next-open", test_market_next_open);
  g_test_add_func("/core/market/preconditions", test_market_preconditions);

  return g_test_run();
}
