/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * test-indices.c - offline tests of StkYahoo and StkRouter.
 *
 * Part 1: the pure URL builder and the parsers, on trimmed real captures
 *         (fixtures/) and on small hand-written bodies.
 * Part 2: the request layer (politeness, retries, deadlines, queue cap,
 *         cancellation) against a loopback fake server. Nothing here touches
 *         the Internet: every request goes to 127.0.0.1.
 * Part 3: the router with fake providers and with the real providers wired to
 *         two loopback fake servers.
 *
 * Usage: test-indices FIXTURE_DIR
 */
#include <glib.h>
#include <gio/gio.h>
#include <math.h>
#include <string.h>

#include "stk-nasdaq.h"
#include "stk-router.h"
#include "stk-yahoo.h"

#define MS_PER_DAY G_GINT64_CONSTANT(86400000)
#define BODY(s) (const guint8 *)(s), strlen(s)

static const char *fixture_dir;

static char *
load_fixture(const char *name, gsize *len)
{
  g_autofree char *path = g_build_filename(fixture_dir, name, NULL);
  g_autoptr(GError) error = NULL;
  char *contents = NULL;
  g_assert_true(g_file_get_contents(path, &contents, len, &error));
  g_assert_no_error(error);
  return contents;
}

/* Epoch seconds / milliseconds of a UTC wall-clock time. */
static gint64
utc_s(int y, int mo, int d, int h, int mi, int s)
{
  g_autoptr(GDateTime) dt = g_date_time_new_utc(y, mo, d, h, mi, s);
  g_assert_nonnull(dt);
  return g_date_time_to_unix(dt);
}

static gint64
utc_ms(int y, int mo, int d, int h, int mi)
{
  return utc_s(y, mo, d, h, mi, 0) * 1000;
}

/* ------------------------------------------------------------------------ */
/* Part 1: URL builder and parsers                                          */
/* ------------------------------------------------------------------------ */

