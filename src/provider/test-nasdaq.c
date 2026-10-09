/*
 * test-nasdaq.c - offline tests for the Nasdaq provider.
 *
 * Part 1 feeds captured API responses (fixtures/) and synthetic bodies to the
 * pure parsers and URL builders. Part 2 runs the real asynchronous request
 * layer (StkNasdaq over libsoup) against a fake HTTP server bound to the
 * loopback interface, to exercise retries, back-off, timeouts, status mapping,
 * the body cap and cancellation. No test touches the Internet.
 *
 * Usage: test-nasdaq FIXTURE_DIR [GLib test options]
 */
#include <math.h>
#include <string.h>

#include "stk-market.h"
#include "stk-nasdaq.h"

static const char *fixture_dir;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static char *
load_fixture(const char *name, gsize *len)
{
  g_autofree char *path = g_build_filename(fixture_dir, name, NULL);
  char *contents = NULL;
  g_autoptr(GError) error = NULL;
  if (!g_file_get_contents(path, &contents, len, &error))
    g_error("cannot read fixture %s: %s", path, error->message);
  return contents;
}

static gint64
utc_ms(int year, int month, int day, int hour, int minute)
{
  GDateTime *dt = g_date_time_new_utc(year, month, day, hour, minute, 0);
  gint64 ms = g_date_time_to_unix(dt) * 1000;
  g_date_time_unref(dt);
  return ms;
}

#define BODY(str) ((const guint8 *)(str)), strlen(str)

static StkQuote *
quote_of(const char *json, GError **error)
{
  return stk_nasdaq_parse_quote(BODY(json), "TEST", STK_KIND_STOCK, error);
}

static void
assert_quote_error(const char *json, StkError code)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(StkQuote) quote = quote_of(json, &error);
  g_assert_null(quote);
  g_assert_error(error, STK_ERROR, (int)code);
}

static void
assert_ascending(const StkSeries *series)
{
  for (gsize i = 1; i < series->n_points; i++)
    g_assert_cmpint(series->points[i].t_ms, >=, series->points[i - 1].t_ms);
  for (gsize i = 0; i < series->n_points; i++)
    g_assert_true(isfinite(series->points[i].v));
}

/* ------------------------------------------------------------------------ */
/* Part 1a: quote parsing                                                   */
/* ------------------------------------------------------------------------ */

static void
test_quote_stock_fixture(void)
{
  gsize len;
  g_autofree char *body = load_fixture("quote_stock.json", &len);
  g_autoptr(GError) error = NULL;
  /* The symbol argument (lower case) wins over the response and is upper-cased. */
  g_autoptr(StkQuote) q =
      stk_nasdaq_parse_quote((const guint8 *)body, len, "aapl", STK_KIND_STOCK, &error);
  g_assert_no_error(error);
  g_assert_nonnull(q);
  g_assert_cmpstr(q->symbol, ==, "AAPL");
  g_assert_cmpstr(q->name, ==, "Apple Inc. Common Stock");
  g_assert_cmpint(q->kind, ==, STK_KIND_STOCK);
  /* Captured after-hours: the regular-session block (official close) is used. */
  g_assert_cmpfloat_with_epsilon(q->price, 340.42, 1e-9);
  g_assert_cmpfloat_with_epsilon(q->change, 3.75, 1e-9);
  g_assert_cmpfloat_with_epsilon(q->change_pct, 1.11, 1e-9);
  g_assert_cmpint(q->as_of_ms, ==, utc_ms(2026, 10, 8, 20, 0)); /* 4:00 PM EDT */
  g_assert_false(q->is_realtime);
  g_assert_false(q->market_open);
  g_assert_cmpstr(q->currency, ==, "USD");
}

static void
test_quote_etf_fixture(void)
{
  gsize len;
  g_autofree char *body = load_fixture("quote_etf.json", &len);
  g_autoptr(GError) error = NULL;
  g_autoptr(StkQuote) q =
      stk_nasdaq_parse_quote((const guint8 *)body, len, "FBND", STK_KIND_ETF, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(q->name, ==, "Fidelity Total Bond ETF");
  g_assert_cmpint(q->kind, ==, STK_KIND_ETF);
  g_assert_cmpfloat_with_epsilon(q->price, 43.47, 1e-9);
  g_assert_cmpfloat_with_epsilon(q->change, 0.15, 1e-9);
  g_assert_cmpfloat_with_epsilon(q->change_pct, 0.35, 1e-9);
  g_assert_false(q->market_open);
}

static void
test_quote_fund_fixture(void)
{
  gsize len;
  g_autofree char *body = load_fixture("quote_fund.json", &len);
  g_autoptr(GError) error = NULL;
  g_autoptr(StkQuote) q =
      stk_nasdaq_parse_quote((const guint8 *)body, len, "FXAIX", STK_KIND_FUND, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(q->name, ==, "Fidelity 500 Index Fund");
  g_assert_cmpint(q->kind, ==, STK_KIND_FUND);
  g_assert_cmpfloat_with_epsilon(q->price, 271.10, 1e-9);
  g_assert_cmpfloat_with_epsilon(q->change, -0.59, 1e-9);
  g_assert_cmpfloat_with_epsilon(q->change_pct, -0.22, 1e-9);
  /* Date-only timestamp: 12:00 New York time of that date. */
  g_assert_cmpint(q->as_of_ms, ==, utc_ms(2026, 10, 8, 16, 0));
  g_assert_false(q->is_realtime);
  g_assert_false(q->market_open);
  g_assert_cmpstr(q->currency, ==, "USD");
}

static void
test_quote_market_open_prefers_primary(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(StkQuote) q = quote_of(
      "{\"data\":{\"companyName\":\"X Inc\",\"marketStatus\":\"Open\","
      "\"primaryData\":{\"lastSalePrice\":\"$10.50\",\"netChange\":\"+0.50\","
      "\"percentageChange\":\"+5.00%\",\"lastTradeTimestamp\":\"Oct 8, 2026 10:15 AM ET\","
      "\"isRealTime\":true,\"currency\":\"EUR\"},"
      "\"secondaryData\":{\"lastSalePrice\":\"$10.00\",\"netChange\":\"+0.10\","
      "\"percentageChange\":\"+1.00%\",\"lastTradeTimestamp\":\"Closed at Oct 7, 2026 4:00 PM ET\","
      "\"isRealTime\":false}},\"status\":{\"rCode\":200}}",
      &error);
  g_assert_no_error(error);
  g_assert_cmpfloat_with_epsilon(q->price, 10.5, 1e-9);
  g_assert_cmpfloat_with_epsilon(q->change, 0.5, 1e-9);
  g_assert_cmpfloat_with_epsilon(q->change_pct, 5.0, 1e-9);
  g_assert_cmpint(q->as_of_ms, ==, utc_ms(2026, 10, 8, 14, 15));
  g_assert_true(q->is_realtime);
  g_assert_true(q->market_open);
  g_assert_cmpstr(q->currency, ==, "EUR");
  g_assert_cmpstr(q->symbol, ==, "TEST");
}

static void
test_quote_fields_missing_or_na(void)
{
  g_autoptr(GError) error = NULL;
  /* Only a price: everything else is unknown, not an error. */
  g_autoptr(StkQuote) q =
      quote_of("{\"data\":{\"primaryData\":{\"lastSalePrice\":\"$1,234.50\"}},"
               "\"status\":{\"rCode\":200}}",
               &error);
  g_assert_no_error(error);
  g_assert_cmpfloat_with_epsilon(q->price, 1234.5, 1e-9);
  g_assert_true(isnan(q->change));
  g_assert_true(isnan(q->change_pct));
  g_assert_cmpint(q->as_of_ms, ==, 0);
  g_assert_cmpstr(q->name, ==, "");
  g_assert_cmpstr(q->currency, ==, "USD");
  g_assert_false(q->is_realtime);
  g_assert_false(q->market_open);
  g_clear_pointer(&q, stk_quote_free);

  /* "N/A" price: quote without values. */
  q = quote_of("{\"data\":{\"primaryData\":{\"lastSalePrice\":\"N/A\",\"netChange\":\"+1.00\"}},"
               "\"status\":{\"rCode\":200}}",
               &error);
  g_assert_no_error(error);
  g_assert_true(isnan(q->price));
  g_assert_true(isnan(q->change));
  g_clear_pointer(&q, stk_quote_free);

  /* Unusable secondary block while closed: fall back to the primary one. */
  q = quote_of("{\"data\":{\"marketStatus\":\"Closed\","
               "\"primaryData\":{\"lastSalePrice\":\"$5.00\",\"netChange\":\"-1.00\","
               "\"percentageChange\":\"-16.67%\"},"
               "\"secondaryData\":{\"lastSalePrice\":\"\"}},\"status\":{\"rCode\":200}}",
               &error);
  g_assert_no_error(error);
  g_assert_cmpfloat_with_epsilon(q->price, 5.0, 1e-9);
  g_assert_cmpfloat_with_epsilon(q->change, -1.0, 1e-9);
  g_clear_pointer(&q, stk_quote_free);

  /* Malformed optional fields become NAN / defaults. */
  q = quote_of("{\"data\":{\"primaryData\":{\"lastSalePrice\":\"$5.00\",\"netChange\":\"abc\","
               "\"percentageChange\":7,\"lastTradeTimestamp\":\"yesterday\","
               "\"isRealTime\":\"yes\",\"currency\":\"us dollars\"}},"
               "\"status\":{\"rCode\":200}}",
               &error);
  g_assert_no_error(error);
  g_assert_true(isnan(q->change));
  g_assert_true(isnan(q->change_pct));
  g_assert_cmpint(q->as_of_ms, ==, 0);
  g_assert_false(q->is_realtime);
  g_assert_cmpstr(q->currency, ==, "USD");
}

static void
test_quote_timestamps(void)
{
  struct {
    const char *text;
    gint64 expected; /* 0 = rejected */
  } cases[] = {
      {"Oct 8, 2026 7:25 PM ET", 0},
      {"Closed at Oct 8, 2026 4:00 PM ET", 0},
      {"Jan 15, 2026 12:00 AM ET", 0},
      {"Jan 15, 2026 12:30 PM ET", 0},
      {"Jul 4, 2026", 0},
      {"Feb 30, 2026 1:00 PM ET", -1},
      {"Oct 8, 2026 13:00 PM ET", -1},
      {"Oct 8, 2026 7:25 PM", -1},
      {"Oct 8, 2026 7:25 PM ET extra", -1},
      {"Foo 8, 2026", -1},
      {"", -1},
  };
  cases[0].expected = utc_ms(2026, 10, 8, 23, 25);
  cases[1].expected = utc_ms(2026, 10, 8, 20, 0);
  cases[2].expected = utc_ms(2026, 1, 15, 5, 0);   /* EST = UTC-5 */
  cases[3].expected = utc_ms(2026, 1, 15, 17, 30);
  cases[4].expected = utc_ms(2026, 7, 4, 16, 0); /* date only: 12:00 EDT */

  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autofree char *json = g_strdup_printf(
        "{\"data\":{\"primaryData\":{\"lastSalePrice\":\"$1.00\",\"lastTradeTimestamp\":\"%s\"}},"
        "\"status\":{\"rCode\":200}}",
        cases[i].text);
    g_autoptr(GError) error = NULL;
    g_autoptr(StkQuote) q = quote_of(json, &error);
    g_assert_no_error(error);
    g_assert_cmpint(q->as_of_ms, ==, cases[i].expected < 0 ? 0 : cases[i].expected);
  }
}

static void
test_quote_errors(void)
{
  gsize len;
  g_autofree char *body = load_fixture("error_400.json", &len);
  g_autoptr(GError) error = NULL;
  g_autoptr(StkQuote) q =
      stk_nasdaq_parse_quote((const guint8 *)body, len, "FXAIX", STK_KIND_STOCK, &error);
  g_assert_null(q);
  g_assert_error(error, STK_ERROR, STK_ERROR_NOT_FOUND);
  g_assert_nonnull(strstr(error->message, "Symbol not exists"));

  assert_quote_error("", STK_ERROR_PARSE);
  assert_quote_error("not json", STK_ERROR_PARSE);
  assert_quote_error("[1,2,3]", STK_ERROR_PARSE);
  assert_quote_error("{}", STK_ERROR_PARSE);
  assert_quote_error("{\"data\":null,\"status\":{\"rCode\":200}}", STK_ERROR_PARSE);
  assert_quote_error("{\"data\":{},\"status\":{\"rCode\":200}}", STK_ERROR_PARSE);
  assert_quote_error("{\"data\":{\"primaryData\":null,\"secondaryData\":null},"
                     "\"status\":{\"rCode\":200}}",
                     STK_ERROR_PARSE);
  assert_quote_error("{\"data\":{\"primaryData\":{\"lastSalePrice\":null}},"
                     "\"status\":{\"rCode\":200}}",
                     STK_ERROR_PARSE);
  assert_quote_error("{\"data\":{\"primaryData\":{\"lastSalePrice\":\"-$3\"}},"
                     "\"status\":{\"rCode\":200}}",
                     STK_ERROR_PARSE);
  assert_quote_error("{\"data\":{\"primaryData\":{\"lastSalePrice\":12.5}},"
                     "\"status\":{\"rCode\":200}}",
                     STK_ERROR_PARSE);
  /* rCode != 200 is NOT_FOUND whatever else the body says; rCode missing is PARSE. */
  assert_quote_error("{\"data\":{\"primaryData\":{\"lastSalePrice\":\"$1\"}},"
                     "\"status\":{\"rCode\":500}}",
                     STK_ERROR_NOT_FOUND);
  assert_quote_error("{\"data\":null,\"status\":{\"rCode\":400,\"bCodeMessage\":null}}",
                     STK_ERROR_NOT_FOUND);
  assert_quote_error("{\"data\":{\"primaryData\":{\"lastSalePrice\":\"$1\"}},\"status\":{}}",
                     STK_ERROR_PARSE);
  assert_quote_error("{\"data\":{\"primaryData\":{\"lastSalePrice\":\"$1\"}},"
                     "\"status\":{\"rCode\":\"200\"}}",
                     STK_ERROR_PARSE);

  /* Truncated real response. */
  g_autofree char *quote_body = load_fixture("quote_stock.json", &len);
  g_clear_error(&error);
  q = stk_nasdaq_parse_quote((const guint8 *)quote_body, len / 2, "AAPL", STK_KIND_STOCK, &error);
  g_assert_null(q);
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&error);

  /* Invalid UTF-8 and an embedded NUL are rejected before JSON parsing. */
  static const guint8 bad_utf8[] = "{\"data\":\"\xff\"}";
  q = stk_nasdaq_parse_quote(bad_utf8, sizeof bad_utf8 - 1, "AAPL", STK_KIND_STOCK, &error);
  g_assert_null(q);
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&error);
  static const guint8 with_nul[] = "{\"data\":null,\0\"status\":{\"rCode\":200}}";
  q = stk_nasdaq_parse_quote(with_nul, sizeof with_nul - 1, "AAPL", STK_KIND_STOCK, &error);
  g_assert_null(q);
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&error);

  /* Deeply nested input must fail cleanly, not overflow the stack. */
  g_autofree char *deep = g_malloc(200000);
  memset(deep, '[', 200000);
  q = stk_nasdaq_parse_quote((const guint8 *)deep, 200000, "AAPL", STK_KIND_STOCK, &error);
  g_assert_null(q);
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&error);

  /* Over the size cap. */
  q = stk_nasdaq_parse_quote((const guint8 *)deep, STK_NASDAQ_MAX_BODY_BYTES + 1, "AAPL",
                             STK_KIND_STOCK, &error);
  g_assert_null(q);
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
}

static void
test_quote_rejects_bad_arguments(void)
{
  g_autoptr(GError) error = NULL;
  const char *body = "{\"data\":{\"primaryData\":{\"lastSalePrice\":\"$1\"}},\"status\":{\"rCode\":200}}";
  g_assert_null(stk_nasdaq_parse_quote(BODY(body), "../etc", STK_KIND_STOCK, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
  g_clear_error(&error);
  g_assert_null(stk_nasdaq_parse_quote(BODY(body), "AAPL", (StkKind)42, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
}

/* ------------------------------------------------------------------------ */
/* Part 1b: chart / historical parsing                                      */
/* ------------------------------------------------------------------------ */

static void
test_chart_1d_fixture(void)
{
  gsize len;
  g_autofree char *body = load_fixture("chart_1d_etf.json", &len);
  g_autoptr(GError) error = NULL;
  g_autoptr(StkSeries) s =
      stk_nasdaq_parse_chart((const guint8 *)body, len, STK_RANGE_1D, &error);
  g_assert_no_error(error);
  g_assert_nonnull(s);
  g_assert_cmpint(s->range, ==, STK_RANGE_1D);
  /* The response's data.previousClose ("$43.35", two sessions old after
   * hours) is ignored: prev_close comes from the quote. */
  g_assert_true(isnan(s->prev_close));

  /* The fixture holds 412 points from 4:00 AM to 6:54 PM ET; 387 of them lie
   * in [9:30, 16:00) on 2026-10-08: the sample labelled 4:00 PM is the first
   * after-hours minute. */
  g_assert_cmpuint(s->n_points, ==, 387);
  GDate day;
  g_date_clear(&day, 1);
  g_date_set_dmy(&day, 8, G_DATE_OCTOBER, 2026);
  gint64 open_ms, close_ms;
  g_assert_true(stk_market_session(&day, &open_ms, &close_ms));
  g_assert_cmpint(s->points[0].t_ms, ==, open_ms); /* "9:30 AM ET" */
  g_assert_cmpint(s->points[0].t_ms, ==, utc_ms(2026, 10, 8, 13, 30));
  g_assert_cmpfloat_with_epsilon(s->points[0].v, 43.3199, 1e-9);
  g_assert_cmpint(s->points[s->n_points - 1].t_ms, ==, close_ms - 60 * 1000); /* "3:59 PM ET" */
  g_assert_cmpfloat_with_epsilon(s->points[s->n_points - 1].v, 43.48, 1e-9);
  assert_ascending(s);
  for (gsize i = 0; i < s->n_points; i++) {
    g_assert_cmpint(s->points[i].t_ms, >=, open_ms);
    g_assert_cmpint(s->points[i].t_ms, <, close_ms);
  }
}

static char *
chart_body(const char *previous_close, const char *points)
{
  return g_strdup_printf("{\"data\":{\"previousClose\":%s,\"chart\":[%s]},\"message\":null,"
                         "\"status\":{\"rCode\":200}}",
                         previous_close, points);
}

/* Chart point whose wall-clock time (what the provider encodes) is given. */
static void
append_wall_point(GString *out, int year, int month, int day, int hour, int minute, double y)
{
  if (out->len > 0)
    g_string_append_c(out, ',');
  g_string_append_printf(out, "{\"x\":%" G_GINT64_FORMAT ",\"y\":%.2f}",
                         utc_ms(year, month, day, hour, minute), y);
}

static void
test_chart_1d_session_filter_and_dst(void)
{
  GString *pts = g_string_new(NULL);
  /* Thursday in winter (EST, UTC-5). */
  append_wall_point(pts, 2026, 1, 15, 9, 29, 1.0);  /* pre-open: dropped */
  append_wall_point(pts, 2026, 1, 15, 9, 30, 2.0);  /* first session minute, kept, 14:30Z */
  append_wall_point(pts, 2026, 1, 15, 15, 59, 3.0); /* last session minute, kept, 20:59Z */
  append_wall_point(pts, 2026, 1, 15, 16, 0, 4.0);  /* labelled with the close: after-hours, dropped */
  append_wall_point(pts, 2026, 1, 15, 16, 1, 5.0);  /* after-hours: dropped */
  g_autofree char *body = chart_body("\"$1.50\"", pts->str);
  g_autoptr(GError) error = NULL;
  g_autoptr(StkSeries) s =
      stk_nasdaq_parse_chart((const guint8 *)body, strlen(body), STK_RANGE_1D, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(s->n_points, ==, 2);
  g_assert_cmpint(s->points[0].t_ms, ==, utc_ms(2026, 1, 15, 14, 30));
  g_assert_cmpint(s->points[1].t_ms, ==, utc_ms(2026, 1, 15, 20, 59));
  g_assert_true(isnan(s->prev_close)); /* previousClose "$1.50" is ignored */

  struct {
    int y, m, d, hour, minute; /* wall clock */
    gint64 expected;           /* true instant, 0 = dropped */
  } cases[] = {
      {2026, 7, 15, 9, 30, 0},    /* summer: EDT, UTC-4 */
      {2026, 3, 9, 9, 30, 0},     /* first Monday after spring-forward */
      {2026, 11, 2, 9, 30, 0},    /* first Monday after fall-back */
      {2026, 11, 27, 12, 59, 0},  /* early close: last session minute, EST */
      {2026, 11, 27, 13, 0, -1},  /* labelled with the early close: after-hours */
      {2026, 11, 27, 13, 1, -1},  /* after the early close */
      {2026, 7, 15, 15, 59, 0},   /* last minute before the regular close, EDT */
      {2026, 7, 15, 16, 0, -1},   /* labelled with the regular close */
      {2026, 10, 10, 10, 0, -1},  /* Saturday */
      {2026, 12, 25, 10, 0, -1},  /* Christmas */
  };
  cases[0].expected = utc_ms(2026, 7, 15, 13, 30);
  cases[1].expected = utc_ms(2026, 3, 9, 13, 30);
  cases[2].expected = utc_ms(2026, 11, 2, 14, 30);
  cases[3].expected = utc_ms(2026, 11, 27, 17, 59);
  cases[6].expected = utc_ms(2026, 7, 15, 19, 59);
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_string_truncate(pts, 0);
    append_wall_point(pts, cases[i].y, cases[i].m, cases[i].d, cases[i].hour, cases[i].minute, 5.0);
    g_autofree char *b = chart_body("null", pts->str);
    g_autoptr(StkSeries) one =
        stk_nasdaq_parse_chart((const guint8 *)b, strlen(b), STK_RANGE_1D, &error);
    g_assert_no_error(error);
    g_assert_true(isnan(one->prev_close));
    if (cases[i].expected < 0) {
      g_assert_cmpuint(one->n_points, ==, 0);
    } else {
      g_assert_cmpuint(one->n_points, ==, 1);
      g_assert_cmpint(one->points[0].t_ms, ==, cases[i].expected);
    }
  }
  g_string_free(pts, TRUE);
}

static void
test_chart_1d_ignores_previous_close(void)
{
  /* data.previousClose lags until the provider's nightly roll, so whatever it
   * holds, a parsed chart leaves prev_close NAN (the quote supplies it). */
  const char *values[] = {"null", "\"N/A\"", "\"\"", "\"$0\"", "\"-5\"", "\"abc\"", "12.5",
                          "true", "{}", "[]", "\"$43.35\"", "\"$1,336.67\""};
  for (gsize i = 0; i < G_N_ELEMENTS(values); i++) {
    g_autofree char *b = chart_body(values[i], "");
    g_autoptr(GError) error = NULL;
    g_autoptr(StkSeries) s =
        stk_nasdaq_parse_chart((const guint8 *)b, strlen(b), STK_RANGE_1D, &error);
    g_assert_no_error(error);
    g_assert_nonnull(s);
    g_assert_true(isnan(s->prev_close));
  }
  static const char missing[] = "{\"data\":{\"chart\":[]},\"status\":{\"rCode\":200}}";
  g_autoptr(GError) error = NULL;
  g_autoptr(StkSeries) none =
      stk_nasdaq_parse_chart((const guint8 *)missing, strlen(missing), STK_RANGE_1D, &error);
  g_assert_no_error(error);
  g_assert_true(isnan(none->prev_close));
  g_autofree char *daily = chart_body("\"$5.00\"", "");
  g_autoptr(StkSeries) d1 =
      stk_nasdaq_parse_chart((const guint8 *)daily, strlen(daily), STK_RANGE_1M, &error);
  g_assert_true(isnan(d1->prev_close));
}

static void
test_envelope_rcode_out_of_range(void)
{
  const char *codes[] = {"1e300", "-1e300", "3000000000", "2147483648", "1.79e308", "-2147483649",
                         "404", "0"};
  for (gsize i = 0; i < G_N_ELEMENTS(codes); i++) {
    g_autofree char *b = g_strdup_printf(
        "{\"data\":{\"chart\":null},\"status\":{\"rCode\":%s}}", codes[i]);
    g_autoptr(GError) error = NULL;
    StkSeries *s = stk_nasdaq_parse_chart((const guint8 *)b, strlen(b), STK_RANGE_1M, &error);
    g_assert_null(s);
    g_assert_error(error, STK_ERROR, STK_ERROR_NOT_FOUND);
    g_assert_nonnull(strstr(error->message, "provider status "));
  }
}

static void
test_envelope_error_message_truncation(void)
{
  /* At most 80 characters of the provider's message are quoted, whole UTF-8
   * characters only, always NUL-terminated. */
  struct {
    const char *unit; /* repeated */
    guint repeat;
    guint expected_repeat;
  } cases[] = {
      {"a", 200, 80}, {"a", 81, 80}, {"a", 80, 80}, {"a", 79, 79}, {"a", 1, 1},
      {"\xc3\xa9", 200, 80}, {"\xe2\x82\xac", 81, 80}, {"\xf0\x9f\x98\x80", 100, 80},
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    GString *msg = g_string_new(NULL), *want = g_string_new("provider status 400: ");
    for (guint k = 0; k < cases[i].repeat; k++)
      g_string_append(msg, cases[i].unit);
    for (guint k = 0; k < cases[i].expected_repeat; k++)
      g_string_append(want, cases[i].unit);
    g_autofree char *b = g_strdup_printf(
        "{\"data\":null,\"status\":{\"rCode\":400,\"bCodeMessage\":[{\"errorMessage\":\"%s\"}]}}",
        msg->str);
    g_autoptr(GError) error = NULL;
    g_autoptr(StkSeries) s =
        stk_nasdaq_parse_chart((const guint8 *)b, strlen(b), STK_RANGE_1M, &error);
    g_assert_null(s);
    g_assert_error(error, STK_ERROR, STK_ERROR_NOT_FOUND);
    g_assert_cmpstr(error->message, ==, want->str);
    g_assert_true(g_utf8_validate(error->message, -1, NULL));
    g_string_free(msg, TRUE);
    g_string_free(want, TRUE);
  }
  /* No message at all, an empty one, a mistyped one. */
  const char *others[] = {"\"bCodeMessage\":null", "\"bCodeMessage\":[{\"errorMessage\":\"\"}]",
                          "\"bCodeMessage\":[{\"errorMessage\":5}]", "\"bCodeMessage\":[]"};
  const char *expected[] = {"provider status 400", "provider status 400: ", "provider status 400",
                            "provider status 400"};
  for (gsize i = 0; i < G_N_ELEMENTS(others); i++) {
    g_autofree char *b =
        g_strdup_printf("{\"data\":null,\"status\":{\"rCode\":400,%s}}", others[i]);
    g_autoptr(GError) error = NULL;
    g_assert_null(stk_nasdaq_parse_chart((const guint8 *)b, strlen(b), STK_RANGE_1M, &error));
    g_assert_cmpstr(error->message, ==, expected[i]);
  }
}

static void
test_chart_1y_fixture(void)
{
  gsize len;
  g_autofree char *body = load_fixture("chart_1y_stock.json", &len);
  g_autoptr(GError) error = NULL;
  g_autoptr(StkSeries) s =
      stk_nasdaq_parse_chart((const guint8 *)body, len, STK_RANGE_1Y, &error);
  g_assert_no_error(error);
  g_assert_cmpint(s->range, ==, STK_RANGE_1Y);
  g_assert_cmpuint(s->n_points, ==, 251);
  g_assert_true(isnan(s->prev_close));
  /* Daily points keep the provider's x: 00:00 UTC of the trading date. */
  g_assert_cmpint(s->points[0].t_ms, ==, utc_ms(2025, 10, 8, 0, 0));
  g_assert_cmpfloat_with_epsilon(s->points[0].v, 258.06, 1e-9);
  g_assert_cmpint(s->points[s->n_points - 1].t_ms, ==, utc_ms(2026, 10, 7, 0, 0));
  g_assert_cmpfloat_with_epsilon(s->points[s->n_points - 1].v, 336.67, 1e-9);
  assert_ascending(s);
}

static void
test_chart_null_and_bad_points(void)
{
  g_autoptr(GError) error = NULL;
  const char *null_chart = "{\"data\":{\"chart\":null},\"status\":{\"rCode\":200}}";
  g_autoptr(StkSeries) empty =
      stk_nasdaq_parse_chart(BODY(null_chart), STK_RANGE_1M, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(empty->n_points, ==, 0);

  const char *missing = "{\"data\":{},\"status\":{\"rCode\":200}}";
  g_assert_null(stk_nasdaq_parse_chart(BODY(missing), STK_RANGE_1M, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&error);
  const char *wrong_type = "{\"data\":{\"chart\":{}},\"status\":{\"rCode\":200}}";
  g_assert_null(stk_nasdaq_parse_chart(BODY(wrong_type), STK_RANGE_1M, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&error);
  g_assert_null(stk_nasdaq_parse_chart(BODY(null_chart), (StkRange)9, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
  g_clear_error(&error);

  const char *bad = "{\"data\":{\"chart\":[{\"x\":null,\"y\":1},{\"x\":\"a\",\"y\":1},{\"y\":2},"
                    "{\"x\":1759881600000},5,null,\"s\",{\"x\":1759881600000,\"y\":-1},"
                    "{\"x\":1759881600000,\"y\":null},{\"x\":-5,\"y\":1},{\"x\":1e30,\"y\":1},"
                    "{\"x\":1759968000000,\"y\":3.5},{\"x\":1759881600000,\"y\":9},"
                    "{\"x\":1760054400000,\"y\":4}]},\"status\":{\"rCode\":200}}";
  g_autoptr(StkSeries) s = stk_nasdaq_parse_chart(BODY(bad), STK_RANGE_3M, &error);
  g_assert_no_error(error);
  /* 3.5 and 4 survive; the earlier-dated 9 would break the ascending order. */
  g_assert_cmpuint(s->n_points, ==, 2);
  g_assert_cmpfloat_with_epsilon(s->points[0].v, 3.5, 1e-9);
  g_assert_cmpfloat_with_epsilon(s->points[1].v, 4.0, 1e-9);
}

static void
test_historical_fixture(void)
{
  gsize len;
  g_autofree char *body = load_fixture("historical_fund.json", &len);
  g_autoptr(GError) error = NULL;
  g_autoptr(StkSeries) s =
      stk_nasdaq_parse_historical((const guint8 *)body, len, STK_RANGE_1Y, &error);
  g_assert_no_error(error);
  g_assert_cmpint(s->range, ==, STK_RANGE_1Y);
  g_assert_cmpuint(s->n_points, ==, 252);
  g_assert_true(isnan(s->prev_close));
  /* Rows are newest first in the response; the series ascends. */
  g_assert_cmpint(s->points[0].t_ms, ==, utc_ms(2025, 10, 8, 0, 0));
  g_assert_cmpfloat_with_epsilon(s->points[0].v, 234.61, 1e-9);
  g_assert_cmpint(s->points[s->n_points - 1].t_ms, ==, utc_ms(2026, 10, 8, 0, 0));
  g_assert_cmpfloat_with_epsilon(s->points[s->n_points - 1].v, 269.83, 1e-9);
  assert_ascending(s);

  g_assert_null(stk_nasdaq_parse_historical((const guint8 *)body, len, STK_RANGE_1D, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
}

static void
test_historical_bad_rows(void)
{
  g_autoptr(GError) error = NULL;
  const char *body =
      "{\"data\":{\"tradesTable\":{\"rows\":["
      "{\"date\":\"10/03/2026\",\"close\":\"12.5\"},"
      "{\"date\":\"10/02/2026\",\"close\":\"N/A\"},"
      "{\"date\":\"13/02/2026\",\"close\":\"1.0\"},"
      "{\"date\":\"2026-10-01\",\"close\":\"1.0\"},"
      "{\"date\":\"10/01/2026\",\"close\":\"-1.0\"},"
      "{\"date\":\"10/01/2026\",\"close\":null},"
      "{\"date\":null,\"close\":\"1.0\"},3,null,"
      "{\"date\":\"09/30/2026\",\"close\":\"$1,000.25\"},"
      "{\"date\":\"09/29/1969\",\"close\":\"7\"}]}},\"status\":{\"rCode\":200}}";
  g_autoptr(StkSeries) s = stk_nasdaq_parse_historical(BODY(body), STK_RANGE_1M, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(s->n_points, ==, 2);
  g_assert_cmpint(s->points[0].t_ms, ==, utc_ms(2026, 9, 30, 0, 0));
  g_assert_cmpfloat_with_epsilon(s->points[0].v, 1000.25, 1e-9);
  g_assert_cmpint(s->points[1].t_ms, ==, utc_ms(2026, 10, 3, 0, 0));

  /* No rows / no table are "no data"; a missing or mistyped table is PARSE. */
  const char *no_rows = "{\"data\":{\"tradesTable\":{\"rows\":null}},\"status\":{\"rCode\":200}}";
  g_autoptr(StkSeries) e1 = stk_nasdaq_parse_historical(BODY(no_rows), STK_RANGE_1M, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(e1->n_points, ==, 0);
  const char *null_table = "{\"data\":{\"tradesTable\":null},\"status\":{\"rCode\":200}}";
  g_autoptr(StkSeries) e2 = stk_nasdaq_parse_historical(BODY(null_table), STK_RANGE_1M, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(e2->n_points, ==, 0);
  const char *missing = "{\"data\":{},\"status\":{\"rCode\":200}}";
  g_assert_null(stk_nasdaq_parse_historical(BODY(missing), STK_RANGE_1M, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&error);
  gsize len;
  g_autofree char *err_body = load_fixture("error_400.json", &len);
  g_assert_null(stk_nasdaq_parse_historical((const guint8 *)err_body, len, STK_RANGE_1M, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_NOT_FOUND);
}

/* ------------------------------------------------------------------------ */
/* Part 1c: search parsing                                                  */
/* ------------------------------------------------------------------------ */

static void
test_search_fixture(void)
{
  gsize len;
  g_autofree char *body = load_fixture("search.json", &len);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) hits = stk_nasdaq_parse_search((const guint8 *)body, len, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(hits->len, ==, 10);
  StkSearchHit *first = g_ptr_array_index(hits, 0);
  g_assert_cmpstr(first->symbol, ==, "FID");
  g_assert_cmpstr(first->name, ==, "First Trust S&P International Dividend Aristocrats ETF");
  g_assert_cmpint(first->kind, ==, STK_KIND_ETF);
  StkSearchHit *second = g_ptr_array_index(hits, 1);
  g_assert_cmpstr(second->symbol, ==, "FIDAX");
  g_assert_cmpint(second->kind, ==, STK_KIND_FUND);
  for (guint i = 0; i < hits->len; i++)
    g_assert_true(stk_symbol_is_valid(((StkSearchHit *)g_ptr_array_index(hits, i))->symbol));
}

static void
test_search_filtering(void)
{
  g_autoptr(GError) error = NULL;
  const char *body =
      "{\"data\":[{\"symbol\":\"aapl\",\"name\":\"Apple\",\"asset\":\"Stocks\"},"
      "{\"symbol\":\"BTC\",\"name\":\"Bitcoin\",\"asset\":\"CRYPTO\"},"
      "{\"symbol\":\"A/B\",\"name\":\"Evil\",\"asset\":\"STOCKS\"},"
      "{\"symbol\":null,\"name\":\"NoSym\",\"asset\":\"STOCKS\"},"
      "{\"symbol\":\"NOASSET\",\"name\":\"x\"},7,null,"
      "{\"symbol\":\"VTI\",\"name\":null,\"asset\":\"ETF\"},"
      "{\"symbol\":\"FXAIX\",\"asset\":\"MUTUALFUNDS\"}],\"status\":{\"rCode\":200}}";
  g_autoptr(GPtrArray) hits = stk_nasdaq_parse_search(BODY(body), &error);
  g_assert_no_error(error);
  g_assert_cmpuint(hits->len, ==, 3);
  StkSearchHit *h = g_ptr_array_index(hits, 0);
  g_assert_cmpstr(h->symbol, ==, "AAPL");
  g_assert_cmpint(h->kind, ==, STK_KIND_STOCK);
  h = g_ptr_array_index(hits, 1);
  g_assert_cmpstr(h->symbol, ==, "VTI");
  g_assert_cmpstr(h->name, ==, "");
  g_assert_cmpint(h->kind, ==, STK_KIND_ETF);
  h = g_ptr_array_index(hits, 2);
  g_assert_cmpint(h->kind, ==, STK_KIND_FUND);
  g_assert_cmpstr(h->name, ==, "");

  const char *null_data = "{\"data\":null,\"status\":{\"rCode\":200}}";
  g_autoptr(GPtrArray) none = stk_nasdaq_parse_search(BODY(null_data), &error);
  g_assert_no_error(error);
  g_assert_cmpuint(none->len, ==, 0);
  const char *missing = "{\"status\":{\"rCode\":200}}";
  g_assert_null(stk_nasdaq_parse_search(BODY(missing), &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&error);
  const char *wrong = "{\"data\":\"x\",\"status\":{\"rCode\":200}}";
  g_assert_null(stk_nasdaq_parse_search(BODY(wrong), &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&error);

  /* More than STK_NASDAQ_MAX_SEARCH_HITS valid hits are truncated. */
  GString *many = g_string_new("{\"data\":[");
  for (int i = 0; i < 30; i++)
    g_string_append_printf(many, "%s{\"symbol\":\"S%d\",\"name\":\"n\",\"asset\":\"STOCKS\"}",
                           i ? "," : "", i);
  g_string_append(many, "],\"status\":{\"rCode\":200}}");
  g_autoptr(GPtrArray) capped =
      stk_nasdaq_parse_search((const guint8 *)many->str, many->len, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(capped->len, ==, STK_NASDAQ_MAX_SEARCH_HITS);
  g_string_free(many, TRUE);
}

/* ------------------------------------------------------------------------ */
/* Part 1d: URL builders                                                    */
/* ------------------------------------------------------------------------ */

#define BASE "https://api.nasdaq.com"

static void
assert_url(char *url, const char *expected)
{
  g_assert_cmpstr(url, ==, expected);
  g_free(url);
}

static void
assert_series_url_invalid(const char *symbol, StkKind kind, StkRange range, const GDate *today)
{
  g_autoptr(GError) error = NULL;
  char *url = stk_nasdaq_series_url(BASE, symbol, kind, range, today, &error);
  g_assert_null(url);
  g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
}

static void
test_urls_quote_and_encoding(void)
{
  g_autoptr(GError) error = NULL;
  assert_url(stk_nasdaq_quote_url(BASE, "AAPL", STK_KIND_STOCK, &error),
             BASE "/api/quote/AAPL/info?assetclass=stocks");
  assert_url(stk_nasdaq_quote_url(BASE, "FBND", STK_KIND_ETF, &error),
             BASE "/api/quote/FBND/info?assetclass=etf");
  assert_url(stk_nasdaq_quote_url(BASE, "FXAIX", STK_KIND_FUND, &error),
             BASE "/api/quote/FXAIX/info?assetclass=mutualfunds");
  assert_url(stk_nasdaq_quote_url(BASE, "brk.b", STK_KIND_STOCK, &error),
             BASE "/api/quote/BRK.B/info?assetclass=stocks");
  assert_url(stk_nasdaq_quote_url(BASE, "^GSPC", STK_KIND_STOCK, &error),
             BASE "/api/quote/%5EGSPC/info?assetclass=stocks");
  assert_url(stk_nasdaq_quote_url(BASE, "EURUSD=X", STK_KIND_STOCK, &error),
             BASE "/api/quote/EURUSD%3DX/info?assetclass=stocks");
  g_assert_no_error(error);

  const char *bad[] = {"", "A/B", "../x", "A B", "A?assetclass=etf", "A#", "A%2F", "A&B", "-A",
                       ".", "..", "ÄÖ", "AAAAAAAAAAAAAAAA", "A\n", NULL};
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_clear_error(&error);
    g_assert_null(stk_nasdaq_quote_url(BASE, bad[i], STK_KIND_STOCK, &error));
    g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
  }
  g_clear_error(&error);
  g_assert_null(stk_nasdaq_quote_url(BASE, NULL, STK_KIND_STOCK, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
  g_clear_error(&error);
  g_assert_null(stk_nasdaq_quote_url(BASE, "AAPL", (StkKind)7, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
}

static void
test_urls_series(void)
{
  GDate today;
  g_date_clear(&today, 1);
  g_date_set_dmy(&today, 8, G_DATE_OCTOBER, 2026);

  assert_url(stk_nasdaq_series_url(BASE, "FBND", STK_KIND_ETF, STK_RANGE_1D, &today, NULL),
             BASE "/api/quote/FBND/chart?assetclass=etf");
  assert_url(stk_nasdaq_series_url(BASE, "AAPL", STK_KIND_STOCK, STK_RANGE_1D, NULL, NULL),
             BASE "/api/quote/AAPL/chart?assetclass=stocks");
  assert_url(stk_nasdaq_series_url(BASE, "AAPL", STK_KIND_STOCK, STK_RANGE_1M, &today, NULL),
             BASE "/api/quote/AAPL/chart?assetclass=stocks&charttype=real"
                  "&fromdate=2026-09-08&todate=2026-10-08");
  assert_url(stk_nasdaq_series_url(BASE, "AAPL", STK_KIND_STOCK, STK_RANGE_3M, &today, NULL),
             BASE "/api/quote/AAPL/chart?assetclass=stocks&charttype=real"
                  "&fromdate=2026-07-08&todate=2026-10-08");
  assert_url(stk_nasdaq_series_url(BASE, "FBND", STK_KIND_ETF, STK_RANGE_6M, &today, NULL),
             BASE "/api/quote/FBND/chart?assetclass=etf&charttype=real"
                  "&fromdate=2026-04-08&todate=2026-10-08");
  assert_url(stk_nasdaq_series_url(BASE, "AAPL", STK_KIND_STOCK, STK_RANGE_1Y, &today, NULL),
             BASE "/api/quote/AAPL/chart?assetclass=stocks&charttype=real"
                  "&fromdate=2025-10-08&todate=2026-10-08");
  assert_url(stk_nasdaq_series_url(BASE, "FXAIX", STK_KIND_FUND, STK_RANGE_1M, &today, NULL),
             BASE "/api/quote/FXAIX/historical?assetclass=mutualfunds"
                  "&fromdate=2026-09-08&todate=2026-10-08&limit=40");
  assert_url(stk_nasdaq_series_url(BASE, "FXAIX", STK_KIND_FUND, STK_RANGE_1Y, &today, NULL),
             BASE "/api/quote/FXAIX/historical?assetclass=mutualfunds"
                  "&fromdate=2025-10-08&todate=2026-10-08&limit=400");

  /* Month arithmetic clamps the day to the target month's length. */
  GDate end_of_month;
  g_date_clear(&end_of_month, 1);
  g_date_set_dmy(&end_of_month, 31, G_DATE_MARCH, 2026);
  assert_url(stk_nasdaq_series_url(BASE, "AAPL", STK_KIND_STOCK, STK_RANGE_1M, &end_of_month, NULL),
             BASE "/api/quote/AAPL/chart?assetclass=stocks&charttype=real"
                  "&fromdate=2026-02-28&todate=2026-03-31");
  GDate leap;
  g_date_clear(&leap, 1);
  g_date_set_dmy(&leap, 29, G_DATE_FEBRUARY, 2028);
  assert_url(stk_nasdaq_series_url(BASE, "AAPL", STK_KIND_STOCK, STK_RANGE_1Y, &leap, NULL),
             BASE "/api/quote/AAPL/chart?assetclass=stocks&charttype=real"
                  "&fromdate=2027-02-28&todate=2028-02-29");

  /* Rejected without any request: fund + 1D, bad arguments, unusable date. */
  assert_series_url_invalid("FXAIX", STK_KIND_FUND, STK_RANGE_1D, &today);
  assert_series_url_invalid("A/B", STK_KIND_STOCK, STK_RANGE_1D, &today);
  assert_series_url_invalid("AAPL", (StkKind)5, STK_RANGE_1D, &today);
  assert_series_url_invalid("AAPL", STK_KIND_STOCK, (StkRange)5, &today);
  assert_series_url_invalid("AAPL", STK_KIND_STOCK, STK_RANGE_1M, NULL);
  GDate invalid;
  g_date_clear(&invalid, 1);
  assert_series_url_invalid("AAPL", STK_KIND_STOCK, STK_RANGE_1M, &invalid);
  GDate ancient;
  g_date_clear(&ancient, 1);
  g_date_set_dmy(&ancient, 1, G_DATE_JANUARY, 2001);
  assert_series_url_invalid("AAPL", STK_KIND_STOCK, STK_RANGE_1M, &ancient);
}

static void
test_urls_search(void)
{
  g_autoptr(GError) error = NULL;
  assert_url(stk_nasdaq_search_url(BASE, "fid", &error),
             BASE "/api/autocomplete/slookup/10?search=fid");
  assert_url(stk_nasdaq_search_url(BASE, "  S&P 500\t", &error),
             BASE "/api/autocomplete/slookup/10?search=S%26P%20500");
  assert_url(stk_nasdaq_search_url(BASE, "a=b&c#d?e/f", &error),
             BASE "/api/autocomplete/slookup/10?search=a%3Db%26c%23d%3Fe%2Ff");
  assert_url(stk_nasdaq_search_url(BASE, "caf\xc3\xa9", &error),
             BASE "/api/autocomplete/slookup/10?search=caf%C3%A9");
  g_autofree char *max_len = g_strnfill(64, 'a');
  char *url = stk_nasdaq_search_url(BASE, max_len, &error);
  g_assert_nonnull(url);
  g_free(url);
  g_assert_no_error(error);

  g_autofree char *too_long = g_strnfill(65, 'a');
  const char *bad[] = {"", "   ", "\t\n", "a\x01z", "a\x7f", "\xff\xfe", too_long};
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_clear_error(&error);
    g_assert_null(stk_nasdaq_search_url(BASE, bad[i], &error));
    g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
  }
  g_clear_error(&error);
  g_assert_null(stk_nasdaq_search_url(BASE, NULL, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
}

static void
test_constructor_origin_policy(void)
{
  /* HTTPS only; plain HTTP is allowed solely towards loopback hosts. */
  const char *rejected[] = {"http://api.nasdaq.com", "http://example.com:8080",
                            "ftp://127.0.0.1", "https://api.nasdaq.com/path",
                            "https://user@api.nasdaq.com", "https://api.nasdaq.com?x=1",
                            "https://api.nasdaq.com#f", "api.nasdaq.com", "http://127.0.0.1.evil.com",
                            "http://", "file:///etc/passwd"};
  for (guint i = 0; i < G_N_ELEMENTS(rejected); i++)
    g_assert_null(stk_nasdaq_new_custom(rejected[i], 1000, 0));
  const char *accepted[] = {"https://api.nasdaq.com", "https://api.nasdaq.com/",
                            "http://127.0.0.1:8080", "http://localhost", "http://[::1]:9/"};
  for (guint i = 0; i < G_N_ELEMENTS(accepted); i++) {
    StkNasdaq *n = stk_nasdaq_new_custom(accepted[i], 1000, 0);
    g_assert_nonnull(n);
    g_assert_true(STK_IS_PROVIDER(n));
    g_object_unref(n);
  }
  StkNasdaq *def = stk_nasdaq_new();
  g_assert_true(STK_IS_PROVIDER(def));
  g_object_unref(def);
}

/* ------------------------------------------------------------------------ */
/* Part 2: request layer against a loopback fake server                     */
/* ------------------------------------------------------------------------ */

typedef enum {
  STEP_RESPOND,       /* status + body, then close */
  STEP_HANG,          /* read the request, never answer */
  STEP_CLOSE,         /* close without answering */
  STEP_BIG_STREAMED,  /* 200, no Content-Length, endless-ish 3 MiB body */
  STEP_BIG_DECLARED,  /* 200 with Content-Length above the cap */
  STEP_RAW,           /* body is the complete raw response, sent verbatim, then close */
  STEP_GZIP           /* 200, body gzip-compressed with a matching Content-Length */
} StepKind;

typedef struct {
  StepKind kind;
  guint status;
  const char *body; /* STEP_RESPOND, NUL-terminated */
} Step;

/* Requests assigned to a lane are answered by its steps in order; the last
 * step repeats. */
typedef struct {
  const Step *steps;
  guint n_steps;
  guint used;
} Lane;

typedef struct Fake Fake;

typedef struct {
  Fake *fake;
  GSocketConnection *connection;
  GString *request;
  guint8 buffer[2048];
  gsize big_remaining; /* STEP_BIG_STREAMED */
} Conn;

struct Fake {
  GSocketService *service;
  guint port;
  Lane lanes[2]; /* [0]: every request (all but /info when routed); [1]: /info when routed */
  gboolean routed;
  guint requests;
  GPtrArray *paths;      /* request target of every request (char *) */
  GPtrArray *held;       /* Conn of hanging requests */
  char *last_request;    /* complete head of the last request */
};

static void conn_read(Conn *conn);

static void
conn_free(Conn *conn)
{
  if (conn->connection != NULL) {
    (void)g_io_stream_close(G_IO_STREAM(conn->connection), NULL, NULL);
    g_object_unref(conn->connection);
  }
  g_string_free(conn->request, TRUE);
  g_free(conn);
}

static void
write_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  Conn *conn = user_data;
  g_autoptr(GError) error = NULL;
  gsize written;
  (void)g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, &written, &error);
  conn_free(conn);
}

static void
respond_text(Conn *conn, GBytes *bytes)
{
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn->connection));
  gsize len;
  const void *data = g_bytes_get_data(bytes, &len);
  /* bytes stays alive through the stream's own copy of the buffer pointer: keep
   * a reference until the write completes by attaching it to the stream. */
  g_object_set_data_full(G_OBJECT(out), "stk-test-bytes", g_bytes_ref(bytes),
                         (GDestroyNotify)g_bytes_unref);
  g_output_stream_write_all_async(out, data, len, G_PRIORITY_DEFAULT, NULL, write_done, conn);
}

static void big_chunk_done(GObject *source, GAsyncResult *result, gpointer user_data);

static void
big_next_chunk(Conn *conn)
{
  static guint8 chunk[64 * 1024];
  if (conn->big_remaining == 0) {
    conn_free(conn);
    return;
  }
  gsize n = MIN(conn->big_remaining, sizeof chunk);
  conn->big_remaining -= n;
  g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(conn->connection)),
                                  chunk, n, G_PRIORITY_DEFAULT, NULL, big_chunk_done, conn);
}

static void
big_chunk_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  Conn *conn = user_data;
  g_autoptr(GError) error = NULL;
  gsize written;
  if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, &written, &error)) {
    conn_free(conn); /* the client gave up (cap reached) */
    return;
  }
  big_next_chunk(conn);
}

static const Step *
select_step(Fake *fake, const char *path)
{
  Lane *lane = &fake->lanes[fake->routed && strstr(path, "/info?") != NULL ? 1 : 0];
  g_assert_cmpuint(lane->n_steps, >, 0);
  guint index = MIN(lane->used, lane->n_steps - 1);
  lane->used++;
  return &lane->steps[index];
}

static void
handle_request(Conn *conn)
{
  Fake *fake = conn->fake;
  g_free(fake->last_request);
  fake->last_request = g_strdup(conn->request->str);
  const char *target = conn->request->str + 4; /* after "GET " */
  const char *end = strchr(target, ' ');
  char *path = end != NULL ? g_strndup(target, (gsize)(end - target)) : g_strdup(target);
  g_ptr_array_add(fake->paths, path);
  fake->requests++;
  const Step *step = select_step(fake, path);

  switch (step->kind) {
  case STEP_RESPOND: {
    gsize body_len = strlen(step->body);
    g_autofree char *head = g_strdup_printf(
        "HTTP/1.1 %u X\r\nContent-Type: application/json\r\nContent-Length: %" G_GSIZE_FORMAT
        "\r\nConnection: close\r\n\r\n",
        step->status, body_len);
    GString *all = g_string_new(head);
    g_string_append(all, step->body);
    GBytes *bytes = g_string_free_to_bytes(all);
    respond_text(conn, bytes);
    g_bytes_unref(bytes);
    break;
  }
  case STEP_RAW: {
    GBytes *bytes = g_bytes_new_static(step->body, strlen(step->body));
    respond_text(conn, bytes);
    g_bytes_unref(bytes);
    break;
  }
  case STEP_GZIP: {
    g_autoptr(GConverter) gzip =
        G_CONVERTER(g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1));
    g_autoptr(GOutputStream) mem = g_memory_output_stream_new_resizable();
    g_autoptr(GOutputStream) zout = g_converter_output_stream_new(mem, gzip);
    g_autoptr(GError) zerror = NULL;
    g_assert_true(
        g_output_stream_write_all(zout, step->body, strlen(step->body), NULL, NULL, &zerror));
    g_assert_true(g_output_stream_close(zout, NULL, &zerror));
    g_autoptr(GBytes) packed =
        g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(mem));
    g_autofree char *head = g_strdup_printf(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\n"
        "Content-Length: %" G_GSIZE_FORMAT "\r\nConnection: close\r\n\r\n",
        g_bytes_get_size(packed));
    GByteArray *all = g_byte_array_new();
    g_byte_array_append(all, (const guint8 *)head, (guint)strlen(head));
    g_byte_array_append(all, g_bytes_get_data(packed, NULL), (guint)g_bytes_get_size(packed));
    GBytes *bytes = g_byte_array_free_to_bytes(all);
    respond_text(conn, bytes);
    g_bytes_unref(bytes);
    break;
  }
  case STEP_HANG:
    g_ptr_array_add(fake->held, conn);
    break;
  case STEP_CLOSE:
    conn_free(conn);
    break;
  case STEP_BIG_STREAMED: {
    static const char head_text[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                    "Connection: close\r\n\r\n";
    GBytes *head = g_bytes_new_static(head_text, strlen(head_text));
    conn->big_remaining = (gsize)3 * 1024 * 1024;
    /* Header first, then the body in chunks. */
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn->connection));
    g_autoptr(GError) error = NULL;
    gsize len;
    const void *data = g_bytes_get_data(head, &len);
    (void)g_output_stream_write_all(out, data, len, NULL, NULL, &error);
    g_bytes_unref(head);
    big_next_chunk(conn);
    break;
  }
  case STEP_BIG_DECLARED: {
    static const char text[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                               "Content-Length: 3000000\r\nConnection: close\r\n\r\n{}";
    GBytes *bytes = g_bytes_new_static(text, strlen(text));
    respond_text(conn, bytes);
    g_bytes_unref(bytes);
    break;
  }
  }
}

static void
read_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  Conn *conn = user_data;
  g_autoptr(GError) error = NULL;
  gssize n = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);
  if (n <= 0) {
    conn_free(conn);
    return;
  }
  g_string_append_len(conn->request, (const char *)conn->buffer, n);
  if (strstr(conn->request->str, "\r\n\r\n") != NULL)
    handle_request(conn);
  else
    conn_read(conn);
}

static void
conn_read(Conn *conn)
{
  g_input_stream_read_async(g_io_stream_get_input_stream(G_IO_STREAM(conn->connection)),
                            conn->buffer, sizeof conn->buffer, G_PRIORITY_DEFAULT, NULL, read_done,
                            conn);
}

static gboolean
on_incoming(GSocketService *service, GSocketConnection *connection, GObject *source,
            gpointer user_data)
{
  (void)service;
  (void)source;
  Conn *conn = g_new0(Conn, 1);
  conn->fake = user_data;
  conn->connection = g_object_ref(connection);
  conn->request = g_string_new(NULL);
  conn_read(conn);
  return TRUE;
}

static Fake *
fake_new_lanes(const Step *script, guint n_steps, const Step *info_script, guint n_info_steps)
{
  Fake *fake = g_new0(Fake, 1);
  fake->lanes[0] = (Lane){script, n_steps, 0};
  fake->lanes[1] = (Lane){info_script, n_info_steps, 0};
  fake->routed = info_script != NULL;
  fake->paths = g_ptr_array_new_with_free_func(g_free);
  fake->held = g_ptr_array_new();
  fake->service = g_socket_service_new();
  g_autoptr(GError) error = NULL;
  guint16 port = g_socket_listener_add_any_inet_port(G_SOCKET_LISTENER(fake->service), NULL, &error);
  g_assert_no_error(error);
  fake->port = port;
  g_signal_connect(fake->service, "incoming", G_CALLBACK(on_incoming), fake);
  g_socket_service_start(fake->service);
  return fake;
}

/* A fake whose steps answer every request, in order. */
static Fake *
fake_new(const Step *script, guint n_steps)
{
  return fake_new_lanes(script, n_steps, NULL, 0);
}

/* A fake that answers the quote (/info) requests from @info_script and all
 * other requests from @script, each in order: a 1D series issues both in
 * parallel, so one shared sequence would be racy. */
static Fake *
fake_new_routed(const Step *script, guint n_steps, const Step *info_script, guint n_info_steps)
{
  return fake_new_lanes(script, n_steps, info_script, n_info_steps);
}

static void
fake_free(Fake *fake)
{
  g_socket_service_stop(fake->service);
  g_socket_listener_close(G_SOCKET_LISTENER(fake->service));
  g_object_unref(fake->service);
  for (guint i = 0; i < fake->held->len; i++)
    conn_free(g_ptr_array_index(fake->held, i));
  g_ptr_array_unref(fake->held);
  g_ptr_array_unref(fake->paths);
  g_free(fake->last_request);
  g_free(fake);
}

static gboolean
path_requested(const Fake *fake, const char *path)
{
  for (guint i = 0; i < fake->paths->len; i++)
    if (strcmp(g_ptr_array_index(fake->paths, i), path) == 0)
      return TRUE;
  return FALSE;
}

/* First requested target that starts with @prefix, or NULL. */
static const char *
path_matching(const Fake *fake, const char *prefix)
{
  for (guint i = 0; i < fake->paths->len; i++)
    if (g_str_has_prefix(g_ptr_array_index(fake->paths, i), prefix))
      return g_ptr_array_index(fake->paths, i);
  return NULL;
}

static StkNasdaq *
provider_for(const Fake *fake, guint timeout_ms, guint backoff_ms)
{
  g_autofree char *base = g_strdup_printf("http://127.0.0.1:%u", fake->port);
  StkNasdaq *nasdaq = stk_nasdaq_new_custom(base, timeout_ms, backoff_ms);
  g_assert_nonnull(nasdaq);
  return nasdaq;
}

typedef struct {
  gboolean done;
  StkQuote *quote;
  StkSeries *series;
  GPtrArray *hits;
  GError *error;
  gint64 finished_us;
  GCancellable *cancel_before_finish; /* on_quote_done() cancels it first, if set */
} Outcome;

static void
outcome_clear(Outcome *o)
{
  g_clear_pointer(&o->quote, stk_quote_free);
  g_clear_pointer(&o->series, stk_series_unref);
  g_clear_pointer(&o->hits, g_ptr_array_unref);
  g_clear_error(&o->error);
}

static void
on_quote_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  Outcome *o = user_data;
  o->quote = stk_provider_fetch_quote_finish(STK_PROVIDER(source), result, &o->error);
  o->finished_us = g_get_monotonic_time();
  o->done = TRUE;
}

static void
on_series_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  Outcome *o = user_data;
  o->series = stk_provider_fetch_series_finish(STK_PROVIDER(source), result, &o->error);
  o->finished_us = g_get_monotonic_time();
  o->done = TRUE;
}

static void
on_search_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  Outcome *o = user_data;
  o->hits = stk_provider_search_finish(STK_PROVIDER(source), result, &o->error);
  o->finished_us = g_get_monotonic_time();
  o->done = TRUE;
}

/* Runs the default main context until @o completes; returns the elapsed time
 * from @start_us in milliseconds. */
static gint64
wait_for(Outcome *o, gint64 start_us)
{
  while (!o->done)
    g_main_context_iteration(NULL, TRUE);
  return (o->finished_us - start_us) / 1000;
}

static gboolean
cancel_later(gpointer user_data)
{
  g_cancellable_cancel(G_CANCELLABLE(user_data));
  return G_SOURCE_REMOVE;
}

static const char *const kQuoteBody =
    "{\"data\":{\"companyName\":\"Test Co\",\"marketStatus\":\"Closed\","
    "\"primaryData\":{\"lastSalePrice\":\"$12.34\",\"netChange\":\"+0.10\","
    "\"percentageChange\":\"+0.82%\",\"lastTradeTimestamp\":\"Oct 8, 2026 4:00 PM ET\","
    "\"isRealTime\":false}},\"status\":{\"rCode\":200}}";

static void
fetch_quote(StkNasdaq *n, Outcome *o, GCancellable *cancellable, gint64 *start_us)
{
  *start_us = g_get_monotonic_time();
  stk_provider_fetch_quote_async(STK_PROVIDER(n), "AAPL", STK_KIND_STOCK, cancellable,
                                 on_quote_done, o);
  g_assert_false(o->done); /* never completes synchronously */
}

static void
test_net_quote_success_and_request_shape(void)
{
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody}};
  Fake *fake = fake_new(script, 1);
  StkNasdaq *n = provider_for(fake, 2000, 10);
  Outcome o = {0};
  gint64 start;
  fetch_quote(n, &o, NULL, &start);
  wait_for(&o, start);
  g_assert_no_error(o.error);
  g_assert_nonnull(o.quote);
  g_assert_cmpstr(o.quote->symbol, ==, "AAPL");
  g_assert_cmpstr(o.quote->name, ==, "Test Co");
  g_assert_cmpfloat_with_epsilon(o.quote->price, 12.34, 1e-9);
  g_assert_cmpuint(fake->requests, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(fake->paths, 0), ==, "/api/quote/AAPL/info?assetclass=stocks");
  /* Read-only, identified, JSON, no credentials or cookies. */
  g_assert_true(g_str_has_prefix(fake->last_request, "GET "));
  g_assert_nonnull(g_strstr_len(fake->last_request, -1, "User-Agent: stocks-app/"));
  g_assert_nonnull(g_strstr_len(fake->last_request, -1, "Accept: application/json"));
  g_assert_null(g_strstr_len(fake->last_request, -1, "Cookie:"));
  g_assert_null(g_strstr_len(fake->last_request, -1, "Authorization:"));
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);
}

static void
test_net_series_and_search_paths(void)
{
  gsize chart_len, hist_len, search_len, quote_len;
  g_autofree char *chart_1d = load_fixture("chart_1d_etf.json", &chart_len);
  g_autofree char *chart_1y = load_fixture("chart_1y_stock.json", &chart_len);
  g_autofree char *hist = load_fixture("historical_fund.json", &hist_len);
  g_autofree char *search = load_fixture("search.json", &search_len);
  g_autofree char *quote = load_fixture("quote_etf.json", &quote_len);
  Step script[] = {{STEP_RESPOND, 200, chart_1d},
                   {STEP_RESPOND, 200, chart_1y},
                   {STEP_RESPOND, 200, hist},
                   {STEP_RESPOND, 200, search}};
  Step info_script[] = {{STEP_RESPOND, 200, quote}};
  Fake *fake = fake_new_routed(script, 4, info_script, 1);
  StkNasdaq *n = provider_for(fake, 5000, 10);
  Outcome o = {0};
  gint64 start = g_get_monotonic_time();

  stk_provider_fetch_series_async(STK_PROVIDER(n), "FBND", STK_KIND_ETF, STK_RANGE_1D, NULL,
                                  on_series_done, &o);
  g_assert_false(o.done);
  wait_for(&o, start);
  g_assert_no_error(o.error);
  g_assert_cmpint(o.series->range, ==, STK_RANGE_1D);
  g_assert_cmpuint(o.series->n_points, ==, 387);
  g_assert_cmpfloat_with_epsilon(o.series->prev_close, 43.32, 1e-9); /* quote 43.47 - 0.15 */
  outcome_clear(&o);
  o.done = FALSE;

  stk_provider_fetch_series_async(STK_PROVIDER(n), "AAPL", STK_KIND_STOCK, STK_RANGE_3M, NULL,
                                  on_series_done, &o);
  wait_for(&o, start);
  g_assert_no_error(o.error);
  g_assert_cmpint(o.series->range, ==, STK_RANGE_3M);
  g_assert_cmpuint(o.series->n_points, ==, 251);
  outcome_clear(&o);
  o.done = FALSE;

  stk_provider_fetch_series_async(STK_PROVIDER(n), "FXAIX", STK_KIND_FUND, STK_RANGE_1Y, NULL,
                                  on_series_done, &o);
  wait_for(&o, start);
  g_assert_no_error(o.error);
  g_assert_cmpint(o.series->range, ==, STK_RANGE_1Y);
  g_assert_cmpuint(o.series->n_points, ==, 252);
  outcome_clear(&o);
  o.done = FALSE;

  stk_provider_search_async(STK_PROVIDER(n), " S&P 500 ", NULL, on_search_done, &o);
  wait_for(&o, start);
  g_assert_no_error(o.error);
  g_assert_cmpuint(o.hits->len, ==, 10);
  outcome_clear(&o);

  g_assert_cmpuint(fake->requests, ==, 5); /* chart + quote for 1D, one per other operation */
  g_assert_true(path_requested(fake, "/api/quote/FBND/info?assetclass=etf"));
  g_assert_true(path_requested(fake, "/api/quote/FBND/chart?assetclass=etf"));
  const char *p1 = path_matching(fake, "/api/quote/AAPL/chart?");
  g_assert_nonnull(p1);
  g_assert_true(g_str_has_prefix(p1, "/api/quote/AAPL/chart?assetclass=stocks&charttype=real&fromdate="));
  g_assert_nonnull(strstr(p1, "&todate="));
  const char *p2 = path_matching(fake, "/api/quote/FXAIX/");
  g_assert_true(g_str_has_prefix(
      p2, "/api/quote/FXAIX/historical?assetclass=mutualfunds&fromdate="));
  g_assert_true(g_str_has_suffix(p2, "&limit=400"));
  g_assert_true(path_requested(fake, "/api/autocomplete/slookup/10?search=S%26P%20500"));
  g_object_unref(n);
  fake_free(fake);
}

static void
test_net_invalid_arguments_make_no_request(void)
{
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody}};
  Fake *fake = fake_new(script, 1);
  StkNasdaq *n = provider_for(fake, 2000, 10);
  Outcome o = {0};
  gint64 start = g_get_monotonic_time();

  /* Mutual fund + 1D: no intraday data. */
  stk_provider_fetch_series_async(STK_PROVIDER(n), "FXAIX", STK_KIND_FUND, STK_RANGE_1D, NULL,
                                  on_series_done, &o);
  g_assert_false(o.done);
  wait_for(&o, start);
  g_assert_null(o.series);
  g_assert_error(o.error, STK_ERROR, STK_ERROR_INVALID_ARG);
  outcome_clear(&o);
  o.done = FALSE;

  stk_provider_fetch_quote_async(STK_PROVIDER(n), "../../etc/passwd", STK_KIND_STOCK, NULL,
                                 on_quote_done, &o);
  g_assert_false(o.done);
  wait_for(&o, start);
  g_assert_null(o.quote);
  g_assert_error(o.error, STK_ERROR, STK_ERROR_INVALID_ARG);
  outcome_clear(&o);
  o.done = FALSE;

  stk_provider_search_async(STK_PROVIDER(n), "   ", NULL, on_search_done, &o);
  g_assert_false(o.done);
  wait_for(&o, start);
  g_assert_null(o.hits);
  g_assert_error(o.error, STK_ERROR, STK_ERROR_INVALID_ARG);
  outcome_clear(&o);

  g_assert_cmpuint(fake->requests, ==, 0);
  g_object_unref(n);
  fake_free(fake);
}

/* One scripted response; the operation must fail with @domain/@code after
 * exactly @expected_requests requests. */
static void
run_failure_case(Step step, StkError code, guint expected_requests)
{
  Fake *fake = fake_new(&step, 1);
  StkNasdaq *n = provider_for(fake, 2000, 5);
  Outcome o = {0};
  gint64 start;
  fetch_quote(n, &o, NULL, &start);
  wait_for(&o, start);
  g_assert_null(o.quote);
  g_assert_error(o.error, STK_ERROR, (int)code);
  g_assert_cmpuint(fake->requests, ==, expected_requests);
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);
}

static void
test_net_status_mapping(void)
{
  /* 4xx and other final failures are never retried. */
  run_failure_case((Step){STEP_RESPOND, 404, "{}"}, STK_ERROR_NOT_FOUND, 1);
  run_failure_case((Step){STEP_RESPOND, 400, "{}"}, STK_ERROR_NOT_FOUND, 1);
  run_failure_case((Step){STEP_RESPOND, 429, "{}"}, STK_ERROR_RATE_LIMITED, 1);
  run_failure_case((Step){STEP_RESPOND, 403, "{}"}, STK_ERROR_HTTP, 1);
  run_failure_case((Step){STEP_RESPOND, 301, "{}"}, STK_ERROR_HTTP, 1); /* redirects not followed */
  /* 200 with an error envelope / bad body: final, not retried. */
  gsize len;
  g_autofree char *error_body = load_fixture("error_400.json", &len);
  run_failure_case((Step){STEP_RESPOND, 200, error_body}, STK_ERROR_NOT_FOUND, 1);
  run_failure_case((Step){STEP_RESPOND, 200, "<html>blocked</html>"}, STK_ERROR_PARSE, 1);
  run_failure_case((Step){STEP_RESPOND, 200, ""}, STK_ERROR_PARSE, 1);
  /* 5xx, closed connections and silence are retried MAX_RETRIES times. */
  run_failure_case((Step){STEP_RESPOND, 500, "{}"}, STK_ERROR_HTTP, 1 + STK_NASDAQ_MAX_RETRIES);
  run_failure_case((Step){STEP_RESPOND, 503, "{}"}, STK_ERROR_HTTP, 1 + STK_NASDAQ_MAX_RETRIES);
  run_failure_case((Step){STEP_CLOSE, 0, NULL}, STK_ERROR_NETWORK, 1 + STK_NASDAQ_MAX_RETRIES);
}

static void
test_net_retry_then_success_with_backoff(void)
{
  Step script[] = {{STEP_RESPOND, 503, "{}"}, {STEP_CLOSE, 0, NULL}, {STEP_RESPOND, 200, kQuoteBody}};
  Fake *fake = fake_new(script, 3);
  StkNasdaq *n = provider_for(fake, 2000, 60); /* waits 60 ms, then 120 ms */
  Outcome o = {0};
  gint64 start;
  fetch_quote(n, &o, NULL, &start);
  gint64 elapsed_ms = wait_for(&o, start);
  g_assert_no_error(o.error);
  g_assert_nonnull(o.quote);
  g_assert_cmpuint(fake->requests, ==, 3);
  g_assert_cmpint(elapsed_ms, >=, 170); /* 60 + 120 minus timer slack */
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);
}