static void
test_url_builder(void)
{
  g_autoptr(GError) error = NULL;
  static const struct {
    StkRange range;
    const char *query;
  } cases[] = {
      {STK_RANGE_1D, "range=1d&interval=5m"}, {STK_RANGE_1M, "range=1mo&interval=1d"},
      {STK_RANGE_3M, "range=3mo&interval=1d"}, {STK_RANGE_6M, "range=6mo&interval=1d"},
      {STK_RANGE_1Y, "range=1y&interval=1d"},
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autofree char *url = stk_yahoo_chart_url("https://query1.finance.yahoo.com", "^GSPC",
                                               STK_KIND_INDEX, cases[i].range, &error);
    g_assert_no_error(error);
    g_autofree char *expected = g_strdup_printf(
        "https://query1.finance.yahoo.com/v8/finance/chart/%%5EGSPC?%s", cases[i].query);
    g_assert_cmpstr(url, ==, expected);
  }
  /* Upper-cased and percent-encoded; nothing of the symbol can add path or query parts. */
  g_autofree char *url = stk_yahoo_chart_url("http://127.0.0.1:9", "eurusd=x", STK_KIND_INDEX,
                                             STK_RANGE_1M, &error);
  g_assert_cmpstr(url, ==, "http://127.0.0.1:9/v8/finance/chart/EURUSD%3DX?range=1mo&interval=1d");
  g_autofree char *dot = stk_yahoo_chart_url("http://127.0.0.1:9", "BRK.B", STK_KIND_INDEX,
                                             STK_RANGE_1D, &error);
  g_assert_cmpstr(dot, ==, "http://127.0.0.1:9/v8/finance/chart/BRK.B?range=1d&interval=5m");
  /* The quote URL: daily samples of the last five sessions. */
  g_autofree char *quote = stk_yahoo_quote_url("https://query1.finance.yahoo.com", "^gspc",
                                               STK_KIND_INDEX, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(quote, ==,
                  "https://query1.finance.yahoo.com/v8/finance/chart/%5EGSPC?range=5d&interval=1d");
}

static void
test_url_builder_rejections(void)
{
  static const StkKind bad_kinds[] = {STK_KIND_STOCK, STK_KIND_ETF, STK_KIND_FUND, (StkKind)42};
  for (guint i = 0; i < G_N_ELEMENTS(bad_kinds); i++) {
    g_autoptr(GError) error = NULL;
    g_assert_null(stk_yahoo_chart_url("http://127.0.0.1:9", "^GSPC", bad_kinds[i], STK_RANGE_1D,
                                      &error));
    g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
    g_clear_error(&error);
    g_assert_null(stk_yahoo_quote_url("http://127.0.0.1:9", "^GSPC", bad_kinds[i], &error));
    g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
  }
  static const char *const bad_symbols[] = {"", " ^GSPC", "^GS PC", "../x", "A/B", "^GSPC?x=1",
                                            "A%2FB", "^GSPC#", "^GS\xc3\xa4", "^0123456789ABCDEF",
                                            "A\nB", "A&B"};
  for (guint i = 0; i < G_N_ELEMENTS(bad_symbols); i++) {
    g_autoptr(GError) error = NULL;
    g_assert_null(stk_yahoo_chart_url("http://127.0.0.1:9", bad_symbols[i], STK_KIND_INDEX,
                                      STK_RANGE_1D, &error));
    g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
    g_clear_error(&error);
    g_assert_null(stk_yahoo_quote_url("http://127.0.0.1:9", bad_symbols[i], STK_KIND_INDEX, &error));
    g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
  }
  g_autoptr(GError) error = NULL;
  g_assert_null(stk_yahoo_chart_url("http://127.0.0.1:9", "^GSPC", STK_KIND_INDEX, (StkRange)17,
                                    &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
  g_clear_error(&error);
  g_assert_null(stk_yahoo_chart_url("http://127.0.0.1:9", NULL, STK_KIND_INDEX, STK_RANGE_1D,
                                    &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
  /* The error is optional. */
  g_assert_null(stk_yahoo_chart_url("http://127.0.0.1:9", "^GSPC", STK_KIND_STOCK, STK_RANGE_1D,
                                    NULL));
}

static void
test_series_1d_fixture(void)
{
  gsize len;
  g_autofree char *data = load_fixture("chart_1d_index.json", &len);
  g_autoptr(GError) error = NULL;
  g_autoptr(StkSeries) s = stk_yahoo_parse_series((const guint8 *)data, len, STK_RANGE_1D, &error);
  g_assert_no_error(error);
  g_assert_nonnull(s);
  g_assert_cmpint(s->range, ==, STK_RANGE_1D);
  g_assert_cmpuint(s->n_points, ==, 7);
  /* meta.previousClose (7765.36 here) is not trusted: the request layer fills prev_close. */
  g_assert_true(isnan(s->prev_close));
  /* True UTC instants, seconds * 1000, inside the regular session of 2026-10-09. */
  g_assert_cmpint(s->points[0].t_ms, ==, utc_ms(2026, 10, 9, 13, 30));
  g_assert_cmpint(s->points[6].t_ms, ==, G_GINT64_CONSTANT(1791554228000));
  g_assert_cmpfloat_with_epsilon(s->points[0].v, 7792.56005859375, 1e-9);
  g_assert_cmpfloat_with_epsilon(s->points[6].v, 7785.60986328125, 1e-9);
  for (gsize i = 1; i < s->n_points; i++)
    g_assert_cmpint(s->points[i].t_ms, >, s->points[i - 1].t_ms);
}

static void
test_series_daily_fixture(void)
{
  gsize len;
  g_autofree char *data = load_fixture("chart_1y_index.json", &len);
  g_autoptr(GError) error = NULL;
  g_autoptr(StkSeries) s = stk_yahoo_parse_series((const guint8 *)data, len, STK_RANGE_1Y, &error);
  g_assert_no_error(error);
  g_assert_nonnull(s);
  g_assert_cmpint(s->range, ==, STK_RANGE_1Y);
  g_assert_cmpuint(s->n_points, ==, 252);
  /* chartPreviousClose of a daily response is the close before the range: never used. */
  g_assert_true(isnan(s->prev_close));
  /* The first bar is 2025-10-09 09:30 EDT, the last 2026-10-09 09:30 EDT. */
  g_assert_cmpint(s->points[0].t_ms, ==, utc_ms(2025, 10, 9, 0, 0));
  g_assert_cmpint(s->points[s->n_points - 1].t_ms, ==, utc_ms(2026, 10, 9, 0, 0));
  g_assert_cmpfloat_with_epsilon(s->points[0].v, 6735.10986328125, 1e-9);
  g_assert_cmpfloat_with_epsilon(s->points[s->n_points - 1].v, 7786.60986328125, 1e-9);
  for (gsize i = 0; i < s->n_points; i++) {
    g_assert_cmpint(s->points[i].t_ms % MS_PER_DAY, ==, 0); /* 00:00 UTC */
    if (i > 0)
      g_assert_cmpint(s->points[i].t_ms, >, s->points[i - 1].t_ms); /* one per date */
  }
  /* The same body parsed for any daily range keeps the requested range label. */
  g_autoptr(StkSeries) m = stk_yahoo_parse_series((const guint8 *)data, len, STK_RANGE_1M, &error);
  g_assert_no_error(error);
  g_assert_cmpint(m->range, ==, STK_RANGE_1M);
  g_assert_true(isnan(m->prev_close));
}

/* Builds a chart body around @meta (a JSON object), @timestamps and @closes
 * (comma-separated JSON values). */
static char *
chart_json(const char *meta, const char *timestamps, const char *closes)
{
  return g_strdup_printf("{\"chart\":{\"result\":[{\"meta\":%s,\"timestamp\":[%s],"
                         "\"indicators\":{\"quote\":[{\"close\":[%s]}]}}],\"error\":null}}",
                         meta, timestamps, closes);
}

#define SESSION_START G_GINT64_CONSTANT(1791552600) /* 2026-10-09 09:30 EDT */
#define SESSION_END G_GINT64_CONSTANT(1791576000)   /* 2026-10-09 16:00 EDT */

static const char *const kMeta1D =
    "{\"previousClose\":100.5,\"chartPreviousClose\":90.25,"
    "\"currentTradingPeriod\":{\"regular\":{\"start\":1791552600,\"end\":1791576000}}}";

static StkSeries *
parse_series_ok(const char *body, StkRange range)
{
  g_autoptr(GError) error = NULL;
  StkSeries *s = stk_yahoo_parse_series(BODY(body), range, &error);
  g_assert_no_error(error);
  g_assert_nonnull(s);
  return s;
}

static void
test_series_1d_filters(void)
{
  const gint64 S = SESSION_START, E = SESSION_END;
  /* before, start (kept), +60, +120, null close, out of order, last second, end (dropped), after */
  g_autofree char *ts = g_strdup_printf("%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT
                                        ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT
                                        ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT,
                                        S - 1, S, S + 60, S + 120, S + 180, S + 30, E - 1, E, E + 60);
  g_autofree char *body = chart_json(kMeta1D, ts, "1,2,3,4,null,5,6,7,8");
  g_autoptr(StkSeries) s = parse_series_ok(body, STK_RANGE_1D);
  g_assert_cmpuint(s->n_points, ==, 4);
  g_assert_cmpint(s->points[0].t_ms, ==, S * 1000);
  g_assert_cmpfloat(s->points[0].v, ==, 2.0);
  g_assert_cmpint(s->points[1].t_ms, ==, (S + 60) * 1000);
  g_assert_cmpint(s->points[2].t_ms, ==, (S + 120) * 1000);
  g_assert_cmpfloat(s->points[2].v, ==, 4.0);
  g_assert_cmpint(s->points[3].t_ms, ==, (E - 1) * 1000);
  g_assert_cmpfloat(s->points[3].v, ==, 6.0);
  /* The meta's previousClose and chartPreviousClose are ignored. */
  g_assert_true(isnan(s->prev_close));
}

static void
test_series_ignores_meta_prev_close(void)
{
  /* Neither meta.previousClose (Yahoo has served it stale) nor chartPreviousClose is ever a
   * series' prev_close, whatever the range. */
  static const char *const periods =
      "\"currentTradingPeriod\":{\"regular\":{\"start\":1791552600,\"end\":1791576000}}";
  static const char *const fields[] = {
      "\"previousClose\":100.5,\"chartPreviousClose\":90.25",
      "\"chartPreviousClose\":90.25",
      "\"x\":1",
  };
  for (guint i = 0; i < G_N_ELEMENTS(fields); i++) {
    g_autofree char *meta = g_strdup_printf("{%s,%s}", fields[i], periods);
    g_autofree char *body = chart_json(meta, "1791552600", "5");
    g_autoptr(StkSeries) s = parse_series_ok(body, STK_RANGE_1D);
    g_assert_cmpuint(s->n_points, ==, 1);
    g_assert_true(isnan(s->prev_close));
    g_autoptr(StkSeries) d = parse_series_ok(body, STK_RANGE_1M);
    g_assert_true(isnan(d->prev_close));
  }
}

static void
test_series_1d_requires_trading_period(void)
{
  static const char *const metas[] = {
      "{}",
      "{\"currentTradingPeriod\":null}",
      "{\"currentTradingPeriod\":{}}",
      "{\"currentTradingPeriod\":{\"regular\":{\"start\":1791552600}}}",
      "{\"currentTradingPeriod\":{\"regular\":{\"start\":1791576000,\"end\":1791552600}}}",
      "{\"currentTradingPeriod\":{\"regular\":{\"start\":1791552600,\"end\":1791552600}}}",
      "{\"currentTradingPeriod\":{\"regular\":{\"start\":\"1791552600\",\"end\":1791576000}}}",
      "{\"currentTradingPeriod\":{\"regular\":{\"start\":1,\"end\":1791576000}}}",
      "{\"currentTradingPeriod\":{\"regular\":{\"start\":1791552600,\"end\":9999999999999}}}",
  };
  for (guint i = 0; i < G_N_ELEMENTS(metas); i++) {
    g_autofree char *body = chart_json(metas[i], "1791552600", "5");
    g_autoptr(GError) error = NULL;
    g_assert_null(stk_yahoo_parse_series(BODY(body), STK_RANGE_1D, &error));
    g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
    /* The trading period is only needed for 1D. */
    g_autoptr(StkSeries) d = parse_series_ok(body, STK_RANGE_1M);
    g_assert_cmpuint(d->n_points, ==, 1);
  }
}

static void
test_series_daily_dates(void)
{
  const gint64 a = utc_s(2026, 10, 8, 13, 30, 0);  /* 10-08 09:30 EDT */
  const gint64 b = utc_s(2026, 10, 9, 2, 0, 0);    /* 10-08 22:00 EDT: UTC date is the 9th */
  const gint64 c = utc_s(2026, 10, 9, 13, 30, 0);  /* 10-09 */
  const gint64 d = utc_s(2026, 10, 9, 15, 0, 0);   /* 10-09, null close */
  const gint64 e = utc_s(2026, 10, 10, 4, 30, 0);  /* 10-10 00:30 EDT */
  const gint64 f = utc_s(2026, 10, 13, 13, 30, 0); /* 10-13 */
  const gint64 g = utc_s(2026, 10, 12, 13, 30, 0); /* earlier than f: out of order */
  g_autofree char *ts =
      g_strdup_printf("%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT
                      ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT
                      ",%" G_GINT64_FORMAT,
                      a, b, c, d, e, f, g);
  g_autofree char *body = chart_json("{}", ts, "10,11,20,null,30,40,50");
  g_autoptr(StkSeries) s = parse_series_ok(body, STK_RANGE_3M);
  g_assert_cmpint(s->range, ==, STK_RANGE_3M);
  g_assert_cmpuint(s->n_points, ==, 4);
  /* Latest sample of a New York date wins (b replaces a); a null never does. */
  g_assert_cmpint(s->points[0].t_ms, ==, utc_ms(2026, 10, 8, 0, 0));
  g_assert_cmpfloat(s->points[0].v, ==, 11.0);
  g_assert_cmpint(s->points[1].t_ms, ==, utc_ms(2026, 10, 9, 0, 0));
  g_assert_cmpfloat(s->points[1].v, ==, 20.0);
  g_assert_cmpint(s->points[2].t_ms, ==, utc_ms(2026, 10, 10, 0, 0));
  g_assert_cmpfloat(s->points[2].v, ==, 30.0);
  g_assert_cmpint(s->points[3].t_ms, ==, utc_ms(2026, 10, 13, 0, 0));
  g_assert_cmpfloat(s->points[3].v, ==, 40.0);
}

static void
test_series_daily_dst_boundary(void)
{
  /* DST ended 2026-11-01 06:00Z: afterwards New York is UTC-5. */
  const gint64 before = utc_s(2026, 11, 2, 4, 59, 59); /* 11-01 23:59:59 EST */
  const gint64 after = utc_s(2026, 11, 2, 5, 0, 0);    /* 11-02 00:00:00 EST */
  g_autofree char *ts = g_strdup_printf("%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT, before, after);
  g_autofree char *body = chart_json("{}", ts, "1,2");
  g_autoptr(StkSeries) s = parse_series_ok(body, STK_RANGE_1M);
  g_assert_cmpuint(s->n_points, ==, 2);
  g_assert_cmpint(s->points[0].t_ms, ==, utc_ms(2026, 11, 1, 0, 0));
  g_assert_cmpint(s->points[1].t_ms, ==, utc_ms(2026, 11, 2, 0, 0));
}

/* At most one point per date: the sample with the latest raw timestamp wins;
 * a sample whose timestamp goes backwards is skipped, as on the 1D path. */
static void
test_series_daily_same_date_latest_raw_sample_wins(void)
{
  /* Same date, the earlier sample comes later in the array. */
  g_autofree char *ts1 = g_strdup_printf("%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT,
                                         utc_s(2026, 10, 8, 19, 0, 0), utc_s(2026, 10, 8, 14, 0, 0));
  g_autofree char *body1 = chart_json("{}", ts1, "111,222");
  g_autoptr(StkSeries) s1 = parse_series_ok(body1, STK_RANGE_1M);
  g_assert_cmpuint(s1->n_points, ==, 1);
  g_assert_cmpint(s1->points[0].t_ms, ==, utc_ms(2026, 10, 8, 0, 0));
  g_assert_cmpfloat(s1->points[0].v, ==, 111.0);

  /* Ascending samples of one date: the latest wins; an equal timestamp too. */
  g_autofree char *ts2 = g_strdup_printf(
      "%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT,
      utc_s(2026, 10, 8, 14, 0, 0), utc_s(2026, 10, 8, 19, 0, 0), utc_s(2026, 10, 8, 19, 0, 0),
      utc_s(2026, 10, 9, 14, 0, 0));
  g_autofree char *body2 = chart_json("{}", ts2, "1,2,3,4");
  g_autoptr(StkSeries) s2 = parse_series_ok(body2, STK_RANGE_6M);
  g_assert_cmpuint(s2->n_points, ==, 2);
  g_assert_cmpfloat(s2->points[0].v, ==, 3.0);
  g_assert_cmpfloat(s2->points[1].v, ==, 4.0);

  /* A backwards sample of an earlier date is skipped; the next date's later
   * sample still replaces its point, and an earlier one does not. */
  g_autofree char *ts3 = g_strdup_printf(
      "%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT,
      utc_s(2026, 10, 9, 15, 0, 0), utc_s(2026, 10, 8, 15, 0, 0), utc_s(2026, 10, 9, 18, 0, 0),
      utc_s(2026, 10, 9, 16, 0, 0));
  g_autofree char *body3 = chart_json("{}", ts3, "30,20,40,35");
  g_autoptr(StkSeries) s3 = parse_series_ok(body3, STK_RANGE_1Y);
  g_assert_cmpuint(s3->n_points, ==, 1);
  g_assert_cmpint(s3->points[0].t_ms, ==, utc_ms(2026, 10, 9, 0, 0));
  g_assert_cmpfloat(s3->points[0].v, ==, 40.0);
}

static void
test_series_skips_unusable_samples(void)
{
  /* timestamps: string, null, float second, below 2022, from 2101, huge, then
   * four good ones; closes: ..., string, bool, number, object. */
  g_autofree char *ts = g_strdup_printf(
      "\"x\",null,%" G_GINT64_FORMAT ".5,1000000000,4133980800,99999999999999999999,"
      "%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT,
      utc_s(2026, 10, 6, 13, 30, 0), utc_s(2026, 10, 6, 13, 30, 0), utc_s(2026, 10, 7, 13, 30, 0),
      utc_s(2026, 10, 8, 13, 30, 0), utc_s(2026, 10, 9, 13, 30, 0));
  g_autofree char *body = chart_json("{}", ts, "1,2,3,4,5,6,\"7\",true,8.5,{}");
  g_autoptr(StkSeries) s = parse_series_ok(body, STK_RANGE_1M);
  g_assert_cmpuint(s->n_points, ==, 2);
  /* The fractional second is cut: 2026-10-06 with close 3. */
  g_assert_cmpint(s->points[0].t_ms, ==, utc_ms(2026, 10, 6, 0, 0));
  g_assert_cmpfloat(s->points[0].v, ==, 3.0);
  g_assert_cmpint(s->points[1].t_ms, ==, utc_ms(2026, 10, 8, 0, 0));
  g_assert_cmpfloat(s->points[1].v, ==, 8.5);
}

static void
test_series_array_length_mismatch(void)
{
  g_autofree char *ts = g_strdup_printf("%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT,
                                        utc_s(2026, 10, 7, 13, 30, 0), utc_s(2026, 10, 8, 13, 30, 0),
                                        utc_s(2026, 10, 9, 13, 30, 0));
  g_autofree char *shorter = chart_json("{}", ts, "1,2");
  g_autoptr(StkSeries) a = parse_series_ok(shorter, STK_RANGE_1M);
  g_assert_cmpuint(a->n_points, ==, 2);
  g_autofree char *longer = chart_json("{}", ts, "1,2,3,4,5");
  g_autoptr(StkSeries) b = parse_series_ok(longer, STK_RANGE_1M);
  g_assert_cmpuint(b->n_points, ==, 3);
}

static void
test_series_empty_and_shapes(void)
{
  /* No timestamps (key absent or null): a valid empty series. */
  static const char *const empties[] = {
      "{\"chart\":{\"result\":[{\"meta\":{},\"indicators\":{\"quote\":[{}]}}],\"error\":null}}",
      "{\"chart\":{\"result\":[{\"meta\":{},\"timestamp\":null,\"indicators\":{\"quote\":[{}]}}]}}",
      "{\"chart\":{\"result\":[{\"meta\":{},\"timestamp\":[]}],\"error\":null}}",
  };
  for (guint i = 0; i < G_N_ELEMENTS(empties); i++) {
    g_autoptr(StkSeries) s = parse_series_ok(empties[i], STK_RANGE_1M);
    g_assert_cmpuint(s->n_points, ==, 0);
  }
  g_autofree char *empty_1d = g_strdup_printf(
      "{\"chart\":{\"result\":[{\"meta\":%s,\"indicators\":{\"quote\":[{}]}}],\"error\":null}}",
      kMeta1D);
  g_autoptr(StkSeries) s1d = parse_series_ok(empty_1d, STK_RANGE_1D);
  g_assert_cmpuint(s1d->n_points, ==, 0);
  g_assert_cmpint(s1d->range, ==, STK_RANGE_1D);
  g_assert_true(isnan(s1d->prev_close));

  /* Timestamps without a close array are malformed. */
  static const char *const broken[] = {
      "{\"chart\":{\"result\":[{\"meta\":{},\"timestamp\":[1791552600]}]}}",
      "{\"chart\":{\"result\":[{\"meta\":{},\"timestamp\":[1791552600],\"indicators\":null}]}}",
      "{\"chart\":{\"result\":[{\"meta\":{},\"timestamp\":[1791552600],\"indicators\":{\"quote\":[]}}]}}",
      "{\"chart\":{\"result\":[{\"meta\":{},\"timestamp\":[1791552600],\"indicators\":{\"quote\":[5]}}]}}",
      "{\"chart\":{\"result\":[{\"meta\":{},\"timestamp\":[1791552600],\"indicators\":{\"quote\":[{\"close\":3}]}}]}}",
      "{\"chart\":{\"result\":[{\"meta\":{},\"timestamp\":\"x\"}]}}",
  };
  for (guint i = 0; i < G_N_ELEMENTS(broken); i++) {
    g_autoptr(GError) error = NULL;
    g_assert_null(stk_yahoo_parse_series(BODY(broken[i]), STK_RANGE_1M, &error));
    g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  }
}

/* Bodies that must map to STK_ERROR_PARSE for both parsers. */
static const char *const kParseErrorBodies[] = {
    "",
    "   ",
    "not json",
    "[]",
    "null",
    "42",
    "{}",
    "{\"chart\":null}",
    "{\"chart\":[]}",
    "{\"chart\":{}}",
    "{\"chart\":{\"result\":null,\"error\":null}}",
    "{\"chart\":{\"result\":[],\"error\":null}}",
    "{\"chart\":{\"result\":{},\"error\":null}}",
    "{\"chart\":{\"result\":[null],\"error\":null}}",
    "{\"chart\":{\"result\":[{}],\"error\":null}}",
    "{\"chart\":{\"result\":[{\"meta\":[]}],\"error\":null}}",
    "{\"chart\":{\"result\":[{\"meta\":{}",
};

static void
test_parse_errors(void)
{
  for (guint i = 0; i < G_N_ELEMENTS(kParseErrorBodies); i++) {
    g_autoptr(GError) error = NULL;
    g_assert_null(stk_yahoo_parse_series(BODY(kParseErrorBodies[i]), STK_RANGE_1M, &error));
    g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
    g_clear_error(&error);
    g_assert_null(stk_yahoo_parse_quote(BODY(kParseErrorBodies[i]), "^GSPC", STK_KIND_INDEX,
                                        &error));
    g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
    g_clear_error(&error);
    g_assert_true(isnan(stk_yahoo_parse_prev_close(BODY(kParseErrorBodies[i]),
                                                   utc_ms(2026, 10, 13, 13, 30), &error)));
    g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  }
  g_autoptr(GError) error = NULL;
  /* NULL data with length 0, invalid UTF-8, a NUL byte in the middle. */
  g_assert_null(stk_yahoo_parse_series(NULL, 0, STK_RANGE_1M, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&error);
  static const guint8 bad_utf8[] = "{\"chart\":{\"result\":null,\"x\":\"\xff\"}}";
  g_assert_null(stk_yahoo_parse_series(bad_utf8, sizeof bad_utf8 - 1, STK_RANGE_1M, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&error);
  /* Nesting far beyond any sane response must be a parse error, not a crash. */
  GString *deep = g_string_new(NULL);
  for (guint i = 0; i < 20000; i++)
    g_string_append_c(deep, '[');
  g_assert_null(stk_yahoo_parse_series((const guint8 *)deep->str, deep->len, STK_RANGE_1M, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&error);
  g_string_free(deep, TRUE);
  /* Above the body cap. */
  gsize big = (gsize)STK_YAHOO_MAX_BODY_BYTES + 1;
  guint8 *blob = g_malloc(big);
  memset(blob, ' ', big);
  g_assert_null(stk_yahoo_parse_series(blob, big, STK_RANGE_1M, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&error);
  g_assert_null(stk_yahoo_parse_quote(blob, big, "^GSPC", STK_KIND_INDEX, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&error);
  g_assert_true(isnan(stk_yahoo_parse_prev_close(blob, big, utc_ms(2026, 10, 13, 13, 30), &error)));
  g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  g_free(blob);
}

static void
test_chart_error_is_not_found(void)
{
  gsize len;
  g_autofree char *data = load_fixture("chart_error.json", &len);
  g_autoptr(GError) error = NULL;
  g_assert_null(stk_yahoo_parse_series((const guint8 *)data, len, STK_RANGE_1D, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_NOT_FOUND);
  g_assert_nonnull(strstr(error->message, "No data found"));
  g_clear_error(&error);
  g_assert_null(stk_yahoo_parse_quote((const guint8 *)data, len, "^ZZZZ", STK_KIND_INDEX, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_NOT_FOUND);
  g_clear_error(&error);
  g_assert_true(isnan(stk_yahoo_parse_prev_close((const guint8 *)data, len,
                                                 utc_ms(2026, 10, 13, 13, 30), &error)));
  g_assert_error(error, STK_ERROR, STK_ERROR_NOT_FOUND);
  g_clear_error(&error);

  /* Any non-null chart.error is NOT_FOUND, even beside a result and with odd shapes. */
  static const char *const variants[] = {
      "{\"chart\":{\"result\":null,\"error\":{\"code\":\"Bad Request\"}}}",
      "{\"chart\":{\"result\":null,\"error\":\"gone\"}}",
      "{\"chart\":{\"result\":[{\"meta\":{}}],\"error\":{}}}",
      "{\"chart\":{\"error\":{\"code\":\"Not Found\",\"description\":null}}}",
  };
  for (guint i = 0; i < G_N_ELEMENTS(variants); i++) {
    g_assert_null(stk_yahoo_parse_series(BODY(variants[i]), STK_RANGE_1M, &error));
    g_assert_error(error, STK_ERROR, STK_ERROR_NOT_FOUND);
    g_clear_error(&error);
  }
  /* A long server message is cut (on a character boundary), not echoed in full. */
  g_autofree char *wordy = g_strdup_printf(
      "{\"chart\":{\"result\":null,\"error\":{\"description\":\"%s\"}}}",
      "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4"
      "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4"
      "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4"
      "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4"
      "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4"
      "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4"
      "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4"
      "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4"
      "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4"
      "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4");
  g_assert_null(stk_yahoo_parse_series(BODY(wordy), STK_RANGE_1M, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_NOT_FOUND);
  g_assert_true(g_utf8_validate(error->message, -1, NULL));
  g_assert_cmpuint(g_utf8_strlen(error->message, -1), <, 120);
  g_clear_error(&error);
}

static StkQuote *
parse_quote_ok(const char *body, const char *symbol)
{
  g_autoptr(GError) error = NULL;
  StkQuote *q = stk_yahoo_parse_quote(BODY(body), symbol, STK_KIND_INDEX, &error);
  g_assert_no_error(error);
  g_assert_nonnull(q);
  return q;
}

static void
test_quote_fixture(void)
{
  gsize len;
  g_autofree char *data = load_fixture("chart_5d_index.json", &len);
  g_autoptr(GError) error = NULL;
  g_autoptr(StkQuote) q =
      stk_yahoo_parse_quote((const guint8 *)data, len, "^gspc", STK_KIND_INDEX, &error);
  g_assert_no_error(error);
  g_assert_nonnull(q);
  /* Symbol and kind are the requested ones, upper-cased. */
  g_assert_cmpstr(q->symbol, ==, "^GSPC");
  g_assert_cmpint(q->kind, ==, STK_KIND_INDEX);
  g_assert_cmpstr(q->name, ==, "S&P 500");
  g_assert_cmpstr(q->currency, ==, "USD");
  g_assert_cmpfloat_with_epsilon(q->price, 7805.45, 1e-9);
  /* Captured 2026-10-13 after the close. The change is against the close of 2026-10-12, the
   * session before regularMarketTime's date (-1.79), not against the 7811.54 of 2026-10-09 that
   * meta.previousClose of the range=1d response gave that evening (-6.09). */
  const double prev = 7807.240234375;
  g_assert_cmpfloat_with_epsilon(q->change, 7805.45 - prev, 1e-9);
  g_assert_cmpfloat_with_epsilon(q->change_pct, (7805.45 - prev) / prev * 100.0, 1e-9);
  g_assert_cmpint(q->as_of_ms, ==, G_GINT64_CONSTANT(1791923582000));
  g_assert_false(q->is_realtime);
  g_assert_false(q->market_open);

  /* The range=1d chart has no daily sample before its session, and its meta.previousClose
   * (7765.36) is not used: the change is unknown. */
  g_autofree char *intraday = load_fixture("chart_1d_index.json", &len);
  g_autoptr(StkQuote) i =
      stk_yahoo_parse_quote((const guint8 *)intraday, len, "^GSPC", STK_KIND_INDEX, &error);
  g_assert_no_error(error);
  g_assert_cmpfloat_with_epsilon(i->price, 7785.61, 1e-9);
  g_assert_true(isnan(i->change));
  g_assert_true(isnan(i->change_pct));
}

/* 09:30 EDT of 2026-10-09, -12, -13 and -14, and 16:33:02 EDT of 2026-10-13 (epoch seconds, as
 * JSON text). */
#define DAY_1009 "1791552600"
#define DAY_1012 "1791811800"
#define DAY_1013 "1791898200"
#define DAY_1014 "1791984600"
#define AFTER_CLOSE_1013 "1791923582"

/* Parses a quote of @price at @time (JSON text) with the daily samples @timestamps / @closes. */
static StkQuote *
parse_daily_quote(double price, const char *time, const char *timestamps, const char *closes)
{
  g_autofree char *meta =
      g_strdup_printf("{\"regularMarketPrice\":%g,\"regularMarketTime\":%s}", price, time);
  g_autofree char *body = chart_json(meta, timestamps, closes);
  return parse_quote_ok(body, "^RUT");
}

static void
test_prev_close(void)
{
  gsize len;
  g_autofree char *data = load_fixture("chart_5d_index.json", &len);
  /* Daily closes of the fixture (2026): 10-07 7801.77, 10-08 7765.36, 10-09 7811.54,
   * 10-12 7807.24, 10-13 7805.45. */
  static const struct {
    int day, hour, minute; /* October 2026, UTC */
    double expected;       /* NAN: none */
  } cases[] = {
      {13, 13, 30, 7807.240234375},  /* the session of 10-13: Monday's close */
      {12, 13, 30, 7811.5400390625}, /* Monday 10-12: Friday's */
      {14, 13, 30, 7805.4501953125}, /* the day after the last sample */
      {13, 3, 0, 7811.5400390625},   /* 10-13 03:00 UTC is 10-12 23:00 in New York */
      {10, 16, 0, 7811.5400390625},  /* Saturday: Friday's */
      {8, 13, 30, 7801.77001953125},
      {7, 13, 30, NAN}, /* nothing before the first sample */
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autoptr(GError) error = NULL;
    double p = stk_yahoo_parse_prev_close(
        (const guint8 *)data, len, utc_ms(2026, 10, cases[i].day, cases[i].hour, cases[i].minute),
        &error);
    g_assert_no_error(error);
    if (isnan(cases[i].expected))
      g_assert_true(isnan(p));
    else
      g_assert_cmpfloat(p, ==, cases[i].expected);
  }
  /* No usable previous close is not an error: an instant outside the supported years, a chart
   * without samples. */
  g_autoptr(GError) error = NULL;
  g_assert_true(isnan(stk_yahoo_parse_prev_close((const guint8 *)data, len, 0, &error)));
  g_assert_no_error(error);
  g_assert_true(isnan(stk_yahoo_parse_prev_close(BODY("{\"chart\":{\"result\":[{\"meta\":{}}]}}"),
                                                 utc_ms(2026, 10, 13, 13, 30), &error)));
  g_assert_no_error(error);
  /* The error is optional. */
  g_assert_true(isnan(stk_yahoo_parse_prev_close(BODY("{}"), utc_ms(2026, 10, 13, 13, 30), NULL)));
}

static void
test_quote_fields(void)
{
  /* The requested symbol/kind win over the response's meta.symbol. The meta's previous-close
   * fields are ignored: the change is against the daily close before regularMarketTime's date. */
  g_autoptr(StkQuote) q = parse_quote_ok(
      "{\"chart\":{\"result\":[{\"meta\":{\"symbol\":\"^DJI\",\"currency\":\"EUR\","
      "\"regularMarketPrice\":200,\"previousClose\":1,\"chartPreviousClose\":1,"
      "\"regularMarketTime\":" AFTER_CLOSE_1013 ",\"longName\":\"Long\",\"shortName\":\"Short\"},"
      "\"timestamp\":[" DAY_1009 "," DAY_1012 "," DAY_1013 "],"
      "\"indicators\":{\"quote\":[{\"close\":[300,250,200]}]}}]}}",
      "^IXIC");
  g_assert_cmpstr(q->symbol, ==, "^IXIC");
  g_assert_cmpstr(q->name, ==, "Long");
  g_assert_cmpstr(q->currency, ==, "EUR");
  g_assert_cmpfloat(q->price, ==, 200.0);
  g_assert_cmpfloat(q->change, ==, -50.0);
  g_assert_cmpfloat_with_epsilon(q->change_pct, -20.0, 1e-9);
  g_assert_cmpint(q->as_of_ms, ==, G_GINT64_CONSTANT(1791923582000));

  /* Pre-market the next day the price is still 10-13's close, so the change stays against 10-12,
   * although a (still empty) sample of 10-14 exists. */
  g_autoptr(StkQuote) pre =
      parse_daily_quote(110, AFTER_CLOSE_1013, DAY_1012 "," DAY_1013 "," DAY_1014, "100,110,null");
  g_assert_cmpfloat(pre->change, ==, 10.0);
  g_assert_cmpfloat_with_epsilon(pre->change_pct, 10.0, 1e-9);

  /* Of several samples of the previous date the latest wins; one whose timestamp goes back does
   * not. 1791820800 is 10-12 12:00 EDT, 1791835200 10-12 16:00 EDT. */
  g_autoptr(StkQuote) late = parse_daily_quote(
      110, AFTER_CLOSE_1013, DAY_1012 ",1791835200,1791820800," DAY_1013, "100,104,999,110");
  g_assert_cmpfloat(late->change, ==, 6.0);

  /* timestamp[] and close[] pair up to the shorter length. */
  g_autoptr(StkQuote) shorter =
      parse_daily_quote(110, AFTER_CLOSE_1013, DAY_1012 "," DAY_1013, "100");
  g_assert_cmpfloat(shorter->change, ==, 10.0);

  /* No usable previous close: the price stays, change and change_pct are unknown. The previous
   * date without a close never falls back to an older session; nothing before the session; no
   * samples at all; no regularMarketTime, or one outside the supported years. */
  static const struct {
    const char *time, *timestamps, *closes;
  } unknown[] = {
      {AFTER_CLOSE_1013, DAY_1009 "," DAY_1012 "," DAY_1013, "90,null,110"},
      {AFTER_CLOSE_1013, DAY_1009 "," DAY_1012 "," DAY_1013, "90,0,110"},
      {AFTER_CLOSE_1013, DAY_1013, "110"},
      {AFTER_CLOSE_1013, "", ""},
      {"null", DAY_1012 "," DAY_1013, "100,110"},
      {"1", DAY_1012 "," DAY_1013, "100,110"},
  };
  for (guint i = 0; i < G_N_ELEMENTS(unknown); i++) {
    g_autoptr(StkQuote) n =
        parse_daily_quote(110, unknown[i].time, unknown[i].timestamps, unknown[i].closes);
    g_assert_cmpfloat(n->price, ==, 110.0);
    g_assert_true(isnan(n->change));
    g_assert_true(isnan(n->change_pct));
  }
  g_autoptr(StkQuote) c = parse_quote_ok(
      "{\"chart\":{\"result\":[{\"meta\":{\"regularMarketPrice\":110,\"previousClose\":100,"
      "\"chartPreviousClose\":100}}]}}",
      "^RUT");
  g_assert_true(isnan(c->change));
  g_assert_cmpstr(c->name, ==, "");
  g_assert_cmpstr(c->currency, ==, "USD");
  g_assert_cmpint(c->as_of_ms, ==, 0);

  /* A previous close so small that the percentage overflows: unknown, never infinite. */
  g_autoptr(StkQuote) t = parse_daily_quote(110, AFTER_CLOSE_1013, DAY_1012 "," DAY_1013, "1e-320,110");
  g_assert_cmpfloat(t->price, ==, 110.0);
  g_assert_true(isnan(t->change));
  g_assert_true(isnan(t->change_pct));
}

static void
test_quote_sanitizes_text_and_time(void)
{
  /* Name: a control character or a long name is not used; the next candidate is. */
  g_autoptr(StkQuote) a = parse_quote_ok(
      "{\"chart\":{\"result\":[{\"meta\":{\"regularMarketPrice\":1,\"longName\":\"a\\nb\","
      "\"shortName\":\"Fine\",\"currency\":\"usd\",\"regularMarketTime\":1}}]}}",
      "^GSPC");
  g_assert_cmpstr(a->name, ==, "Fine");
  g_assert_cmpstr(a->currency, ==, "USD"); /* lower case: default */
  g_assert_cmpint(a->as_of_ms, ==, 0);     /* before 2022: unknown */
  g_autofree char *long_name = g_strnfill(257, 'n');
  g_autofree char *body = g_strdup_printf(
      "{\"chart\":{\"result\":[{\"meta\":{\"regularMarketPrice\":1,\"longName\":\"%s\","
      "\"shortName\":\"\\u007f\",\"currency\":\"EURO\"}}]}}",
      long_name);
  g_autoptr(StkQuote) b = parse_quote_ok(body, "^GSPC");
  g_assert_cmpstr(b->name, ==, "");
  g_assert_cmpstr(b->currency, ==, "USD"); /* not three letters */
  g_autofree char *max_name = g_strnfill(256, 'n');
  g_autofree char *body2 = g_strdup_printf(
      "{\"chart\":{\"result\":[{\"meta\":{\"regularMarketPrice\":1,\"longName\":\"%s\"}}]}}",
      max_name);
  g_autoptr(StkQuote) c = parse_quote_ok(body2, "^GSPC");
  g_assert_cmpstr(c->name, ==, max_name);
  /* UTF-8 names pass. */
  g_autoptr(StkQuote) d = parse_quote_ok(
      "{\"chart\":{\"result\":[{\"meta\":{\"regularMarketPrice\":1,\"longName\":\"Nikkei \\u65e5\"}}]}}",
      "^N225");
  g_assert_cmpstr(d->name, ==, "Nikkei \xe6\x97\xa5");
}

static void
test_quote_errors(void)
{
  static const char *const no_price[] = {
      "{\"chart\":{\"result\":[{\"meta\":{}}]}}",
      "{\"chart\":{\"result\":[{\"meta\":{\"regularMarketPrice\":null}}]}}",
      "{\"chart\":{\"result\":[{\"meta\":{\"regularMarketPrice\":\"7785\"}}]}}",
      "{\"chart\":{\"result\":[{\"meta\":{\"regularMarketPrice\":0}}]}}",
      "{\"chart\":{\"result\":[{\"meta\":{\"regularMarketPrice\":-5}}]}}",
      "{\"chart\":{\"result\":[{\"meta\":{\"regularMarketPrice\":true}}]}}",
      "{\"chart\":{\"result\":[{\"meta\":{\"regularMarketPrice\":1e999}}]}}",
  };
  for (guint i = 0; i < G_N_ELEMENTS(no_price); i++) {
    g_autoptr(GError) error = NULL;
    g_assert_null(stk_yahoo_parse_quote(BODY(no_price[i]), "^GSPC", STK_KIND_INDEX, &error));
    g_assert_error(error, STK_ERROR, STK_ERROR_PARSE);
  }
  /* Arguments are checked before the body. */
  g_autoptr(GError) error = NULL;
  g_assert_null(stk_yahoo_parse_quote(BODY("{}"), "^GSPC", STK_KIND_STOCK, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
  g_clear_error(&error);
  g_assert_null(stk_yahoo_parse_quote(BODY("{}"), "a/b", STK_KIND_INDEX, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
  g_clear_error(&error);
  g_assert_null(stk_yahoo_parse_series(BODY("{}"), (StkRange)9, &error));
  g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
}

static void
test_constants_and_origin_policy(void)
{
  /* Politeness limits are part of the contract. */
  g_assert_cmpuint(STK_YAHOO_MIN_GAP_MS, >=, 3000);
  g_assert_cmpuint(STK_YAHOO_TIMEOUT_MS, ==, 10000);
  g_assert_cmpuint(STK_YAHOO_MAX_RETRIES, ==, 2);
  g_assert_cmpuint(STK_YAHOO_MAX_BODY_BYTES, ==, 2 * 1024 * 1024);
  g_assert_cmpstr(STK_YAHOO_BASE_URL, ==, "https://query1.finance.yahoo.com");

  static const char *const accepted[] = {
      "https://query1.finance.yahoo.com", "https://query1.finance.yahoo.com/",
      "https://example.org:8443",         "http://127.0.0.1:8080",
      "http://localhost:80/",             "http://[::1]:9",
  };
  for (guint i = 0; i < G_N_ELEMENTS(accepted); i++) {
    g_autoptr(GError) error = NULL;
    StkYahoo *y = stk_yahoo_new_custom(accepted[i], 1000, 0, &error);
    g_assert_no_error(error);
    g_assert_nonnull(y);
    g_assert_true(STK_IS_PROVIDER(y));
    g_object_unref(y);
  }
  static const char *const rejected[] = {
      "",
      "http://example.org",
      "http://127.0.0.1.example.org",
      "ftp://127.0.0.1",
      "file:///tmp",
      "https://",
      "https://user:pw@query1.finance.yahoo.com",
      "https://query1.finance.yahoo.com/v8",
      "https://query1.finance.yahoo.com?x=1",
      "https://query1.finance.yahoo.com#f",
      "not a url",
      "//query1.finance.yahoo.com",
  };
  for (guint i = 0; i < G_N_ELEMENTS(rejected); i++) {
    g_autoptr(GError) error = NULL;
    g_assert_null(stk_yahoo_new_custom(rejected[i], 1000, 0, &error));
    g_assert_error(error, STK_ERROR, STK_ERROR_INVALID_ARG);
  }
  g_assert_null(stk_yahoo_new_custom("http://example.org", 1000, 0, NULL)); /* error optional */
  StkYahoo *def = stk_yahoo_new();
  g_assert_true(STK_IS_PROVIDER(def));
  g_object_unref(def);
}

/* ------------------------------------------------------------------------ */
/* Part 2: request layer against a loopback fake server                     */
/* ------------------------------------------------------------------------ */

typedef enum {
  STEP_RESPOND,      /* status + body (after delay_ms), then close */
  STEP_HANG,         /* read the request, never answer */
  STEP_CLOSE,        /* close without answering */
  STEP_BIG_STREAMED, /* 200, no Content-Length, 3 MiB body */
  STEP_BIG_DECLARED, /* 200 with Content-Length above the cap */
  STEP_RAW,          /* body is the complete raw response, sent verbatim, then close */
  STEP_GZIP          /* 200, body gzip-compressed with a matching Content-Length */
} StepKind;

typedef struct {
  StepKind kind;
  guint status;
  const char *body; /* NUL-terminated */
  guint delay_ms;   /* STEP_RESPOND only */
} Step;

typedef struct Fake Fake;

typedef struct {
  Fake *fake;
  GSocketConnection *connection;
  GString *request;
  guint8 buffer[2048];
  gsize big_remaining; /* STEP_BIG_STREAMED */
  gboolean counted;    /* counted in fake->active */
  guint timer;         /* delayed response source, 0 if none */
  GBytes *pending;     /* the delayed response */
} Conn;

struct Fake {
  GSocketService *service;
  guint port;
  const Step *steps; /* request n is answered by steps[min(n, n_steps - 1)] */
  guint n_steps;
  guint requests;
  guint active;     /* requests received whose connection is not finished yet */
  guint max_active;
  GPtrArray *paths;   /* request target of every request (char *) */
  GArray *arrivals;   /* g_get_monotonic_time() of every request (gint64) */
  GPtrArray *held;    /* Conn of hanging or delayed requests */
  char *last_request; /* complete head of the last request */
};

static void conn_read(Conn *conn);

static void
conn_free(Conn *conn)
{
  if (conn->timer != 0)
    g_source_remove(conn->timer);
  g_clear_pointer(&conn->pending, g_bytes_unref);
  if (conn->counted)
    conn->fake->active--;
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
respond_bytes(Conn *conn, GBytes *bytes)
{
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn->connection));
  gsize len;
  const void *data = g_bytes_get_data(bytes, &len);
  /* Keep the bytes alive until the write completes by attaching them to the stream. */
  g_object_set_data_full(G_OBJECT(out), "stk-test-bytes", g_bytes_ref(bytes),
                         (GDestroyNotify)g_bytes_unref);
  g_output_stream_write_all_async(out, data, len, G_PRIORITY_DEFAULT, NULL, write_done, conn);
}

static gboolean
delayed_respond(gpointer user_data)
{
  Conn *conn = user_data;
  conn->timer = 0;
  g_ptr_array_remove(conn->fake->held, conn);
  GBytes *bytes = g_steal_pointer(&conn->pending);
  respond_bytes(conn, bytes);
  g_bytes_unref(bytes);
  return G_SOURCE_REMOVE;
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

static void
handle_request(Conn *conn)
{
  Fake *fake = conn->fake;
  g_free(fake->last_request);
  fake->last_request = g_strdup(conn->request->str);
  const char *target = conn->request->str + 4; /* after "GET " */
  const char *end = strchr(target, ' ');
  g_ptr_array_add(fake->paths, end != NULL ? g_strndup(target, (gsize)(end - target))
                                           : g_strdup(target));
  gint64 now = g_get_monotonic_time();
  g_array_append_val(fake->arrivals, now);
  const Step *step = &fake->steps[MIN(fake->requests, fake->n_steps - 1)];
  fake->requests++;
  conn->counted = TRUE;
  fake->active++;
  fake->max_active = MAX(fake->max_active, fake->active);

  switch (step->kind) {
  case STEP_RESPOND: {
    g_autofree char *head = g_strdup_printf(
        "HTTP/1.1 %u X\r\nContent-Type: application/json\r\nContent-Length: %" G_GSIZE_FORMAT
        "\r\nConnection: close\r\n\r\n",
        step->status, strlen(step->body));
    GString *all = g_string_new(head);
    g_string_append(all, step->body);
    GBytes *bytes = g_string_free_to_bytes(all);
    if (step->delay_ms == 0) {
      respond_bytes(conn, bytes);
      g_bytes_unref(bytes);
    } else {
      conn->pending = bytes;
      g_ptr_array_add(fake->held, conn);
      conn->timer = g_timeout_add(step->delay_ms, delayed_respond, conn);
    }
    break;
  }
  case STEP_RAW: {
    GBytes *bytes = g_bytes_new_static(step->body, strlen(step->body));
    respond_bytes(conn, bytes);
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
    g_autoptr(GBytes) packed = g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(mem));
    g_autofree char *head = g_strdup_printf(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\n"
        "Content-Length: %" G_GSIZE_FORMAT "\r\nConnection: close\r\n\r\n",
        g_bytes_get_size(packed));
    GByteArray *all = g_byte_array_new();
    g_byte_array_append(all, (const guint8 *)head, (guint)strlen(head));
    g_byte_array_append(all, g_bytes_get_data(packed, NULL), (guint)g_bytes_get_size(packed));
    GBytes *bytes = g_byte_array_free_to_bytes(all);
    respond_bytes(conn, bytes);
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
    conn->big_remaining = (gsize)3 * 1024 * 1024;
    g_autoptr(GError) error = NULL;
    (void)g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn->connection)),
                                    head_text, strlen(head_text), NULL, NULL, &error);
    big_next_chunk(conn);
    break;
  }
  case STEP_BIG_DECLARED: {
    static const char text[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                               "Content-Length: 3000000\r\nConnection: close\r\n\r\n{}";
    GBytes *bytes = g_bytes_new_static(text, strlen(text));
    respond_bytes(conn, bytes);
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

/* A fake server answering request n with steps[min(n, n_steps - 1)]. */
static Fake *
fake_new(const Step *steps, guint n_steps)
{
  g_assert_cmpuint(n_steps, >, 0);
  Fake *fake = g_new0(Fake, 1);
  fake->steps = steps;
  fake->n_steps = n_steps;
  fake->paths = g_ptr_array_new_with_free_func(g_free);
  fake->arrivals = g_array_new(FALSE, FALSE, sizeof(gint64));
  fake->held = g_ptr_array_new();
  fake->service = g_socket_service_new();
  g_autoptr(GError) error = NULL;
  guint16 port =
      g_socket_listener_add_any_inet_port(G_SOCKET_LISTENER(fake->service), NULL, &error);
  g_assert_no_error(error);
  fake->port = port;
  g_signal_connect(fake->service, "incoming", G_CALLBACK(on_incoming), fake);
  g_socket_service_start(fake->service);
  return fake;
}

static void
fake_free(Fake *fake)
{
  g_socket_service_stop(fake->service);
  g_socket_listener_close(G_SOCKET_LISTENER(fake->service));
  g_object_unref(fake->service);
  while (fake->held->len > 0) {
    Conn *conn = g_ptr_array_index(fake->held, fake->held->len - 1);
    g_ptr_array_set_size(fake->held, fake->held->len - 1);
    conn_free(conn);
  }
  g_ptr_array_unref(fake->held);
  g_ptr_array_unref(fake->paths);
  g_array_unref(fake->arrivals);
  g_free(fake->last_request);
  g_free(fake);
}

static gint64
arrival_us(const Fake *fake, guint i)
{
  return g_array_index(fake->arrivals, gint64, i);
}

static StkYahoo *
provider_for(const Fake *fake, guint timeout_ms, guint gap_ms)
{
  g_autofree char *base = g_strdup_printf("http://127.0.0.1:%u", fake->port);
  g_autoptr(GError) error = NULL;
  StkYahoo *yahoo = stk_yahoo_new_custom(base, timeout_ms, gap_ms, &error);
  g_assert_no_error(error);
  g_assert_nonnull(yahoo);
  return yahoo;
}

typedef struct {
  guint calls;
  GObject *source;
  StkQuote *quote;
  StkSeries *series;
  GPtrArray *hits;
  GError *error;
  gint64 started_us;
  gint64 finished_us;
  GCancellable *cancel_before_finish; /* on_quote_done() cancels it first, if set */
  GCancellable *cancel_after_finish;  /* on_quote_done() cancels it last, if set */
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
  if (o->cancel_before_finish != NULL)
    g_cancellable_cancel(o->cancel_before_finish);
  o->source = source;
  o->quote = stk_provider_fetch_quote_finish(STK_PROVIDER(source), result, &o->error);
  o->finished_us = g_get_monotonic_time();
  o->calls++;
  g_assert_cmpuint(o->calls, ==, 1); /* every operation completes exactly once */
  if (o->cancel_after_finish != NULL)
    g_cancellable_cancel(o->cancel_after_finish);
}

static void
on_series_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  Outcome *o = user_data;
  o->source = source;
  o->series = stk_provider_fetch_series_finish(STK_PROVIDER(source), result, &o->error);
  o->finished_us = g_get_monotonic_time();
  o->calls++;
  g_assert_cmpuint(o->calls, ==, 1);
}

static void
on_search_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  Outcome *o = user_data;
  o->source = source;
  o->hits = stk_provider_search_finish(STK_PROVIDER(source), result, &o->error);
  o->finished_us = g_get_monotonic_time();
  o->calls++;
  g_assert_cmpuint(o->calls, ==, 1);
}

static void
fetch_quote(StkProvider *p, Outcome *o, const char *symbol, StkKind kind, GCancellable *cancellable)
{
  o->started_us = g_get_monotonic_time();
  stk_provider_fetch_quote_async(p, symbol, kind, cancellable, on_quote_done, o);
  g_assert_cmpuint(o->calls, ==, 0); /* never completes synchronously */
}

static void
fetch_series(StkProvider *p, Outcome *o, const char *symbol, StkKind kind, StkRange range,
             GCancellable *cancellable)
{
  o->started_us = g_get_monotonic_time();
  stk_provider_fetch_series_async(p, symbol, kind, range, cancellable, on_series_done, o);
  g_assert_cmpuint(o->calls, ==, 0);
}

/* Runs the default main context until @o completes; returns the time from its
 * start to its completion in milliseconds. */
static gint64
wait_for(Outcome *o)
{
  while (o->calls == 0)
    g_main_context_iteration(NULL, TRUE);
  return (o->finished_us - o->started_us) / 1000;
}

static gboolean
set_flag(gpointer user_data)
{
  *(gboolean *)user_data = TRUE;
  return G_SOURCE_REMOVE;
}

/* Runs the main context for @ms (late or duplicate callbacks would show up). */
static void
spin_ms(guint ms)
{
  gboolean flag = FALSE;
  g_timeout_add(ms, set_flag, &flag);
  while (!flag)
    g_main_context_iteration(NULL, TRUE);
}

static gboolean
cancel_later(gpointer user_data)
{
  g_cancellable_cancel(G_CANCELLABLE(user_data));
  return G_SOURCE_REMOVE;
}

static void
on_weak_notify(gpointer data, GObject *where_the_object_was)
{
  (void)where_the_object_was;
  *(gboolean *)data = TRUE;
}

/* Drops the caller's reference to @obj and reports whether it was finalized
 * within @ms (every operation holds a reference to its provider). */
static gboolean
released_within(gpointer obj, guint ms)
{
  gboolean finalized = FALSE;
  g_object_weak_ref(G_OBJECT(obj), on_weak_notify, &finalized);
  g_object_unref(obj);
  gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;
  while (!finalized && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  return finalized;
}

static char *kQuoteBody;   /* the quote URL fixture, loaded by main() */
static char *kChartBody1d; /* the 1D chart fixture */
static char *kSeriesBody1y;

static void
test_net_request_shape_and_results(void)
{
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody, 0}, {STEP_RESPOND, 200, kChartBody1d, 0},
                   {STEP_RESPOND, 200, kQuoteBody, 0}, {STEP_RESPOND, 200, kSeriesBody1y, 0}};
  Fake *fake = fake_new(script, G_N_ELEMENTS(script));
  StkYahoo *y = provider_for(fake, 2000, 20);

  /* Quote: served from the quote URL. Symbol and kind are the requested ones. */
  Outcome o = {0};
  fetch_quote(STK_PROVIDER(y), &o, "^gspc", STK_KIND_INDEX, NULL);
  wait_for(&o);
  g_assert_no_error(o.error);
  g_assert_nonnull(o.quote);
  g_assert_true(o.source == G_OBJECT(y));
  g_assert_cmpstr(o.quote->symbol, ==, "^GSPC");
  g_assert_cmpint(o.quote->kind, ==, STK_KIND_INDEX);
  g_assert_cmpfloat_with_epsilon(o.quote->price, 7805.45, 1e-9);
  g_assert_cmpfloat_with_epsilon(o.quote->change, 7805.45 - 7807.240234375, 1e-9);
  g_assert_false(o.quote->is_realtime);
  g_assert_cmpuint(fake->requests, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(fake->paths, 0), ==,
                  "/v8/finance/chart/%5EGSPC?range=5d&interval=1d");
  /* Read-only, identified, JSON, no credentials, cookies or browser identity. */
  g_assert_true(g_str_has_prefix(fake->last_request, "GET /v8/finance/chart/%5EGSPC?"));
  g_assert_nonnull(g_strstr_len(fake->last_request, -1, "User-Agent: stocks-app/"));
  g_assert_nonnull(g_strstr_len(fake->last_request, -1, "Accept: application/json"));
  g_assert_null(g_strstr_len(fake->last_request, -1, "Cookie:"));
  g_assert_null(g_strstr_len(fake->last_request, -1, "Authorization:"));
  g_assert_null(g_strstr_len(fake->last_request, -1, "Mozilla"));
  g_assert_null(g_strstr_len(fake->last_request, -1, "Referer:"));
  outcome_clear(&o);
  spin_ms(30);
  g_assert_cmpuint(o.calls, ==, 1);

  /* 1D series: the chart, then the quote URL for the previous close of the chart's trading day
   * (2026-10-09: the close of 2026-10-08 in the quote fixture). */
  Outcome s = {0};
  fetch_series(STK_PROVIDER(y), &s, "^DJI", STK_KIND_INDEX, STK_RANGE_1D, NULL);
  wait_for(&s);
  g_assert_no_error(s.error);
  g_assert_cmpuint(s.series->n_points, ==, 7);
  g_assert_cmpint(s.series->range, ==, STK_RANGE_1D);
  g_assert_cmpfloat(s.series->prev_close, ==, 7765.35986328125);
  g_assert_cmpuint(fake->requests, ==, 3);
  g_assert_cmpstr(g_ptr_array_index(fake->paths, 1), ==,
                  "/v8/finance/chart/%5EDJI?range=1d&interval=5m");
  g_assert_cmpstr(g_ptr_array_index(fake->paths, 2), ==,
                  "/v8/finance/chart/%5EDJI?range=5d&interval=1d");
  outcome_clear(&s);

  /* 1Y series. */
  Outcome d = {0};
  fetch_series(STK_PROVIDER(y), &d, "^IXIC", STK_KIND_INDEX, STK_RANGE_1Y, NULL);
  wait_for(&d);
  g_assert_no_error(d.error);
  g_assert_cmpuint(d.series->n_points, ==, 252);
  g_assert_cmpint(d.series->range, ==, STK_RANGE_1Y);
  g_assert_true(isnan(d.series->prev_close));
  g_assert_cmpuint(fake->requests, ==, 4); /* one request for every other range */
  g_assert_cmpstr(g_ptr_array_index(fake->paths, 3), ==,
                  "/v8/finance/chart/%5EIXIC?range=1y&interval=1d");
  outcome_clear(&d);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_all_range_paths(void)
{
  Step script[] = {{STEP_RESPOND, 200, kSeriesBody1y, 0}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 2000, 5);
  static const struct {
    StkRange range;
    const char *path;
  } cases[] = {
      {STK_RANGE_1M, "/v8/finance/chart/%5ERUT?range=1mo&interval=1d"},
      {STK_RANGE_3M, "/v8/finance/chart/%5ERUT?range=3mo&interval=1d"},
      {STK_RANGE_6M, "/v8/finance/chart/%5ERUT?range=6mo&interval=1d"},
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    Outcome o = {0};
    fetch_series(STK_PROVIDER(y), &o, "^RUT", STK_KIND_INDEX, cases[i].range, NULL);
    wait_for(&o);
    g_assert_no_error(o.error);
    g_assert_cmpint(o.series->range, ==, cases[i].range);
    g_assert_cmpstr(g_ptr_array_index(fake->paths, i), ==, cases[i].path);
    outcome_clear(&o);
  }
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_base_url_trailing_slash(void)
{
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody, 0}};
  Fake *fake = fake_new(script, 1);
  g_autofree char *base = g_strdup_printf("http://localhost:%u/", fake->port);
  g_autoptr(GError) error = NULL;
  StkYahoo *y = stk_yahoo_new_custom(base, 2000, 0, &error);
  g_assert_no_error(error);
  Outcome o = {0};
  fetch_quote(STK_PROVIDER(y), &o, "^GSPC", STK_KIND_INDEX, NULL);
  wait_for(&o);
  g_assert_no_error(o.error);
  g_assert_cmpstr(g_ptr_array_index(fake->paths, 0), ==,
                  "/v8/finance/chart/%5EGSPC?range=5d&interval=1d");
  outcome_clear(&o);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_invalid_args(void)
{
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody, 0}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 2000, 5);
  static const StkKind kinds[] = {STK_KIND_STOCK, STK_KIND_ETF, STK_KIND_FUND, (StkKind)42};
  for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) {
    Outcome q = {0}, s = {0};
    fetch_quote(STK_PROVIDER(y), &q, "^GSPC", kinds[i], NULL);
    wait_for(&q);
    g_assert_null(q.quote);
    g_assert_error(q.error, STK_ERROR, STK_ERROR_INVALID_ARG);
    fetch_series(STK_PROVIDER(y), &s, "^GSPC", kinds[i], STK_RANGE_1M, NULL);
    wait_for(&s);
    g_assert_null(s.series);
    g_assert_error(s.error, STK_ERROR, STK_ERROR_INVALID_ARG);
    outcome_clear(&q);
    outcome_clear(&s);
  }
  static const char *const symbols[] = {"", "^G SPC", "../x", "^0123456789ABCDEF", "^GS%50C"};
  for (guint i = 0; i < G_N_ELEMENTS(symbols); i++) {
    Outcome q = {0};
    fetch_quote(STK_PROVIDER(y), &q, symbols[i], STK_KIND_INDEX, NULL);
    wait_for(&q);
    g_assert_error(q.error, STK_ERROR, STK_ERROR_INVALID_ARG);
    outcome_clear(&q);
  }
  Outcome r = {0};
  fetch_series(STK_PROVIDER(y), &r, "^GSPC", STK_KIND_INDEX, (StkRange)9, NULL);
  wait_for(&r);
  g_assert_error(r.error, STK_ERROR, STK_ERROR_INVALID_ARG);
  outcome_clear(&r);
  spin_ms(50);
  g_assert_cmpuint(fake->requests, ==, 0);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_search_is_empty_and_offline(void)
{
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody, 0}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 2000, 5);
  static const char *const texts[] = {"s&p", "", "\xff\xfe", "very long text very long text very long text "
                                      "very long text very long text very long text very long text"};
  for (guint i = 0; i < G_N_ELEMENTS(texts); i++) {
    Outcome o = {0};
    o.started_us = g_get_monotonic_time();
    stk_provider_search_async(STK_PROVIDER(y), texts[i], NULL, on_search_done, &o);
    g_assert_cmpuint(o.calls, ==, 0); /* not synchronous */
    wait_for(&o);
    g_assert_no_error(o.error);
    g_assert_nonnull(o.hits);
    g_assert_cmpuint(o.hits->len, ==, 0);
    g_assert_true(o.source == G_OBJECT(y));
    outcome_clear(&o);
  }
  /* A cancelled search is cancelled, like any operation. */
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  Outcome c = {0};
  stk_provider_search_async(STK_PROVIDER(y), "x", cancellable, on_search_done, &c);
  g_cancellable_cancel(cancellable);
  wait_for(&c);
  g_assert_error(c.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  outcome_clear(&c);
  spin_ms(50);
  g_assert_cmpuint(fake->requests, ==, 0);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

/* One request answered by @step; checks the error and the number of requests
 * (1 + retries for retryable failures). */
static void
run_failure_case(Step step, GQuark domain, int code, guint expected_requests)
{
  Fake *fake = fake_new(&step, 1);
  StkYahoo *y = provider_for(fake, 2000, 10);
  Outcome o = {0};
  fetch_quote(STK_PROVIDER(y), &o, "^GSPC", STK_KIND_INDEX, NULL);
  wait_for(&o);
  g_assert_null(o.quote);
  g_assert_error(o.error, domain, code);
  g_assert_cmpuint(fake->requests, ==, expected_requests);
  outcome_clear(&o);
  spin_ms(60);
  g_assert_cmpuint(fake->requests, ==, expected_requests);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_status_mapping(void)
{
  static const char *const error_body =
      "{\"chart\":{\"result\":null,\"error\":{\"code\":\"Not Found\",\"description\":\"gone\"}}}";
  /* 404 and chart.error: unknown symbol. Never retried. */
  run_failure_case((Step){STEP_RESPOND, 404, error_body, 0}, STK_ERROR, STK_ERROR_NOT_FOUND, 1);
  run_failure_case((Step){STEP_RESPOND, 404, "", 0}, STK_ERROR, STK_ERROR_NOT_FOUND, 1);
  run_failure_case((Step){STEP_RESPOND, 200, error_body, 0}, STK_ERROR, STK_ERROR_NOT_FOUND, 1);
  /* 429: reported, never retried. */
  run_failure_case((Step){STEP_RESPOND, 429, "Too Many Requests", 0}, STK_ERROR,
                   STK_ERROR_RATE_LIMITED, 1);
  /* Other 4xx: final HTTP errors (400 is not "not found" here). */
  static const guint client_errors[] = {400, 401, 403, 408, 410, 418, 451};
  for (guint i = 0; i < G_N_ELEMENTS(client_errors); i++)
    run_failure_case((Step){STEP_RESPOND, client_errors[i], "{}", 0}, STK_ERROR, STK_ERROR_HTTP, 1);
  /* Redirects are not followed and not retried. */
  run_failure_case((Step){STEP_RAW, 0,
                          "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:9/x\r\n"
                          "Content-Length: 0\r\nConnection: close\r\n\r\n",
                          0},
                   STK_ERROR, STK_ERROR_HTTP, 1);
  /* Bad bodies with status 200 are final PARSE errors. */
  run_failure_case((Step){STEP_RESPOND, 200, "<html>Too Many Requests</html>", 0}, STK_ERROR,
                   STK_ERROR_PARSE, 1);
  run_failure_case((Step){STEP_RESPOND, 200, "", 0}, STK_ERROR, STK_ERROR_PARSE, 1);
  run_failure_case((Step){STEP_RESPOND, 200, "{\"chart\":{\"result\":[{\"meta\":{}}]}}", 0},
                   STK_ERROR, STK_ERROR_PARSE, 1);
}

static void
test_net_retry_rules(void)
{
  /* NETWORK, TIMEOUT-like and 5xx: 1 + 2 retries, then the last error. */
  run_failure_case((Step){STEP_RESPOND, 500, "{}", 0}, STK_ERROR, STK_ERROR_HTTP,
                   1 + STK_YAHOO_MAX_RETRIES);
  run_failure_case((Step){STEP_RESPOND, 503, "", 0}, STK_ERROR, STK_ERROR_HTTP,
                   1 + STK_YAHOO_MAX_RETRIES);
  run_failure_case((Step){STEP_RESPOND, 599, "", 0}, STK_ERROR, STK_ERROR_HTTP,
                   1 + STK_YAHOO_MAX_RETRIES);
  run_failure_case((Step){STEP_CLOSE, 0, NULL, 0}, STK_ERROR, STK_ERROR_NETWORK,
                   1 + STK_YAHOO_MAX_RETRIES);
  /* A body shorter than its Content-Length is a dropped connection (retryable). */
  run_failure_case((Step){STEP_RAW, 0,
                          "HTTP/1.1 200 OK\r\nContent-Length: 536\r\nConnection: close\r\n\r\n{}", 0},
                   STK_ERROR, STK_ERROR_NETWORK, 1 + STK_YAHOO_MAX_RETRIES);
  /* Oversized bodies are PARSE and final. */
  run_failure_case((Step){STEP_BIG_DECLARED, 0, NULL, 0}, STK_ERROR, STK_ERROR_PARSE, 1);
  run_failure_case((Step){STEP_BIG_STREAMED, 0, NULL, 0}, STK_ERROR, STK_ERROR_PARSE, 1);
}

static void
test_net_refused_connection(void)
{
  /* Port of a closed listener: connection refused is a NETWORK error, retried. */
  Step script[] = {{STEP_CLOSE, 0, NULL, 0}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 2000, 5);
  g_socket_service_stop(fake->service);
  g_socket_listener_close(G_SOCKET_LISTENER(fake->service));
  Outcome o = {0};
  fetch_quote(STK_PROVIDER(y), &o, "^GSPC", STK_KIND_INDEX, NULL);
  wait_for(&o);
  g_assert_error(o.error, STK_ERROR, STK_ERROR_NETWORK);
  g_assert_cmpuint(fake->requests, ==, 0);
  outcome_clear(&o);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_retry_then_success(void)
{
  Step script[] = {{STEP_RESPOND, 503, "", 0}, {STEP_CLOSE, 0, NULL, 0},
                   {STEP_RESPOND, 200, kQuoteBody, 0}};
  Fake *fake = fake_new(script, G_N_ELEMENTS(script));
  StkYahoo *y = provider_for(fake, 2000, 10);
  Outcome o = {0};
  fetch_quote(STK_PROVIDER(y), &o, "^GSPC", STK_KIND_INDEX, NULL);
  wait_for(&o);
  g_assert_no_error(o.error);
  g_assert_nonnull(o.quote);
  g_assert_cmpuint(fake->requests, ==, 3);
  outcome_clear(&o);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_429_after_5xx_is_final(void)
{
  Step script[] = {{STEP_RESPOND, 502, "", 0}, {STEP_RESPOND, 429, "", 0},
                   {STEP_RESPOND, 200, kQuoteBody, 0}};
  Fake *fake = fake_new(script, G_N_ELEMENTS(script));
  StkYahoo *y = provider_for(fake, 2000, 10);
  Outcome o = {0};
  fetch_quote(STK_PROVIDER(y), &o, "^GSPC", STK_KIND_INDEX, NULL);
  wait_for(&o);
  g_assert_error(o.error, STK_ERROR, STK_ERROR_RATE_LIMITED);
  g_assert_cmpuint(fake->requests, ==, 2);
  outcome_clear(&o);
  spin_ms(60);
  g_assert_cmpuint(fake->requests, ==, 2);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_gzip_body(void)
{
  /* Content-Length counts the compressed bytes: it must not be compared with
   * the decoded length. */
  Step step = {STEP_GZIP, 0, NULL, 0};
  step.body = kQuoteBody;
  Fake *fake = fake_new(&step, 1);
  StkYahoo *y = provider_for(fake, 2000, 5);
  Outcome o = {0};
  fetch_quote(STK_PROVIDER(y), &o, "^GSPC", STK_KIND_INDEX, NULL);
  wait_for(&o);
  g_assert_no_error(o.error);
  g_assert_cmpfloat_with_epsilon(o.quote->price, 7805.45, 1e-9);
  g_assert_cmpuint(fake->requests, ==, 1);
  outcome_clear(&o);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_timeout_per_attempt(void)
{
  Step script[] = {{STEP_HANG, 0, NULL, 0}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 150, 30);
  Outcome o = {0};
  fetch_quote(STK_PROVIDER(y), &o, "^GSPC", STK_KIND_INDEX, NULL);
  gint64 elapsed_ms = wait_for(&o);
  g_assert_error(o.error, STK_ERROR, STK_ERROR_TIMEOUT);
  g_assert_cmpuint(fake->requests, ==, 1 + STK_YAHOO_MAX_RETRIES);
  g_assert_cmpint(elapsed_ms, >=, 3 * 140); /* each attempt has its own deadline */
  g_assert_cmpint(elapsed_ms, <, 5000);
  outcome_clear(&o);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

/* The request starts of one fake must be at least @gap_ms apart (a margin
 * covers connection set-up jitter between two arrivals). */
static void
assert_spaced(const Fake *fake, guint first, guint last, guint gap_ms, guint margin_ms)
{
  for (guint i = first + 1; i <= last; i++)
    g_assert_cmpint((arrival_us(fake, i) - arrival_us(fake, i - 1)) / 1000, >=,
                    (gint64)gap_ms - (gint64)margin_ms);
}

static void
test_net_pacing_between_requests(void)
{
  /* Four operations started at once: one request at a time, spaced by the gap,
   * in submission order. */
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody, 0}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 2000, 300);
  static const char *const symbols[] = {"^GSPC", "^DJI", "^IXIC", "^RUT"};
  Outcome o[4] = {{0}};
  gint64 start = g_get_monotonic_time();
  for (guint i = 0; i < 4; i++)
    fetch_quote(STK_PROVIDER(y), &o[i], symbols[i], STK_KIND_INDEX, NULL);
  for (guint i = 0; i < 4; i++) {
    wait_for(&o[i]);
    g_assert_no_error(o[i].error);
    g_assert_cmpstr(o[i].quote->symbol, ==, symbols[i]);
  }
  g_assert_cmpuint(fake->requests, ==, 4);
  g_assert_cmpint((g_get_monotonic_time() - start) / 1000, >=, 3 * 300 - 60);
  assert_spaced(fake, 0, 3, 300, 100);
  for (guint i = 0; i < 4; i++) {
    g_autofree char *expected = g_strdup_printf("/v8/finance/chart/%%5E%s?range=5d&interval=1d",
                                                symbols[i] + 1);
    g_assert_cmpstr(g_ptr_array_index(fake->paths, i), ==, expected);
    outcome_clear(&o[i]);
  }
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_one_request_on_the_wire(void)
{
  /* Responses take 300 ms, the gap is only 40 ms: the next request must wait
   * for the previous attempt to END, so the server never sees two at once. */
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody, 300}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 2000, 40);
  Outcome o[3] = {{0}};
  for (guint i = 0; i < 3; i++)
    fetch_quote(STK_PROVIDER(y), &o[i], "^GSPC", STK_KIND_INDEX, NULL);
  for (guint i = 0; i < 3; i++) {
    wait_for(&o[i]);
    g_assert_no_error(o[i].error);
    outcome_clear(&o[i]);
  }
  g_assert_cmpuint(fake->requests, ==, 3);
  g_assert_cmpuint(fake->max_active, ==, 1);
  assert_spaced(fake, 0, 2, 300, 30);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_retries_are_paced(void)
{
  /* The spacing also applies to retries; a retry goes before the operations
   * queued behind it. */
  Step script[] = {{STEP_RESPOND, 503, "", 0}, {STEP_RESPOND, 503, "", 0},
                   {STEP_RESPOND, 200, kQuoteBody, 0}};
  Fake *fake = fake_new(script, G_N_ELEMENTS(script));
  StkYahoo *y = provider_for(fake, 2000, 250);
  Outcome a = {0}, b = {0};
  fetch_quote(STK_PROVIDER(y), &a, "^GSPC", STK_KIND_INDEX, NULL);
  fetch_quote(STK_PROVIDER(y), &b, "^DJI", STK_KIND_INDEX, NULL);
  wait_for(&a);
  wait_for(&b);
  /* A: 503, 503, 200 (third attempt); B: nothing left in the script, repeats the 200. */
  g_assert_no_error(a.error);
  g_assert_no_error(b.error);
  g_assert_cmpuint(fake->requests, ==, 4);
  g_assert_cmpstr(g_ptr_array_index(fake->paths, 0), ==, g_ptr_array_index(fake->paths, 1));
  g_assert_cmpstr(g_ptr_array_index(fake->paths, 1), ==, g_ptr_array_index(fake->paths, 2));
  g_assert_cmpstr(g_ptr_array_index(fake->paths, 3), ==,
                  "/v8/finance/chart/%5EDJI?range=5d&interval=1d");
  assert_spaced(fake, 0, 3, 250, 100);
  g_assert_cmpstr(a.quote->symbol, ==, "^GSPC");
  g_assert_cmpstr(b.quote->symbol, ==, "^DJI");
  outcome_clear(&a);
  outcome_clear(&b);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_1d_prev_close_request(void)
{
  const char *const error_body =
      "{\"chart\":{\"result\":null,\"error\":{\"code\":\"Not Found\",\"description\":\"gone\"}}}";
  /* The previous close is optional: a final failure of the second request (404, 429, a chart
   * error, a body without daily samples, 5xx after its own retries) still delivers the chart,
   * with prev_close NAN. */
  const struct {
    Step second;
    guint requests;
  } optional[] = {
      {{STEP_RESPOND, 404, "", 0}, 2},
      {{STEP_RESPOND, 429, "", 0}, 2},
      {{STEP_RESPOND, 200, error_body, 0}, 2},
      {{STEP_RESPOND, 200, "{\"chart\":{\"result\":[{\"meta\":{}}]}}", 0}, 2},
      {{STEP_RESPOND, 503, "", 0}, 2 + STK_YAHOO_MAX_RETRIES},
  };
  for (guint i = 0; i < G_N_ELEMENTS(optional); i++) {
    Step script[] = {{STEP_RESPOND, 200, kChartBody1d, 0}, optional[i].second};
    Fake *fake = fake_new(script, G_N_ELEMENTS(script));
    StkYahoo *y = provider_for(fake, 2000, 10);
    Outcome o = {0};
    fetch_series(STK_PROVIDER(y), &o, "^GSPC", STK_KIND_INDEX, STK_RANGE_1D, NULL);
    wait_for(&o);
    g_assert_no_error(o.error);
    g_assert_cmpuint(o.series->n_points, ==, 7);
    g_assert_true(isnan(o.series->prev_close));
    spin_ms(60);
    g_assert_cmpuint(o.calls, ==, 1);
    g_assert_cmpuint(fake->requests, ==, optional[i].requests);
    g_assert_cmpstr(g_ptr_array_index(fake->paths, 1), ==,
                    "/v8/finance/chart/%5EGSPC?range=5d&interval=1d");
    outcome_clear(&o);
    g_assert_true(released_within(y, 2000));
    fake_free(fake);
  }

  /* A failing chart fails the series; the second request is never made. */
  Step failing[] = {{STEP_RESPOND, 404, error_body, 0}, {STEP_RESPOND, 200, kQuoteBody, 0}};
  Fake *fake = fake_new(failing, G_N_ELEMENTS(failing));
  StkYahoo *y = provider_for(fake, 2000, 10);
  Outcome f = {0};
  fetch_series(STK_PROVIDER(y), &f, "^GSPC", STK_KIND_INDEX, STK_RANGE_1D, NULL);
  wait_for(&f);
  g_assert_error(f.error, STK_ERROR, STK_ERROR_NOT_FOUND);
  g_assert_null(f.series);
  spin_ms(60);
  g_assert_cmpuint(fake->requests, ==, 1);
  outcome_clear(&f);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);

  /* Each request has retries of its own, and the second one is paced like any other. */
  Step retried[] = {{STEP_RESPOND, 503, "", 0},          {STEP_CLOSE, 0, NULL, 0},
                    {STEP_RESPOND, 200, kChartBody1d, 0}, {STEP_RESPOND, 502, "", 0},
                    {STEP_RESPOND, 503, "", 0},           {STEP_RESPOND, 200, kQuoteBody, 0}};
  fake = fake_new(retried, G_N_ELEMENTS(retried));
  y = provider_for(fake, 2000, 150);
  Outcome r = {0};
  fetch_series(STK_PROVIDER(y), &r, "^GSPC", STK_KIND_INDEX, STK_RANGE_1D, NULL);
  wait_for(&r);
  g_assert_no_error(r.error);
  g_assert_cmpuint(r.series->n_points, ==, 7);
  g_assert_cmpfloat(r.series->prev_close, ==, 7765.35986328125);
  g_assert_cmpuint(fake->requests, ==, 6);
  for (guint i = 0; i < 6; i++)
    g_assert_cmpstr(g_ptr_array_index(fake->paths, i), ==,
                    i < 3 ? "/v8/finance/chart/%5EGSPC?range=1d&interval=5m"
                          : "/v8/finance/chart/%5EGSPC?range=5d&interval=1d");
  assert_spaced(fake, 0, 5, 150, 60);
  outcome_clear(&r);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_1d_cancel_second_request(void)
{
  /* Cancelled while the second request waits for its slot (a 30 s gap): the series completes at
   * once with G_IO_ERROR_CANCELLED and the second request is never sent. */
  Step waiting[] = {{STEP_RESPOND, 200, kChartBody1d, 0}, {STEP_RESPOND, 200, kQuoteBody, 0}};
  Fake *fake = fake_new(waiting, G_N_ELEMENTS(waiting));
  StkYahoo *y = provider_for(fake, 2000, 30000);
  g_autoptr(GCancellable) c1 = g_cancellable_new();
  Outcome a = {0};
  fetch_series(STK_PROVIDER(y), &a, "^GSPC", STK_KIND_INDEX, STK_RANGE_1D, c1);
  spin_ms(200);
  g_assert_cmpuint(fake->requests, ==, 1);
  g_assert_cmpuint(a.calls, ==, 0);
  g_cancellable_cancel(c1);
  gint64 start = g_get_monotonic_time();
  while (a.calls == 0)
    g_main_context_iteration(NULL, TRUE);
  g_assert_cmpint((g_get_monotonic_time() - start) / 1000, <, 1500);
  g_assert_error(a.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null(a.series);
  spin_ms(60);
  g_assert_cmpuint(fake->requests, ==, 1);
  outcome_clear(&a);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);

  /* Cancelled while the second request is on the wire: cancelled, not retried, and the chart is
   * not delivered on its own. */
  Step hanging[] = {{STEP_RESPOND, 200, kChartBody1d, 0}, {STEP_HANG, 0, NULL, 0}};
  fake = fake_new(hanging, G_N_ELEMENTS(hanging));
  y = provider_for(fake, 20000, 10);
  g_autoptr(GCancellable) c2 = g_cancellable_new();
  Outcome b = {0};
  fetch_series(STK_PROVIDER(y), &b, "^GSPC", STK_KIND_INDEX, STK_RANGE_1D, c2);
  spin_ms(200);
  g_assert_cmpuint(fake->requests, ==, 2);
  g_assert_cmpuint(b.calls, ==, 0);
  g_cancellable_cancel(c2);
  wait_for(&b);
  g_assert_error(b.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null(b.series);
  spin_ms(60);
  g_assert_cmpuint(fake->requests, ==, 2);
  outcome_clear(&b);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_deadline_starts_at_send(void)
{
  /* The second request is queued for ~600 ms (the gap) although the deadline
   * is 400 ms: the deadline only counts from the moment it is sent. */
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody, 100}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 400, 600);
  Outcome a = {0}, b = {0};
  fetch_quote(STK_PROVIDER(y), &a, "^GSPC", STK_KIND_INDEX, NULL);
  fetch_quote(STK_PROVIDER(y), &b, "^DJI", STK_KIND_INDEX, NULL);
  wait_for(&a);
  gint64 elapsed_ms = wait_for(&b);
  g_assert_no_error(a.error);
  g_assert_no_error(b.error);
  g_assert_cmpint(elapsed_ms, >=, 500);
  g_assert_cmpuint(fake->requests, ==, 2);
  outcome_clear(&a);
  outcome_clear(&b);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_cancel_queued_operation(void)
{
  /* The first request hangs on the wire; the second waits in the queue for a
   * 30 s gap and is cancelled: it completes at once and sends nothing. */
  Step script[] = {{STEP_HANG, 0, NULL, 0}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 20000, 30000);
  g_autoptr(GCancellable) c1 = g_cancellable_new();
  g_autoptr(GCancellable) c2 = g_cancellable_new();
  g_autoptr(GCancellable) c3 = g_cancellable_new();
  Outcome a = {0}, b = {0}, c = {0};
  fetch_quote(STK_PROVIDER(y), &a, "^GSPC", STK_KIND_INDEX, c1);
  fetch_series(STK_PROVIDER(y), &b, "^DJI", STK_KIND_INDEX, STK_RANGE_1M, c2);
  fetch_quote(STK_PROVIDER(y), &c, "^IXIC", STK_KIND_INDEX, c3);
  g_timeout_add(100, cancel_later, c2);
  gint64 elapsed_ms = wait_for(&b);
  g_assert_null(b.series);
  g_assert_error(b.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpint(elapsed_ms, <, 1500);
  g_assert_cmpuint(a.calls, ==, 0); /* the request on the wire is untouched */
  g_assert_cmpuint(c.calls, ==, 0); /* so is the other queued one */
  g_assert_cmpuint(fake->requests, ==, 1);

  /* The last queued operation too, then the one on the wire. */
  g_cancellable_cancel(c3);
  wait_for(&c);
  g_assert_error(c.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(a.calls, ==, 0);
  g_cancellable_cancel(c1);
  wait_for(&a);
  g_assert_error(a.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  spin_ms(100);
  g_assert_cmpuint(fake->requests, ==, 1); /* nothing else was ever sent */
  g_assert_cmpuint(a.calls + b.calls + c.calls, ==, 3);
  outcome_clear(&a);
  outcome_clear(&b);
  outcome_clear(&c);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_cancel_all_at_once(void)
{
  /* Everything cancelled in one go: the operations behind the one on the wire
   * are already cancelled when the wire frees up and must not be sent. */
  Step script[] = {{STEP_HANG, 0, NULL, 0}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 20000, 0);
  GCancellable *cancels[4];
  Outcome o[4] = {{0}};
  for (guint i = 0; i < 4; i++) {
    cancels[i] = g_cancellable_new();
    fetch_quote(STK_PROVIDER(y), &o[i], "^GSPC", STK_KIND_INDEX, cancels[i]);
  }
  spin_ms(100);
  g_assert_cmpuint(fake->requests, ==, 1);
  for (guint i = 0; i < 4; i++)
    g_cancellable_cancel(cancels[i]);
  for (guint i = 0; i < 4; i++) {
    wait_for(&o[i]);
    g_assert_error(o[i].error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    outcome_clear(&o[i]);
    g_object_unref(cancels[i]);
  }
  spin_ms(100);
  g_assert_cmpuint(fake->requests, ==, 1);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_cancel_from_a_callback(void)
{
  /* The callback of the request that just ended cancels the next queued
   * operation: its cancel watch has not been dispatched yet when the wire
   * frees up, so the queue itself must notice and not start it. */
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody, 20}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 2000, 0);
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  Outcome a = {0}, b = {0}, c = {0};
  a.cancel_after_finish = cancellable;
  fetch_quote(STK_PROVIDER(y), &a, "^GSPC", STK_KIND_INDEX, NULL);
  fetch_quote(STK_PROVIDER(y), &b, "^DJI", STK_KIND_INDEX, cancellable);
  fetch_quote(STK_PROVIDER(y), &c, "^IXIC", STK_KIND_INDEX, NULL);
  wait_for(&a);
  wait_for(&b);
  wait_for(&c);
  g_assert_no_error(a.error);
  g_assert_error(b.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_no_error(c.error); /* the operation behind it is unaffected */
  spin_ms(50);
  g_assert_cmpuint(fake->requests, ==, 2);
  g_assert_cmpstr(g_ptr_array_index(fake->paths, 1), ==,
                  "/v8/finance/chart/%5EIXIC?range=5d&interval=1d");
  outcome_clear(&a);
  outcome_clear(&b);
  outcome_clear(&c);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_cancel_during_request(void)
{
  Step script[] = {{STEP_HANG, 0, NULL, 0}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 10000, 5);
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  Outcome o = {0};
  fetch_quote(STK_PROVIDER(y), &o, "^GSPC", STK_KIND_INDEX, cancellable);
  g_timeout_add(100, cancel_later, cancellable);
  gint64 elapsed_ms = wait_for(&o);
  g_assert_null(o.quote);
  g_assert_error(o.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(fake->requests, ==, 1); /* cancelling is not a reason to retry */
  g_assert_cmpint(elapsed_ms, <, 2000);
  spin_ms(100);
  g_assert_cmpuint(fake->requests, ==, 1);
  outcome_clear(&o);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_cancel_while_waiting_for_retry(void)
{
  /* A 503 puts the retry behind a 30 s gap; cancelling ends it at once. */
  Step script[] = {{STEP_RESPOND, 503, "", 0}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 2000, 30000);
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  Outcome o = {0};
  fetch_quote(STK_PROVIDER(y), &o, "^GSPC", STK_KIND_INDEX, cancellable);
  g_timeout_add(300, cancel_later, cancellable);
  gint64 elapsed_ms = wait_for(&o);
  g_assert_null(o.quote);
  g_assert_error(o.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(fake->requests, ==, 1);
  g_assert_cmpint(elapsed_ms, <, 3000);
  outcome_clear(&o);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_already_cancelled(void)
{
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody, 0}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 2000, 5);
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  g_cancellable_cancel(cancellable);
  Outcome q = {0}, s = {0};
  fetch_quote(STK_PROVIDER(y), &q, "^GSPC", STK_KIND_INDEX, cancellable);
  fetch_series(STK_PROVIDER(y), &s, "^GSPC", STK_KIND_INDEX, STK_RANGE_1Y, cancellable);
  wait_for(&q);
  wait_for(&s);
  g_assert_error(q.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_error(s.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  spin_ms(50);
  g_assert_cmpuint(fake->requests, ==, 0);
  outcome_clear(&q);
  outcome_clear(&s);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_cancel_before_finish(void)
{
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody, 0}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 2000, 5);
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  Outcome o = {0};
  o.cancel_before_finish = cancellable;
  fetch_quote(STK_PROVIDER(y), &o, "^GSPC", STK_KIND_INDEX, cancellable);
  wait_for(&o);
  /* The request succeeded, but cancelling before *_finish wins. */
  g_assert_null(o.quote);
  g_assert_error(o.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(fake->requests, ==, 1);
  outcome_clear(&o);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_inflight_cap(void)
{
  /* One request hangs on the wire (gap 5 min), the others queue: queued
   * operations count toward the cap, the overflow is RATE_LIMITED with no
   * traffic, and operations that finish free their slots. */
  Step script[] = {{STEP_HANG, 0, NULL, 0}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 60000, 300000);
  GCancellable *cancels[STK_YAHOO_MAX_INFLIGHT];
  Outcome o[STK_YAHOO_MAX_INFLIGHT] = {{0}};
  for (guint i = 0; i < STK_YAHOO_MAX_INFLIGHT; i++) {
    cancels[i] = g_cancellable_new();
    fetch_quote(STK_PROVIDER(y), &o[i], "^GSPC", STK_KIND_INDEX, cancels[i]);
  }
  Outcome over = {0}, over_series = {0};
  fetch_quote(STK_PROVIDER(y), &over, "^GSPC", STK_KIND_INDEX, NULL);
  fetch_series(STK_PROVIDER(y), &over_series, "^DJI", STK_KIND_INDEX, STK_RANGE_1M, NULL);
  wait_for(&over);
  wait_for(&over_series);
  g_assert_error(over.error, STK_ERROR, STK_ERROR_RATE_LIMITED);
  g_assert_error(over_series.error, STK_ERROR, STK_ERROR_RATE_LIMITED);
  spin_ms(100); /* the one request on the wire reaches the server */
  g_assert_cmpuint(fake->requests, ==, 1);
  /* Invalid arguments are still reported as such, before the cap. */
  Outcome bad = {0};
  fetch_quote(STK_PROVIDER(y), &bad, "^GSPC", STK_KIND_STOCK, NULL);
  wait_for(&bad);
  g_assert_error(bad.error, STK_ERROR, STK_ERROR_INVALID_ARG);
  for (guint i = 0; i < STK_YAHOO_MAX_INFLIGHT; i++)
    g_assert_cmpuint(o[i].calls, ==, 0);

  /* Cancel a queued one: its slot is free again once its callback has returned. */
  g_cancellable_cancel(cancels[STK_YAHOO_MAX_INFLIGHT - 1]);
  wait_for(&o[STK_YAHOO_MAX_INFLIGHT - 1]);
  g_assert_error(o[STK_YAHOO_MAX_INFLIGHT - 1].error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  spin_ms(30);
  Outcome again = {0};
  g_autoptr(GCancellable) again_cancel = g_cancellable_new();
  fetch_quote(STK_PROVIDER(y), &again, "^GSPC", STK_KIND_INDEX, again_cancel);
  spin_ms(50);
  g_assert_cmpuint(again.calls, ==, 0); /* accepted (queued) */
  g_cancellable_cancel(again_cancel);
  wait_for(&again);
  g_assert_error(again.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

  for (guint i = 0; i < STK_YAHOO_MAX_INFLIGHT; i++) {
    g_cancellable_cancel(cancels[i]);
    wait_for(&o[i]);
    g_assert_error(o[i].error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    outcome_clear(&o[i]);
    g_object_unref(cancels[i]);
  }
  spin_ms(100);
  g_assert_cmpuint(fake->requests, ==, 1);
  outcome_clear(&over);
  outcome_clear(&over_series);
  outcome_clear(&bad);
  outcome_clear(&again);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

static void
test_net_provider_dropped_while_queued(void)
{
  /* Operations keep the provider (and its queue) alive until they complete. */
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody, 0}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 2000, 150);
  Outcome o[3] = {{0}};
  for (guint i = 0; i < 3; i++)
    fetch_quote(STK_PROVIDER(y), &o[i], "^GSPC", STK_KIND_INDEX, NULL);
  gboolean finalized = FALSE;
  g_object_weak_ref(G_OBJECT(y), on_weak_notify, &finalized);
  g_object_unref(y);
  g_assert_false(finalized);
  for (guint i = 0; i < 3; i++) {
    wait_for(&o[i]);
    g_assert_no_error(o[i].error);
    outcome_clear(&o[i]);
  }
  gint64 deadline = g_get_monotonic_time() + 2000 * 1000;
  while (!finalized && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  g_assert_true(finalized);
  g_assert_cmpuint(fake->requests, ==, 3);
  fake_free(fake);
}

static void
test_net_gap_zero_and_exactly_once(void)
{
  /* With no gap, sequential operations still go one at a time and each
   * completes exactly once. */
  Step script[] = {{STEP_RESPOND, 200, kQuoteBody, 20}};
  Fake *fake = fake_new(script, 1);
  StkYahoo *y = provider_for(fake, 2000, 0);
  Outcome o[6] = {{0}};
  for (guint i = 0; i < 6; i++)
    fetch_quote(STK_PROVIDER(y), &o[i], "^GSPC", STK_KIND_INDEX, NULL);
  for (guint i = 0; i < 6; i++)
    wait_for(&o[i]);
  spin_ms(100);
  for (guint i = 0; i < 6; i++) {
    g_assert_cmpuint(o[i].calls, ==, 1);
    g_assert_no_error(o[i].error);
    outcome_clear(&o[i]);
  }
  g_assert_cmpuint(fake->requests, ==, 6);
  g_assert_cmpuint(fake->max_active, ==, 1);
  g_assert_true(released_within(y, 2000));
  fake_free(fake);
}

/* ------------------------------------------------------------------------ */
/* Part 3: router                                                           */
/* ------------------------------------------------------------------------ */

/* A StkProvider that records its calls and answers from the main loop. */
#define TEST_TYPE_FAKE (test_fake_get_type())
G_DECLARE_FINAL_TYPE(TestFake, test_fake, TEST, FAKE, GObject)

struct _TestFake {
  GObject parent_instance;
  char *label; /* used as quote name */
  guint quote_calls, series_calls, search_calls;
  char *symbol;
  StkKind kind;
  StkRange range;
  char *text;
  GCancellable *cancellable; /* the one the last call received (not owned) */
  gboolean fail;             /* complete with STK_ERROR_NOT_FOUND */
  gboolean hang;             /* never complete (until the cancellable is cancelled) */
};

static void test_fake_provider_init(StkProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(TestFake, test_fake, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(STK_TYPE_PROVIDER, test_fake_provider_init))

static void
test_fake_init(TestFake *self)
{
  (void)self;
}

static void
test_fake_finalize(GObject *object)
{
  TestFake *self = TEST_FAKE(object);
  g_free(self->label);
  g_free(self->symbol);
  g_free(self->text);
  G_OBJECT_CLASS(test_fake_parent_class)->finalize(object);
}

static void
test_fake_class_init(TestFakeClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = test_fake_finalize;
}

static TestFake *
test_fake_new(const char *label)
{
  TestFake *f = g_object_new(TEST_TYPE_FAKE, NULL);
  f->label = g_strdup(label);
  return f;
}

static void
fake_record(TestFake *f, const char *symbol, StkKind kind, StkRange range, GCancellable *c)
{
  g_free(f->symbol);
  f->symbol = g_strdup(symbol);
  f->kind = kind;
  f->range = range;
  f->cancellable = c;
}

static gboolean
cancelled_cb(GCancellable *c, gpointer task)
{
  (void)c;
  g_task_return_error(G_TASK(task), g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled"));
  return G_SOURCE_REMOVE;
}

/* Completes @task: fail, hang until cancelled, or succeed with @result. */
static void
fake_finish(TestFake *f, GTask *task, gpointer result, GDestroyNotify destroy)
{
  if (f->fail) {
    g_task_return_new_error(task, STK_ERROR, STK_ERROR_NOT_FOUND, "fake %s failed", f->label);
    if (destroy != NULL)
      destroy(result);
  } else if (f->hang) {
    GCancellable *c = g_task_get_cancellable(task);
    g_assert_nonnull(c);
    GSource *watch = g_cancellable_source_new(c);
    g_source_set_callback(watch, G_SOURCE_FUNC(cancelled_cb), g_object_ref(task), g_object_unref);
    g_source_attach(watch, NULL);
    g_source_unref(watch);
    if (destroy != NULL)
      destroy(result);
  } else {
    g_task_return_pointer(task, result, destroy);
  }
  g_object_unref(task);
}

static void
fake_quote_async(StkProvider *p, const char *symbol, StkKind kind, GCancellable *c,
                 GAsyncReadyCallback cb, gpointer ud)
{
  TestFake *f = TEST_FAKE(p);
  f->quote_calls++;
  fake_record(f, symbol, kind, STK_RANGE_1D, c);
  GTask *task = g_task_new(f, c, cb, ud);
  StkQuote *q = stk_quote_new("^GSPC", f->label, STK_KIND_INDEX, NULL);
  q->price = 1.5;
  fake_finish(f, task, q, (GDestroyNotify)stk_quote_free);
}

static StkQuote *
fake_quote_finish(StkProvider *p, GAsyncResult *r, GError **e)
{
  g_return_val_if_fail(g_task_is_valid(r, p), NULL);
  return g_task_propagate_pointer(G_TASK(r), e);
}

static void
fake_series_async(StkProvider *p, const char *symbol, StkKind kind, StkRange range,
                  GCancellable *c, GAsyncReadyCallback cb, gpointer ud)
{
  TestFake *f = TEST_FAKE(p);
  f->series_calls++;
  fake_record(f, symbol, kind, range, c);
  GTask *task = g_task_new(f, c, cb, ud);
  StkSeries *s = stk_series_new(range, 42.0, 1);
  stk_series_append(s, 1000, f->label[0]);
  fake_finish(f, task, s, (GDestroyNotify)stk_series_unref);
}

static StkSeries *
fake_series_finish(StkProvider *p, GAsyncResult *r, GError **e)
{
  g_return_val_if_fail(g_task_is_valid(r, p), NULL);
  return g_task_propagate_pointer(G_TASK(r), e);
}

static void
fake_search_async(StkProvider *p, const char *text, GCancellable *c, GAsyncReadyCallback cb,
                  gpointer ud)
{
  TestFake *f = TEST_FAKE(p);
  f->search_calls++;
  g_free(f->text);
  f->text = g_strdup(text);
  f->cancellable = c;
  GTask *task = g_task_new(f, c, cb, ud);
  GPtrArray *hits = g_ptr_array_new_with_free_func((GDestroyNotify)stk_search_hit_free);
  for (guint i = 0; i < 2; i++) {
    StkSearchHit *h = g_new0(StkSearchHit, 1);
    h->symbol = g_strdup_printf("%s%u", f->label, i);
    h->name = g_strdup("n");
    h->kind = STK_KIND_STOCK;
    g_ptr_array_add(hits, h);
  }
  fake_finish(f, task, hits, (GDestroyNotify)g_ptr_array_unref);
}

static GPtrArray *
fake_search_finish(StkProvider *p, GAsyncResult *r, GError **e)
{
  g_return_val_if_fail(g_task_is_valid(r, p), NULL);
  return g_task_propagate_pointer(G_TASK(r), e);
}

static void
test_fake_provider_init(StkProviderInterface *iface)
{
  iface->fetch_quote_async = fake_quote_async;
  iface->fetch_quote_finish = fake_quote_finish;
  iface->fetch_series_async = fake_series_async;
  iface->fetch_series_finish = fake_series_finish;
  iface->search_async = fake_search_async;
  iface->search_finish = fake_search_finish;
}

typedef struct {
  TestFake *primary, *index;
  StkRouter *router;
} RouterFixture;

static void
router_fixture_init(RouterFixture *fx)
{
  fx->primary = test_fake_new("PRI");
  fx->index = test_fake_new("IDX");
  fx->router = stk_router_new(STK_PROVIDER(fx->primary), STK_PROVIDER(fx->index));
}

static void
router_fixture_clear(RouterFixture *fx)
{
  g_assert_true(released_within(fx->router, 2000));
  g_object_unref(fx->primary);
  g_object_unref(fx->index);
}

static void
test_router_routes_by_symbol(void)
{
  RouterFixture fx;
  router_fixture_init(&fx);
  static const struct {
    const char *symbol;
    StkKind kind;
    gboolean to_index;
  } cases[] = {
      {"^GSPC", STK_KIND_INDEX, TRUE}, {"^DJI", STK_KIND_INDEX, TRUE},
      {"^GSPC", STK_KIND_STOCK, TRUE}, /* the inner provider rejects the kind, not the router */
      {"^X", STK_KIND_FUND, TRUE},     {"AAPL", STK_KIND_STOCK, FALSE},
      {"VTI", STK_KIND_ETF, FALSE},    {"FXAIX", STK_KIND_FUND, FALSE},
      {"AAPL", STK_KIND_INDEX, FALSE}, /* likewise for Nasdaq */
      {"EURUSD=X", STK_KIND_INDEX, FALSE}, {"A^B", STK_KIND_STOCK, FALSE},
      {"", STK_KIND_STOCK, FALSE},     {"a b", STK_KIND_STOCK, FALSE},
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    TestFake *want = cases[i].to_index ? fx.index : fx.primary;
    TestFake *other = cases[i].to_index ? fx.primary : fx.index;
    guint want_quotes = want->quote_calls, other_quotes = other->quote_calls;
    guint want_series = want->series_calls, other_series = other->series_calls;
    g_autoptr(GCancellable) cancellable = g_cancellable_new();

    Outcome q = {0};
    fetch_quote(STK_PROVIDER(fx.router), &q, cases[i].symbol, cases[i].kind, cancellable);
    wait_for(&q);
    g_assert_no_error(q.error);
    g_assert_true(q.source == G_OBJECT(fx.router)); /* the router is the source object */
    g_assert_cmpstr(q.quote->name, ==, want->label);
    g_assert_cmpfloat(q.quote->price, ==, 1.5);
    g_assert_cmpuint(want->quote_calls, ==, want_quotes + 1);
    g_assert_cmpuint(other->quote_calls, ==, other_quotes);
    /* Arguments and cancellable are passed through unchanged. */
    g_assert_cmpstr(want->symbol, ==, cases[i].symbol);
    g_assert_cmpint(want->kind, ==, cases[i].kind);
    g_assert_true(want->cancellable == cancellable);
    outcome_clear(&q);

    Outcome s = {0};
    fetch_series(STK_PROVIDER(fx.router), &s, cases[i].symbol, cases[i].kind, STK_RANGE_3M, NULL);
    wait_for(&s);
    g_assert_no_error(s.error);
    g_assert_true(s.source == G_OBJECT(fx.router));
    g_assert_cmpint(s.series->range, ==, STK_RANGE_3M);
    g_assert_cmpfloat(s.series->prev_close, ==, 42.0);
    g_assert_cmpfloat(s.series->points[0].v, ==, (double)want->label[0]);
    g_assert_cmpuint(want->series_calls, ==, want_series + 1);
    g_assert_cmpuint(other->series_calls, ==, other_series);
    g_assert_cmpint(want->range, ==, STK_RANGE_3M);
    g_assert_null(want->cancellable);
    outcome_clear(&s);
  }
  router_fixture_clear(&fx);
}

static void
test_router_search_goes_to_primary(void)
{
  RouterFixture fx;
  router_fixture_init(&fx);
  static const char *const texts[] = {"  s&p 500  ", "^GSPC", "dow", "\xff", ""};
  for (guint i = 0; i < G_N_ELEMENTS(texts); i++) {
    g_autoptr(GCancellable) cancellable = g_cancellable_new();
    Outcome o = {0};
    o.started_us = g_get_monotonic_time();
    stk_provider_search_async(STK_PROVIDER(fx.router), texts[i], cancellable, on_search_done, &o);
    g_assert_cmpuint(o.calls, ==, 0);
    wait_for(&o);
    g_assert_no_error(o.error);
    g_assert_true(o.source == G_OBJECT(fx.router));
    g_assert_cmpuint(o.hits->len, ==, 2);
    g_assert_cmpstr(((StkSearchHit *)g_ptr_array_index(o.hits, 0))->symbol, ==, "PRI0");
    g_assert_cmpuint(fx.primary->search_calls, ==, i + 1);
    g_assert_cmpstr(fx.primary->text, ==, texts[i]); /* unchanged */
    g_assert_true(fx.primary->cancellable == cancellable);
    outcome_clear(&o);
  }
  g_assert_cmpuint(fx.index->search_calls, ==, 0);
  router_fixture_clear(&fx);
}

static void
test_router_errors_and_cancellation(void)
{
  RouterFixture fx;
  router_fixture_init(&fx);

  /* Inner errors come through unchanged. */
  fx.index->fail = TRUE;
  Outcome q = {0};
  fetch_quote(STK_PROVIDER(fx.router), &q, "^GSPC", STK_KIND_INDEX, NULL);
  wait_for(&q);
  g_assert_null(q.quote);
  g_assert_error(q.error, STK_ERROR, STK_ERROR_NOT_FOUND);
  g_assert_cmpstr(q.error->message, ==, "fake IDX failed");
  outcome_clear(&q);
  Outcome s = {0};
  fetch_series(STK_PROVIDER(fx.router), &s, "^GSPC", STK_KIND_INDEX, STK_RANGE_1D, NULL);
  wait_for(&s);
  g_assert_null(s.series);
  g_assert_error(s.error, STK_ERROR, STK_ERROR_NOT_FOUND);
  outcome_clear(&s);
  fx.primary->fail = TRUE;
  Outcome h = {0};
  h.started_us = g_get_monotonic_time();
  stk_provider_search_async(STK_PROVIDER(fx.router), "x", NULL, on_search_done, &h);
  wait_for(&h);
  g_assert_null(h.hits);
  g_assert_error(h.error, STK_ERROR, STK_ERROR_NOT_FOUND);
  outcome_clear(&h);
  fx.index->fail = FALSE;
  fx.primary->fail = FALSE;

  /* A cancellation reaches the inner operation and comes back as CANCELLED. */
  fx.index->hang = TRUE;
  fx.primary->hang = TRUE;
  g_autoptr(GCancellable) c1 = g_cancellable_new();
  g_autoptr(GCancellable) c2 = g_cancellable_new();
  g_autoptr(GCancellable) c3 = g_cancellable_new();
  Outcome a = {0}, b = {0}, c = {0};
  fetch_quote(STK_PROVIDER(fx.router), &a, "^GSPC", STK_KIND_INDEX, c1);
  fetch_series(STK_PROVIDER(fx.router), &b, "AAPL", STK_KIND_STOCK, STK_RANGE_1M, c2);
  c.started_us = g_get_monotonic_time();
  stk_provider_search_async(STK_PROVIDER(fx.router), "x", c3, on_search_done, &c);
  spin_ms(50);
  g_assert_cmpuint(a.calls + b.calls + c.calls, ==, 0);
  g_cancellable_cancel(c1);
  g_cancellable_cancel(c2);
  g_cancellable_cancel(c3);
  wait_for(&a);
  wait_for(&b);
  wait_for(&c);
  g_assert_error(a.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_error(b.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_error(c.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  outcome_clear(&a);
  outcome_clear(&b);
  outcome_clear(&c);
  fx.index->hang = FALSE;
  fx.primary->hang = FALSE;

  /* Cancelling before *_finish wins even though the inner operation succeeded. */
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  Outcome o = {0};
  o.cancel_before_finish = cancellable;
  fetch_quote(STK_PROVIDER(fx.router), &o, "AAPL", STK_KIND_STOCK, cancellable);
  wait_for(&o);
  g_assert_null(o.quote);
  g_assert_error(o.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  outcome_clear(&o);
  spin_ms(30);
  router_fixture_clear(&fx);
}

static void
test_router_ownership(void)
{
  /* The router owns references: the creator may drop its own at once, and an
   * operation keeps the router (and so the inner providers) alive. */
  TestFake *primary = test_fake_new("PRI");
  TestFake *index = test_fake_new("IDX");
  gboolean primary_gone = FALSE, index_gone = FALSE, router_gone = FALSE;
  g_object_weak_ref(G_OBJECT(primary), on_weak_notify, &primary_gone);
  g_object_weak_ref(G_OBJECT(index), on_weak_notify, &index_gone);
  StkRouter *router = stk_router_new(STK_PROVIDER(primary), STK_PROVIDER(index));
  g_object_weak_ref(G_OBJECT(router), on_weak_notify, &router_gone);
  g_object_unref(primary);
  g_object_unref(index);
  g_assert_false(primary_gone || index_gone);
  Outcome o = {0};
  fetch_quote(STK_PROVIDER(router), &o, "^DJI", STK_KIND_INDEX, NULL);
  g_object_unref(router);
  g_assert_false(router_gone);
  wait_for(&o);
  g_assert_no_error(o.error);
  g_assert_cmpstr(o.quote->name, ==, "IDX");
  outcome_clear(&o);
  gint64 deadline = g_get_monotonic_time() + 2000 * 1000;
  while (!(router_gone && primary_gone && index_gone) && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  g_assert_true(router_gone && primary_gone && index_gone);

  /* The same provider may serve both roles. */
  TestFake *both = test_fake_new("ALL");
  StkRouter *same = stk_router_new(STK_PROVIDER(both), STK_PROVIDER(both));
  g_object_unref(both);
  Outcome a = {0}, b = {0};
  fetch_quote(STK_PROVIDER(same), &a, "^GSPC", STK_KIND_INDEX, NULL);
  fetch_quote(STK_PROVIDER(same), &b, "AAPL", STK_KIND_STOCK, NULL);
  wait_for(&a);
  wait_for(&b);
  g_assert_cmpstr(a.quote->name, ==, "ALL");
  g_assert_cmpstr(b.quote->name, ==, "ALL");
  outcome_clear(&a);
  outcome_clear(&b);
  g_assert_true(released_within(same, 2000));
}

/* Real providers behind the router, each wired to its own loopback server. */
static const char *const kNasdaqQuoteBody =
    "{\"data\":{\"companyName\":\"Test Co\",\"marketStatus\":\"Closed\","
    "\"primaryData\":{\"lastSalePrice\":\"$12.34\",\"netChange\":\"+0.10\","
    "\"percentageChange\":\"+0.82%\",\"lastTradeTimestamp\":\"Oct 8, 2026 4:00 PM ET\","
    "\"isRealTime\":false}},\"status\":{\"rCode\":200}}";
static const char *const kNasdaqSearchBody =
    "{\"data\":[{\"symbol\":\"AAPL\",\"name\":\"Apple\",\"asset\":\"STOCKS\"}],"
    "\"status\":{\"rCode\":200}}";

static void
test_router_with_real_providers(void)
{
  Step yahoo_script[] = {{STEP_RESPOND, 200, kQuoteBody, 0}, {STEP_RESPOND, 200, kChartBody1d, 0},
                         {STEP_RESPOND, 200, kQuoteBody, 0}};
  Step nasdaq_script[] = {{STEP_RESPOND, 200, kNasdaqQuoteBody, 0},
                          {STEP_RESPOND, 200, kNasdaqSearchBody, 0}};
  Fake *yahoo_fake = fake_new(yahoo_script, G_N_ELEMENTS(yahoo_script));
  Fake *nasdaq_fake = fake_new(nasdaq_script, G_N_ELEMENTS(nasdaq_script));
  StkYahoo *yahoo = provider_for(yahoo_fake, 2000, 10);
  g_autofree char *base = g_strdup_printf("http://127.0.0.1:%u", nasdaq_fake->port);
  StkNasdaq *nasdaq = stk_nasdaq_new_custom(base, 2000, 10);
  g_assert_nonnull(nasdaq);
  StkRouter *router = stk_router_new(STK_PROVIDER(nasdaq), STK_PROVIDER(yahoo));
  g_object_unref(nasdaq);
  g_object_unref(yahoo);

  /* '^' index -> Yahoo only. */
  Outcome o = {0};
  fetch_quote(STK_PROVIDER(router), &o, "^GSPC", STK_KIND_INDEX, NULL);
  wait_for(&o);
  g_assert_no_error(o.error);
  g_assert_cmpstr(o.quote->symbol, ==, "^GSPC");
  g_assert_cmpint(o.quote->kind, ==, STK_KIND_INDEX);
  g_assert_cmpfloat_with_epsilon(o.quote->price, 7805.45, 1e-9);
  g_assert_cmpuint(yahoo_fake->requests, ==, 1);
  g_assert_cmpuint(nasdaq_fake->requests, ==, 0);
  outcome_clear(&o);

  Outcome s = {0};
  fetch_series(STK_PROVIDER(router), &s, "^GSPC", STK_KIND_INDEX, STK_RANGE_1D, NULL);
  wait_for(&s);
  g_assert_no_error(s.error);
  g_assert_cmpuint(s.series->n_points, ==, 7);
  g_assert_cmpfloat(s.series->prev_close, ==, 7765.35986328125);
  g_assert_cmpuint(yahoo_fake->requests, ==, 3); /* the chart and the previous close */
  g_assert_cmpuint(nasdaq_fake->requests, ==, 0);
  outcome_clear(&s);

  /* Anything else -> Nasdaq only. */
  Outcome n = {0};
  fetch_quote(STK_PROVIDER(router), &n, "AAPL", STK_KIND_STOCK, NULL);
  wait_for(&n);
  g_assert_no_error(n.error);
  g_assert_cmpstr(n.quote->symbol, ==, "AAPL");
  g_assert_cmpstr(n.quote->name, ==, "Test Co");
  g_assert_cmpuint(yahoo_fake->requests, ==, 3);
  g_assert_cmpuint(nasdaq_fake->requests, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(nasdaq_fake->paths, 0), ==,
                  "/api/quote/AAPL/info?assetclass=stocks");
  outcome_clear(&n);

  /* Search: Nasdaq, never Yahoo. */
  Outcome h = {0};
  h.started_us = g_get_monotonic_time();
  stk_provider_search_async(STK_PROVIDER(router), "apple", NULL, on_search_done, &h);
  wait_for(&h);
  g_assert_no_error(h.error);
  g_assert_cmpuint(h.hits->len, ==, 1);
  g_assert_cmpstr(((StkSearchHit *)g_ptr_array_index(h.hits, 0))->symbol, ==, "AAPL");
  g_assert_cmpuint(yahoo_fake->requests, ==, 3);
  g_assert_cmpuint(nasdaq_fake->requests, ==, 2);
  g_assert_true(g_str_has_prefix(g_ptr_array_index(nasdaq_fake->paths, 1), "/api/autocomplete/"));
  outcome_clear(&h);

  /* Mismatched kind and symbol: rejected by the inner provider, no traffic. */
  static const struct {
    const char *symbol;
    StkKind kind;
  } rejected[] = {
      {"^GSPC", STK_KIND_STOCK}, {"^GSPC", STK_KIND_ETF}, {"^GSPC", STK_KIND_FUND},
      {"AAPL", STK_KIND_INDEX},  {"SPY", STK_KIND_INDEX}, {"EURUSD=X", STK_KIND_INDEX},
  };
  for (guint i = 0; i < G_N_ELEMENTS(rejected); i++) {
    Outcome q = {0}, r = {0};
    fetch_quote(STK_PROVIDER(router), &q, rejected[i].symbol, rejected[i].kind, NULL);
    wait_for(&q);
    g_assert_error(q.error, STK_ERROR, STK_ERROR_INVALID_ARG);
    g_assert_true(q.source == G_OBJECT(router));
    fetch_series(STK_PROVIDER(router), &r, rejected[i].symbol, rejected[i].kind, STK_RANGE_1M,
                 NULL);
    wait_for(&r);
    g_assert_error(r.error, STK_ERROR, STK_ERROR_INVALID_ARG);
    outcome_clear(&q);
    outcome_clear(&r);
  }
  spin_ms(50);
  g_assert_cmpuint(yahoo_fake->requests, ==, 3);
  g_assert_cmpuint(nasdaq_fake->requests, ==, 2);

  /* Yahoo's search is empty and offline; the router never uses it anyway. */
  g_assert_true(released_within(router, 2000));
  fake_free(yahoo_fake);
  fake_free(nasdaq_fake);
}

/* ------------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_setenv("GIO_USE_PROXY_RESOLVER", "dummy", TRUE);
  if (argc < 2) {
    g_printerr("usage: %s FIXTURE_DIR\n", argv[0]);
    return 1;
  }
  fixture_dir = argv[1];
  gsize len;
  kQuoteBody = load_fixture("chart_5d_index.json", &len);
  kChartBody1d = load_fixture("chart_1d_index.json", &len);
  kSeriesBody1y = load_fixture("chart_1y_index.json", &len);

  g_test_add_func("/indices/url/builder", test_url_builder);
  g_test_add_func("/indices/url/rejections", test_url_builder_rejections);
  g_test_add_func("/indices/series/1d-fixture", test_series_1d_fixture);
  g_test_add_func("/indices/series/daily-fixture", test_series_daily_fixture);
  g_test_add_func("/indices/series/1d-filters", test_series_1d_filters);
  g_test_add_func("/indices/series/no-meta-prev-close", test_series_ignores_meta_prev_close);
  g_test_add_func("/indices/series/1d-needs-period", test_series_1d_requires_trading_period);
  g_test_add_func("/indices/series/daily-dates", test_series_daily_dates);
  g_test_add_func("/indices/series/daily-dst", test_series_daily_dst_boundary);
  g_test_add_func("/indices/series/daily-same-date-latest",
                  test_series_daily_same_date_latest_raw_sample_wins);
  g_test_add_func("/indices/series/unusable-samples", test_series_skips_unusable_samples);
  g_test_add_func("/indices/series/length-mismatch", test_series_array_length_mismatch);
  g_test_add_func("/indices/series/empty-and-shapes", test_series_empty_and_shapes);
  g_test_add_func("/indices/parse/errors", test_parse_errors);
  g_test_add_func("/indices/parse/chart-error", test_chart_error_is_not_found);
  g_test_add_func("/indices/quote/fixture", test_quote_fixture);
  g_test_add_func("/indices/quote/fields", test_quote_fields);
  g_test_add_func("/indices/quote/prev-close", test_prev_close);
  g_test_add_func("/indices/quote/text-and-time", test_quote_sanitizes_text_and_time);
  g_test_add_func("/indices/quote/errors", test_quote_errors);
  g_test_add_func("/indices/provider/constants-and-origins", test_constants_and_origin_policy);

  g_test_add_func("/indices/net/request-shape", test_net_request_shape_and_results);
  g_test_add_func("/indices/net/range-paths", test_net_all_range_paths);
  g_test_add_func("/indices/net/trailing-slash", test_net_base_url_trailing_slash);
  g_test_add_func("/indices/net/invalid-args", test_net_invalid_args);
  g_test_add_func("/indices/net/search-empty", test_net_search_is_empty_and_offline);
  g_test_add_func("/indices/net/status-mapping", test_net_status_mapping);
  g_test_add_func("/indices/net/retry-rules", test_net_retry_rules);
  g_test_add_func("/indices/net/refused", test_net_refused_connection);
  g_test_add_func("/indices/net/retry-then-success", test_net_retry_then_success);
  g_test_add_func("/indices/net/429-after-5xx", test_net_429_after_5xx_is_final);
  g_test_add_func("/indices/net/gzip", test_net_gzip_body);
  g_test_add_func("/indices/net/timeout", test_net_timeout_per_attempt);
  g_test_add_func("/indices/net/pacing", test_net_pacing_between_requests);
  g_test_add_func("/indices/net/one-on-the-wire", test_net_one_request_on_the_wire);
  g_test_add_func("/indices/net/retries-paced", test_net_retries_are_paced);
  g_test_add_func("/indices/net/1d-prev-close", test_net_1d_prev_close_request);
  g_test_add_func("/indices/net/1d-cancel-second", test_net_1d_cancel_second_request);
  g_test_add_func("/indices/net/deadline-at-send", test_net_deadline_starts_at_send);
  g_test_add_func("/indices/net/cancel-queued", test_net_cancel_queued_operation);
  g_test_add_func("/indices/net/cancel-all", test_net_cancel_all_at_once);
  g_test_add_func("/indices/net/cancel-from-callback", test_net_cancel_from_a_callback);
  g_test_add_func("/indices/net/cancel-in-request", test_net_cancel_during_request);
  g_test_add_func("/indices/net/cancel-waiting-retry", test_net_cancel_while_waiting_for_retry);
  g_test_add_func("/indices/net/pre-cancelled", test_net_already_cancelled);
  g_test_add_func("/indices/net/cancel-before-finish", test_net_cancel_before_finish);
  g_test_add_func("/indices/net/inflight-cap", test_net_inflight_cap);
  g_test_add_func("/indices/net/provider-dropped", test_net_provider_dropped_while_queued);
  g_test_add_func("/indices/net/gap-zero-exactly-once", test_net_gap_zero_and_exactly_once);

  g_test_add_func("/indices/router/routing", test_router_routes_by_symbol);
  g_test_add_func("/indices/router/search", test_router_search_goes_to_primary);
  g_test_add_func("/indices/router/errors-cancellation", test_router_errors_and_cancellation);
  g_test_add_func("/indices/router/ownership", test_router_ownership);
  g_test_add_func("/indices/router/real-providers", test_router_with_real_providers);

  int rc = g_test_run();
  g_free(kQuoteBody);
  g_free(kChartBody1d);
  g_free(kSeriesBody1y);
  return rc;
}