static void
test_net_connection_refused_is_network_error(void)
{
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody}};
  Fake *fake = fake_new(script, 1);
  StkNasdaq *n = provider_for(fake, 2000, 5);
  /* Stop listening: connections are refused. */
  g_socket_service_stop(fake->service);
  g_socket_listener_close(G_SOCKET_LISTENER(fake->service));
  Outcome o = {0};
  gint64 start;
  fetch_quote(n, &o, NULL, &start);
  wait_for(&o, start);
  g_assert_null(o.quote);
  g_assert_error(o.error, STK_ERROR, STK_ERROR_NETWORK);
  g_assert_cmpuint(fake->requests, ==, 0);
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);
}

static void
test_net_timeout(void)
{
  Step script[] = {{STEP_HANG, 0, NULL}};
  Fake *fake = fake_new(script, 1);
  StkNasdaq *n = provider_for(fake, 150, 5);
  Outcome o = {0};
  gint64 start;
  fetch_quote(n, &o, NULL, &start);
  gint64 elapsed_ms = wait_for(&o, start);
  g_assert_null(o.quote);
  g_assert_error(o.error, STK_ERROR, STK_ERROR_TIMEOUT);
  g_assert_cmpuint(fake->requests, ==, 1 + STK_NASDAQ_MAX_RETRIES);
  g_assert_cmpint(elapsed_ms, >=, 3 * 140); /* each attempt has its own deadline */
  g_assert_cmpint(elapsed_ms, <, 3000);
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);
}

static void
test_net_body_cap(void)
{
  run_failure_case((Step){STEP_BIG_DECLARED, 0, NULL}, STK_ERROR_PARSE, 1);
  run_failure_case((Step){STEP_BIG_STREAMED, 0, NULL}, STK_ERROR_PARSE, 1);
}

static void
test_net_cancel_during_request(void)
{
  Step script[] = {{STEP_HANG, 0, NULL}};
  Fake *fake = fake_new(script, 1);
  StkNasdaq *n = provider_for(fake, 10000, 5);
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  Outcome o = {0};
  gint64 start;
  fetch_quote(n, &o, cancellable, &start);
  g_timeout_add(100, cancel_later, cancellable);
  gint64 elapsed_ms = wait_for(&o, start);
  g_assert_null(o.quote);
  g_assert_error(o.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(fake->requests, ==, 1);
  g_assert_cmpint(elapsed_ms, <, 2000);
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);
}

static void
test_net_cancel_during_backoff(void)
{
  Step script[] = {{STEP_RESPOND, 503, "{}"}};
  Fake *fake = fake_new(script, 1);
  StkNasdaq *n = provider_for(fake, 2000, 30000); /* would wait 30 s */
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  Outcome o = {0};
  gint64 start;
  fetch_quote(n, &o, cancellable, &start);
  g_timeout_add(300, cancel_later, cancellable);
  gint64 elapsed_ms = wait_for(&o, start);
  g_assert_null(o.quote);
  g_assert_error(o.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(fake->requests, ==, 1);
  g_assert_cmpint(elapsed_ms, <, 3000);
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);
}

static void
test_net_already_cancelled(void)
{
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody}};
  Fake *fake = fake_new(script, 1);
  StkNasdaq *n = provider_for(fake, 2000, 5);
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  g_cancellable_cancel(cancellable);
  Outcome o = {0};
  gint64 start;
  fetch_quote(n, &o, cancellable, &start);
  wait_for(&o, start);
  g_assert_null(o.quote);
  g_assert_error(o.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(fake->requests, ==, 0);
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);
}

static void
on_quote_cancel_then_finish(GObject *source, GAsyncResult *result, gpointer user_data)
{
  Outcome *o = user_data;
  /* The request has already succeeded; cancelling before *_finish wins. */
  g_cancellable_cancel(o->cancel_before_finish);
  on_quote_done(source, result, user_data);
}

static void
test_net_cancel_before_finish(void)
{
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody}};
  Fake *fake = fake_new(script, 1);
  StkNasdaq *n = provider_for(fake, 2000, 5);
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  Outcome o = {0};
  o.cancel_before_finish = cancellable;
  stk_provider_fetch_quote_async(STK_PROVIDER(n), "AAPL", STK_KIND_STOCK, cancellable,
                                 on_quote_cancel_then_finish, &o);
  while (!o.done)
    g_main_context_iteration(NULL, TRUE);
  g_assert_null(o.quote);
  g_assert_error(o.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(fake->requests, ==, 1);
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);
}

static void
test_net_provider_dropped_while_in_flight(void)
{
  /* The operation keeps the provider alive until it completes. */
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody}};
  Fake *fake = fake_new(script, 1);
  StkNasdaq *n = provider_for(fake, 2000, 5);
  Outcome o = {0};
  gint64 start;
  fetch_quote(n, &o, NULL, &start);
  g_object_unref(n);
  wait_for(&o, start);
  g_assert_no_error(o.error);
  g_assert_nonnull(o.quote);
  outcome_clear(&o);
  fake_free(fake);
}

static void
fetch_series_1d(StkNasdaq *n, Outcome *o, GCancellable *cancellable, gint64 *start_us)
{
  *start_us = g_get_monotonic_time();
  stk_provider_fetch_series_async(STK_PROVIDER(n), "AAPL", STK_KIND_STOCK, STK_RANGE_1D,
                                  cancellable, on_series_done, o);
  g_assert_false(o->done);
}

static void
on_weak_notify(gpointer data, GObject *where_the_object_was)
{
  (void)where_the_object_was;
  *(gboolean *)data = TRUE;
}

/* Drops the caller's reference to @n and reports whether the provider was
 * finalized within @ms, i.e. whether every request it started has ended
 * (each request holds a reference to the provider). */
static gboolean
provider_released_within(StkNasdaq *n, guint ms)
{
  gboolean finalized = FALSE;
  g_object_weak_ref(G_OBJECT(n), on_weak_notify, &finalized);
  g_object_unref(n);
  gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;
  while (!finalized && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  return finalized;
}

static void
test_net_truncated_body(void)
{
  /* Declared 536 bytes, only a complete-looking 36-byte JSON body, then EOF. */
  static const char valid_prefix[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 536\r\nConnection: close\r\n\r\n"
      "{\"data\":null,\"status\":{\"rCode\":200}}";
  static const char cut_json[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\n{\"data\":";
  for (guint i = 0; i < 2; i++) {
    Step step = {STEP_RAW, 0, i == 0 ? valid_prefix : cut_json};
    Fake *fake = fake_new(&step, 1);
    StkNasdaq *n = provider_for(fake, 2000, 5);
    Outcome o = {0};
    gint64 start = g_get_monotonic_time();
    stk_provider_search_async(STK_PROVIDER(n), "abc", NULL, on_search_done, &o);
    wait_for(&o, start);
    g_assert_null(o.hits);
    g_assert_error(o.error, STK_ERROR, STK_ERROR_NETWORK);
    g_assert_cmpuint(fake->requests, ==, 1 + STK_NASDAQ_MAX_RETRIES); /* retryable */
    outcome_clear(&o);
    g_object_unref(n);
    fake_free(fake);
  }
}

static void
test_net_gzip_body(void)
{
  /* Content-Length counts the compressed bytes: it must not be compared with
   * the decoded length. */
  static const char json[] = "{\"data\":null,\"status\":{\"rCode\":200}}";
  Step step = {STEP_GZIP, 0, json};
  Fake *fake = fake_new(&step, 1);
  StkNasdaq *n = provider_for(fake, 2000, 5);
  Outcome o = {0};
  gint64 start = g_get_monotonic_time();
  stk_provider_search_async(STK_PROVIDER(n), "abc", NULL, on_search_done, &o);
  wait_for(&o, start);
  g_assert_no_error(o.error);
  g_assert_nonnull(o.hits);
  g_assert_cmpuint(o.hits->len, ==, 0);
  g_assert_cmpuint(fake->requests, ==, 1);
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);
}

static void
test_net_non_5xx_not_retried(void)
{
  run_failure_case((Step){STEP_RAW, 0,
                          "HTTP/1.1 600 X\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}"},
                   STK_ERROR_HTTP, 1);
  run_failure_case((Step){STEP_RAW, 0,
                          "HTTP/1.1 999 X\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}"},
                   STK_ERROR_HTTP, 1);
  run_failure_case((Step){STEP_RESPOND, 599, "{}"}, STK_ERROR_HTTP, 1 + STK_NASDAQ_MAX_RETRIES);
}

/* Quote bodies for the 1D tests. The first is the unit's quote_etf.json
 * capture shape: closed market, official close 43.47, netChange +0.15. */
static const char *const kQuoteOpen =
    "{\"data\":{\"companyName\":\"T\",\"marketStatus\":\"Open\","
    "\"primaryData\":{\"lastSalePrice\":\"$100.50\",\"netChange\":\"+1.50\","
    "\"percentageChange\":\"+1.5%\",\"lastTradeTimestamp\":\"Oct 8, 2026 11:00 AM ET\","
    "\"isRealTime\":true},\"secondaryData\":null},\"status\":{\"rCode\":200}}";

static char *
quote_closed_body(const char *price, const char *net_change)
{
  return g_strdup_printf(
      "{\"data\":{\"companyName\":\"T\",\"marketStatus\":\"After-Hours\","
      "\"primaryData\":{\"lastSalePrice\":\"$99\",\"netChange\":\"-1\"},"
      "\"secondaryData\":{\"lastSalePrice\":\"%s\",\"netChange\":\"%s\","
      "\"percentageChange\":\"+1%%\",\"lastTradeTimestamp\":\"Closed at Oct 8, 2026 4:00 PM ET\","
      "\"isRealTime\":false}},\"status\":{\"rCode\":200}}",
      price, net_change);
}

static void
assert_1d_series(const Outcome *o, double expected_prev_close)
{
  g_assert_no_error(o->error);
  g_assert_nonnull(o->series);
  g_assert_cmpint(o->series->range, ==, STK_RANGE_1D);
  g_assert_cmpuint(o->series->n_points, ==, 387);
  if (isnan(expected_prev_close))
    g_assert_true(isnan(o->series->prev_close));
  else
    g_assert_cmpfloat_with_epsilon(o->series->prev_close, expected_prev_close, 1e-9);
}

/* 1D series whose chart is the chart_1d_etf.json capture and whose quote
 * request is answered by @quote_step; @expected_requests counts both GETs. */
static void
run_1d_with_quote(Step quote_step, double expected_prev_close, guint expected_requests,
                  guint timeout_ms)
{
  gsize len;
  g_autofree char *chart = load_fixture("chart_1d_etf.json", &len);
  Step chart_script[] = {{STEP_RESPOND, 200, chart}};
  Fake *fake = fake_new_routed(chart_script, 1, &quote_step, 1);
  StkNasdaq *n = provider_for(fake, timeout_ms, 5);
  Outcome o = {0};
  gint64 start;
  fetch_series_1d(n, &o, NULL, &start);
  gint64 elapsed_ms = wait_for(&o, start);
  assert_1d_series(&o, expected_prev_close);
  g_assert_cmpuint(fake->requests, ==, expected_requests);
  g_assert_cmpint(elapsed_ms, <, 3000);
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);
}

static void
test_net_1d_prev_close_from_quote(void)
{
  gsize len;
  g_autofree char *chart = load_fixture("chart_1d_etf.json", &len);
  g_autofree char *quote = load_fixture("quote_etf.json", &len);
  Step chart_script[] = {{STEP_RESPOND, 200, chart}};
  Step info_script[] = {{STEP_RESPOND, 200, quote}};
  Fake *fake = fake_new_routed(chart_script, 1, info_script, 1);
  StkNasdaq *n = provider_for(fake, 2000, 5);
  Outcome o = {0};
  gint64 start;
  fetch_series_1d(n, &o, NULL, &start);
  wait_for(&o, start);
  /* The chart's own previousClose is $43.35; the previous session's close is
   * the official close 43.47 minus its netChange 0.15. */
  assert_1d_series(&o, 43.32);
  g_assert_cmpuint(fake->requests, ==, 2);
  g_assert_true(path_requested(fake, "/api/quote/AAPL/chart?assetclass=stocks"));
  g_assert_true(path_requested(fake, "/api/quote/AAPL/info?assetclass=stocks"));
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);

  /* Market open: live price minus its change. */
  run_1d_with_quote((Step){STEP_RESPOND, 200, kQuoteOpen}, 99.0, 2, 2000);
  /* Official close minus change, whatever the other block says. */
  g_autofree char *after = quote_closed_body("$340.42", "+3.75");
  run_1d_with_quote((Step){STEP_RESPOND, 200, after}, 336.67, 2, 2000);
  /* Unusable results: NAN, series still delivered. */
  g_autofree char *no_change = quote_closed_body("$43.47", "N/A");
  g_autofree char *zero = quote_closed_body("$2.00", "+2.00");
  g_autofree char *negative = quote_closed_body("$1.00", "+2.00");
  g_autofree char *no_price = quote_closed_body("N/A", "+2.00");
  run_1d_with_quote((Step){STEP_RESPOND, 200, no_change}, NAN, 2, 2000);
  run_1d_with_quote((Step){STEP_RESPOND, 200, zero}, NAN, 2, 2000);
  run_1d_with_quote((Step){STEP_RESPOND, 200, negative}, NAN, 2, 2000);
  run_1d_with_quote((Step){STEP_RESPOND, 200, no_price}, NAN, 2, 2000);
}

/* Full /info bodies for every market state the app fetches in, fed to
 * fetch_series(1D); the previous close must come out right in each. The
 * extended-hours primaryData change is measured from today's close, so
 * it must never be used. */
static void
test_net_1d_prev_close_all_market_states(void)
{
  static const struct {
    const char *status;
    StkKind kind;
    const char *symbol;
    const char *primary;   /* complete primaryData object */
    const char *secondary; /* complete secondaryData object, or "null" */
    double expected;
  } cases[] = {
      {"Open", STK_KIND_STOCK, "AAPL",
       "{\"lastSalePrice\":\"$341.00\",\"netChange\":\"+4.33\",\"percentageChange\":\"+1.29%\","
       "\"lastTradeTimestamp\":\"Oct 8, 2026 11:15 AM ET\",\"isRealTime\":true}",
       "{\"lastSalePrice\":\"$336.67\",\"netChange\":\"-0.01\",\"percentageChange\":\"-0.00%\","
       "\"lastTradeTimestamp\":\"Closed at Oct 7, 2026 4:00 PM ET\",\"isRealTime\":false}",
       336.67},
      {"Pre-Market", STK_KIND_STOCK, "AAPL",
       "{\"lastSalePrice\":\"$341.10\",\"netChange\":\"+0.68\",\"percentageChange\":\"+0.20%\","
       "\"lastTradeTimestamp\":\"Oct 8, 2026 8:15 AM ET\",\"isRealTime\":true}",
       "{\"lastSalePrice\":\"$340.42\",\"netChange\":\"+3.75\",\"percentageChange\":\"+1.11%\","
       "\"lastTradeTimestamp\":\"Closed at Oct 8, 2026 4:00 PM ET\",\"isRealTime\":false}",
       336.67},
      {"After-Hours", STK_KIND_STOCK, "AAPL",
       "{\"lastSalePrice\":\"$340.45\",\"netChange\":\"+0.03\",\"percentageChange\":\"+0.01%\","
       "\"lastTradeTimestamp\":\"Oct 8, 2026 7:25 PM ET\",\"isRealTime\":true}",
       "{\"lastSalePrice\":\"$340.42\",\"netChange\":\"+3.75\",\"percentageChange\":\"+1.11%\","
       "\"lastTradeTimestamp\":\"Closed at Oct 8, 2026 4:00 PM ET\",\"isRealTime\":false}",
       336.67},
      {"Closed", STK_KIND_STOCK, "AAPL",
       "{\"lastSalePrice\":\"$340.42\",\"netChange\":\"+3.75\",\"percentageChange\":\"+1.11%\","
       "\"lastTradeTimestamp\":\"Oct 8, 2026\",\"isRealTime\":false}",
       "null", 336.67},
      {"Open", STK_KIND_ETF, "FBND",
       "{\"lastSalePrice\":\"$43.50\",\"netChange\":\"+0.18\",\"percentageChange\":\"+0.42%\","
       "\"lastTradeTimestamp\":\"Oct 8, 2026 11:15 AM ET\",\"isRealTime\":true}",
       "{\"lastSalePrice\":\"$43.32\",\"netChange\":\"-0.03\",\"percentageChange\":\"-0.07%\","
       "\"lastTradeTimestamp\":\"Closed at Oct 7, 2026 4:00 PM ET\",\"isRealTime\":false}",
       43.32},
      {"Pre-Market", STK_KIND_ETF, "FBND",
       "{\"lastSalePrice\":\"$43.46\",\"netChange\":\"+0.01\",\"percentageChange\":\"+0.02%\","
       "\"lastTradeTimestamp\":\"Oct 8, 2026 8:15 AM ET\",\"isRealTime\":true}",
       "{\"lastSalePrice\":\"$43.45\",\"netChange\":\"+0.13\",\"percentageChange\":\"+0.30%\","
       "\"lastTradeTimestamp\":\"Closed at Oct 8, 2026 4:00 PM ET\",\"isRealTime\":false}",
       43.32},
      {"After-Hours", STK_KIND_ETF, "FBND",
       "{\"lastSalePrice\":\"$43.4508\",\"netChange\":\"-0.0192\",\"percentageChange\":\"-0.04%\","
       "\"lastTradeTimestamp\":\"Oct 8, 2026 7:25 PM ET\",\"isRealTime\":true}",
       "{\"lastSalePrice\":\"$43.45\",\"netChange\":\"+0.13\",\"percentageChange\":\"+0.30%\","
       "\"lastTradeTimestamp\":\"Closed at Oct 8, 2026 4:00 PM ET\",\"isRealTime\":false}",
       43.32},
      {"Closed", STK_KIND_ETF, "FBND",
       "{\"lastSalePrice\":\"$43.45\",\"netChange\":\"+0.13\",\"percentageChange\":\"+0.30%\","
       "\"lastTradeTimestamp\":\"Oct 8, 2026\",\"isRealTime\":false}",
       "null", 43.32},
  };
  gsize len;
  g_autofree char *chart = load_fixture("chart_1d_etf.json", &len);
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autofree char *info = g_strdup_printf(
        "{\"data\":{\"symbol\":\"%s\",\"companyName\":\"X\",\"primaryData\":%s,"
        "\"secondaryData\":%s,\"marketStatus\":\"%s\"},\"status\":{\"rCode\":200}}",
        cases[i].symbol, cases[i].primary, cases[i].secondary, cases[i].status);
    Step chart_script[] = {{STEP_RESPOND, 200, chart}};
    Step info_script[] = {{STEP_RESPOND, 200, info}};
    Fake *fake = fake_new_routed(chart_script, 1, info_script, 1);
    StkNasdaq *n = provider_for(fake, 2000, 5);
    Outcome o = {0};
    gint64 start = g_get_monotonic_time();
    stk_provider_fetch_series_async(STK_PROVIDER(n), cases[i].symbol, cases[i].kind, STK_RANGE_1D,
                                    NULL, on_series_done, &o);
    wait_for(&o, start);
    g_test_message("%s %s", cases[i].symbol, cases[i].status);
    assert_1d_series(&o, cases[i].expected);
    outcome_clear(&o);
    g_object_unref(n);
    fake_free(fake);
  }
}

static void
test_net_1d_quote_failure_keeps_series(void)
{
  /* Any quote failure leaves prev_close NAN; the quote's own retry policy
   * applies (1 + MAX_RETRIES GETs for retryable failures) plus the chart GET. */
  run_1d_with_quote((Step){STEP_RESPOND, 404, "{}"}, NAN, 2, 2000);
  run_1d_with_quote((Step){STEP_RESPOND, 429, "{}"}, NAN, 2, 2000);
  run_1d_with_quote((Step){STEP_RESPOND, 200, "<html>blocked</html>"}, NAN, 2, 2000);
  run_1d_with_quote((Step){STEP_RESPOND, 500, "{}"}, NAN, 2 + STK_NASDAQ_MAX_RETRIES, 2000);
  run_1d_with_quote((Step){STEP_CLOSE, 0, NULL}, NAN, 2 + STK_NASDAQ_MAX_RETRIES, 2000);
  /* A silent quote times out per attempt (150 ms), 3 attempts. */
  run_1d_with_quote((Step){STEP_HANG, 0, NULL}, NAN, 2 + STK_NASDAQ_MAX_RETRIES, 150);
}

static void
test_net_1d_chart_failure_fails_series(void)
{
  gsize len;
  g_autofree char *quote = load_fixture("quote_etf.json", &len);
  Step info_ok[] = {{STEP_RESPOND, 200, quote}};

  /* Chart 404 while the quote hangs: the series fails at once with the chart's
   * error and the hung quote request is cancelled (the provider is released
   * long before the quote's 10 s deadline would end it). */
  Step chart_404[] = {{STEP_RESPOND, 404, "{}"}};
  Step info_hang[] = {{STEP_HANG, 0, NULL}};
  Fake *fake = fake_new_routed(chart_404, 1, info_hang, 1);
  StkNasdaq *n = provider_for(fake, 10000, 5);
  Outcome o = {0};
  gint64 start;
  fetch_series_1d(n, &o, NULL, &start);
  gint64 elapsed_ms = wait_for(&o, start);
  g_assert_null(o.series);
  g_assert_error(o.error, STK_ERROR, STK_ERROR_NOT_FOUND);
  g_assert_cmpint(elapsed_ms, <, 2000);
  g_assert_cmpuint(fake->requests, <=, 2);
  outcome_clear(&o);
  g_assert_true(provider_released_within(n, 2000));
  fake_free(fake);

  /* Chart 5xx: retried, then fails; the already delivered quote does not rescue it. */
  Step chart_500[] = {{STEP_RESPOND, 500, "{}"}};
  fake = fake_new_routed(chart_500, 1, info_ok, 1);
  n = provider_for(fake, 2000, 5);
  o = (Outcome){0};
  fetch_series_1d(n, &o, NULL, &start);
  wait_for(&o, start);
  g_assert_null(o.series);
  g_assert_error(o.error, STK_ERROR, STK_ERROR_HTTP);
  g_assert_cmpuint(fake->requests, ==, 1 + STK_NASDAQ_MAX_RETRIES + 1);
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);

  /* Unparsable chart. */
  Step chart_bad[] = {{STEP_RESPOND, 200, "<html>blocked</html>"}};
  fake = fake_new_routed(chart_bad, 1, info_ok, 1);
  n = provider_for(fake, 2000, 5);
  o = (Outcome){0};
  fetch_series_1d(n, &o, NULL, &start);
  wait_for(&o, start);
  g_assert_null(o.series);
  g_assert_error(o.error, STK_ERROR, STK_ERROR_PARSE);
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);

  /* Silent chart: TIMEOUT after 3 attempts, quote notwithstanding. */
  Step chart_hang[] = {{STEP_HANG, 0, NULL}};
  fake = fake_new_routed(chart_hang, 1, info_ok, 1);
  n = provider_for(fake, 150, 5);
  o = (Outcome){0};
  fetch_series_1d(n, &o, NULL, &start);
  wait_for(&o, start);
  g_assert_null(o.series);
  g_assert_error(o.error, STK_ERROR, STK_ERROR_TIMEOUT);
  g_assert_cmpuint(fake->requests, ==, 1 + STK_NASDAQ_MAX_RETRIES + 1);
  outcome_clear(&o);
  g_object_unref(n);
  fake_free(fake);
}

static void
test_net_1d_cancel(void)
{
  gsize len;
  g_autofree char *chart = load_fixture("chart_1d_etf.json", &len);
  Step hang[] = {{STEP_HANG, 0, NULL}};
  Step chart_ok[] = {{STEP_RESPOND, 200, chart}};

  /* Both requests hanging. */
  Fake *fake = fake_new_routed(hang, 1, hang, 1);
  StkNasdaq *n = provider_for(fake, 10000, 5);
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  Outcome o = {0};
  gint64 start;
  fetch_series_1d(n, &o, cancellable, &start);
  g_timeout_add(100, cancel_later, cancellable);
  gint64 elapsed_ms = wait_for(&o, start);
  g_assert_null(o.series);
  g_assert_error(o.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpint(elapsed_ms, <, 2000);
  g_assert_cmpuint(fake->requests, ==, 2);
  outcome_clear(&o);
  g_assert_true(provider_released_within(n, 2000));
  fake_free(fake);

  /* Chart delivered, quote still pending: the cancellation still wins. */
  fake = fake_new_routed(chart_ok, 1, hang, 1);
  n = provider_for(fake, 10000, 5);
  g_autoptr(GCancellable) late = g_cancellable_new();
  o = (Outcome){0};
  fetch_series_1d(n, &o, late, &start);
  g_timeout_add(400, cancel_later, late);
  elapsed_ms = wait_for(&o, start);
  g_assert_null(o.series);
  g_assert_error(o.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpint(elapsed_ms, <, 2000);
  g_assert_cmpuint(fake->requests, ==, 2);
  outcome_clear(&o);
  g_assert_true(provider_released_within(n, 2000));
  fake_free(fake);

  /* Already cancelled: no request at all. */
  fake = fake_new_routed(chart_ok, 1, hang, 1);
  n = provider_for(fake, 2000, 5);
  g_autoptr(GCancellable) early = g_cancellable_new();
  g_cancellable_cancel(early);
  o = (Outcome){0};
  fetch_series_1d(n, &o, early, &start);
  wait_for(&o, start);
  g_assert_null(o.series);
  g_assert_error(o.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(fake->requests, ==, 0);
  outcome_clear(&o);
  g_assert_true(provider_released_within(n, 2000));
  fake_free(fake);
}

/* Drives the main context until every outcome in @outs is done. */
static void
wait_all(Outcome *outs, guint count)
{
  for (;;) {
    gboolean all = TRUE;
    for (guint i = 0; i < count; i++)
      all = all && outs[i].done;
    if (all)
      return;
    g_main_context_iteration(NULL, TRUE);
  }
}

static void
test_net_inflight_cap(void)
{
  Step hang[] = {{STEP_HANG, 0, NULL}};
  Fake *fake = fake_new(hang, 1);
  StkNasdaq *n = provider_for(fake, 10000, 5);
  GCancellable *cancels[STK_NASDAQ_MAX_INFLIGHT];
  Outcome outs[STK_NASDAQ_MAX_INFLIGHT] = {0};
  gint64 start;

  /* Fill the cap minus one with hanging quotes. */
  for (guint i = 0; i < STK_NASDAQ_MAX_INFLIGHT - 1; i++) {
    cancels[i] = g_cancellable_new();
    stk_provider_fetch_quote_async(STK_PROVIDER(n), "AAPL", STK_KIND_STOCK, cancels[i],
                                   on_quote_done, &outs[i]);
  }

  /* A 1D series needs two slots: refused through the callback, nothing sent. */
  guint sent_before = fake->requests;
  Outcome refused = {0};
  fetch_series_1d(n, &refused, NULL, &start);
  gint64 elapsed_ms = wait_for(&refused, start);
  g_assert_null(refused.series);
  g_assert_error(refused.error, STK_ERROR, STK_ERROR_RATE_LIMITED);
  g_assert_cmpint(elapsed_ms, <, 1000);
  outcome_clear(&refused);

  /* Invalid arguments are still reported as such. */
  refused = (Outcome){0};
  stk_provider_fetch_quote_async(STK_PROVIDER(n), "../x", STK_KIND_STOCK, NULL, on_quote_done,
                                 &refused);
  wait_for(&refused, g_get_monotonic_time());
  g_assert_error(refused.error, STK_ERROR, STK_ERROR_INVALID_ARG);
  outcome_clear(&refused);

  /* One slot is left: this quote is accepted ... */
  cancels[STK_NASDAQ_MAX_INFLIGHT - 1] = g_cancellable_new();
  stk_provider_fetch_quote_async(STK_PROVIDER(n), "AAPL", STK_KIND_STOCK,
                                 cancels[STK_NASDAQ_MAX_INFLIGHT - 1], on_quote_done,
                                 &outs[STK_NASDAQ_MAX_INFLIGHT - 1]);
  /* ... and the next request of every kind is refused. */
  refused = (Outcome){0};
  fetch_quote(n, &refused, NULL, &start);
  wait_for(&refused, start);
  g_assert_null(refused.quote);
  g_assert_error(refused.error, STK_ERROR, STK_ERROR_RATE_LIMITED);
  outcome_clear(&refused);
  refused = (Outcome){0};
  stk_provider_search_async(STK_PROVIDER(n), "abc", NULL, on_search_done, &refused);
  wait_for(&refused, g_get_monotonic_time());
  g_assert_error(refused.error, STK_ERROR, STK_ERROR_RATE_LIMITED);
  outcome_clear(&refused);
  refused = (Outcome){0};
  stk_provider_fetch_series_async(STK_PROVIDER(n), "AAPL", STK_KIND_STOCK, STK_RANGE_1Y, NULL,
                                  on_series_done, &refused);
  wait_for(&refused, g_get_monotonic_time());
  g_assert_error(refused.error, STK_ERROR, STK_ERROR_RATE_LIMITED);
  outcome_clear(&refused);
  g_assert_cmpuint(fake->requests - sent_before, <=, STK_NASDAQ_MAX_INFLIGHT);

  /* Completed operations release their slots. */
  for (guint i = 0; i < STK_NASDAQ_MAX_INFLIGHT; i++)
    g_cancellable_cancel(cancels[i]);
  wait_all(outs, STK_NASDAQ_MAX_INFLIGHT);
  for (guint i = 0; i < STK_NASDAQ_MAX_INFLIGHT; i++) {
    g_assert_error(outs[i].error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    outcome_clear(&outs[i]);
    g_object_unref(cancels[i]);
  }
  g_autoptr(GCancellable) again = g_cancellable_new();
  Outcome accepted = {0};
  fetch_quote(n, &accepted, again, &start);
  g_cancellable_cancel(again);
  wait_for(&accepted, start);
  g_assert_error(accepted.error, G_IO_ERROR, G_IO_ERROR_CANCELLED); /* not RATE_LIMITED */
  outcome_clear(&accepted);
  g_assert_true(provider_released_within(n, 2000));
  fake_free(fake);
}

/* ------------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  if (argc < 2) {
    g_printerr("usage: %s FIXTURE_DIR\n", argv[0]);
    return 2;
  }
  fixture_dir = argv[1];
  /* Loopback traffic must never be sent through a user's proxy settings. */
  g_setenv("GIO_USE_PROXY_RESOLVER", "dummy", TRUE);

  g_test_add_func("/nasdaq/quote/stock-fixture", test_quote_stock_fixture);
  g_test_add_func("/nasdaq/quote/etf-fixture", test_quote_etf_fixture);
  g_test_add_func("/nasdaq/quote/fund-fixture", test_quote_fund_fixture);
  g_test_add_func("/nasdaq/quote/market-open", test_quote_market_open_prefers_primary);
  g_test_add_func("/nasdaq/quote/missing-or-na", test_quote_fields_missing_or_na);
  g_test_add_func("/nasdaq/quote/timestamps", test_quote_timestamps);
  g_test_add_func("/nasdaq/quote/errors", test_quote_errors);
  g_test_add_func("/nasdaq/quote/bad-arguments", test_quote_rejects_bad_arguments);
  g_test_add_func("/nasdaq/chart/1d-fixture", test_chart_1d_fixture);
  g_test_add_func("/nasdaq/chart/1d-session-dst", test_chart_1d_session_filter_and_dst);
  g_test_add_func("/nasdaq/chart/1d-ignores-previous-close", test_chart_1d_ignores_previous_close);
  g_test_add_func("/nasdaq/envelope/message-truncation", test_envelope_error_message_truncation);
  g_test_add_func("/nasdaq/envelope/rcode-range", test_envelope_rcode_out_of_range);
  g_test_add_func("/nasdaq/chart/1y-fixture", test_chart_1y_fixture);
  g_test_add_func("/nasdaq/chart/null-and-bad-points", test_chart_null_and_bad_points);
  g_test_add_func("/nasdaq/historical/fixture", test_historical_fixture);
  g_test_add_func("/nasdaq/historical/bad-rows", test_historical_bad_rows);
  g_test_add_func("/nasdaq/search/fixture", test_search_fixture);
  g_test_add_func("/nasdaq/search/filtering", test_search_filtering);
  g_test_add_func("/nasdaq/url/quote", test_urls_quote_and_encoding);
  g_test_add_func("/nasdaq/url/series", test_urls_series);
  g_test_add_func("/nasdaq/url/search", test_urls_search);
  g_test_add_func("/nasdaq/origin-policy", test_constructor_origin_policy);
  g_test_add_func("/nasdaq/net/quote", test_net_quote_success_and_request_shape);
  g_test_add_func("/nasdaq/net/series-search", test_net_series_and_search_paths);
  g_test_add_func("/nasdaq/net/invalid-args", test_net_invalid_arguments_make_no_request);
  g_test_add_func("/nasdaq/net/status-mapping", test_net_status_mapping);
  g_test_add_func("/nasdaq/net/retry-backoff", test_net_retry_then_success_with_backoff);
  g_test_add_func("/nasdaq/net/refused", test_net_connection_refused_is_network_error);
  g_test_add_func("/nasdaq/net/timeout", test_net_timeout);
  g_test_add_func("/nasdaq/net/body-cap", test_net_body_cap);
  g_test_add_func("/nasdaq/net/cancel-request", test_net_cancel_during_request);
  g_test_add_func("/nasdaq/net/cancel-backoff", test_net_cancel_during_backoff);
  g_test_add_func("/nasdaq/net/pre-cancelled", test_net_already_cancelled);
  g_test_add_func("/nasdaq/net/cancel-before-finish", test_net_cancel_before_finish);
  g_test_add_func("/nasdaq/net/provider-dropped", test_net_provider_dropped_while_in_flight);
  g_test_add_func("/nasdaq/net/1d-prev-close-all-states", test_net_1d_prev_close_all_market_states);
  g_test_add_func("/nasdaq/net/1d-prev-close-from-quote", test_net_1d_prev_close_from_quote);
  g_test_add_func("/nasdaq/net/1d-quote-failure", test_net_1d_quote_failure_keeps_series);
  g_test_add_func("/nasdaq/net/1d-chart-failure", test_net_1d_chart_failure_fails_series);
  g_test_add_func("/nasdaq/net/1d-cancel", test_net_1d_cancel);
  g_test_add_func("/nasdaq/net/inflight-cap", test_net_inflight_cap);
  g_test_add_func("/nasdaq/net/truncated-body", test_net_truncated_body);
  g_test_add_func("/nasdaq/net/gzip-body", test_net_gzip_body);
  g_test_add_func("/nasdaq/net/non-5xx-not-retried", test_net_non_5xx_not_retried);
  return g_test_run();
}
