/* test-chart-geom.c - unit tests for the pure chart geometry (no GTK). */
#include <float.h>
#include <locale.h>
#include <math.h>
#include <string.h>

#include "stk-chart-geom.h"
#include "stk-market.h"

#define MAX_TICKS STK_CHART_GEOM_MAX_TICKS

static gint64
utc_ms(int y, int mo, int d, int h, int mi)
{
  g_autoptr(GDateTime) dt = g_date_time_new_utc(y, mo, d, h, mi, 0);
  g_assert_nonnull(dt);
  return g_date_time_to_unix(dt) * 1000;
}

/* New York local hour of an instant. */
static int
ny_hour(gint64 ms)
{
  g_autoptr(GTimeZone) tz = g_time_zone_new_identifier("America/New_York");
  g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc(ms / 1000);
  g_autoptr(GDateTime) local = g_date_time_to_timezone(utc, tz);
  return g_date_time_get_hour(local) * 100 + g_date_time_get_minute(local);
}

/* ------------------------------------------------------------------ */

static void
test_is_up(void)
{
  double v[3] = { 1.5, 2.5, 0.5 };
  g_assert_true(stk_chart_geom_is_up(NULL, 0));
  g_assert_true(stk_chart_geom_is_up(v, 1));
  g_assert_false(stk_chart_geom_is_up(v, 3)); /* 0.5 < 1.5 */
  double flat[2] = { 4.0, 4.0 };
  g_assert_true(stk_chart_geom_is_up(flat, 2));
}

static void
test_domain(void)
{
  double t0 = -1, t1 = -1;
  double empty[1] = { 0 };
  g_assert_false(stk_chart_geom_domain(STK_RANGE_1D, empty, 0, &t0, &t1));
  g_assert_false(stk_chart_geom_domain(STK_RANGE_1M, NULL, 0, &t0, &t1));
  g_assert_cmpfloat(t0, ==, -1.0);

  /* Thursday 2026-10-08 regular session, EDT: 13:30Z - 20:00Z. */
  GDate date;
  g_date_set_dmy(&date, 8, G_DATE_OCTOBER, 2026);
  gint64 s, e;
  g_assert_true(stk_market_session(&date, &s, &e));
  g_assert_cmpint(s, ==, utc_ms(2026, 10, 8, 13, 30));
  g_assert_cmpint(e, ==, utc_ms(2026, 10, 8, 20, 0));

  double day[2] = { (double)utc_ms(2026, 10, 8, 14, 0), (double)utc_ms(2026, 10, 8, 15, 0) };
  g_assert_true(stk_chart_geom_domain(STK_RANGE_1D, day, 2, &t0, &t1));
  g_assert_cmpfloat(t0, ==, (double)s);
  g_assert_cmpfloat(t1, ==, (double)e);

  /* One sample: still the whole session. */
  g_assert_true(stk_chart_geom_domain(STK_RANGE_1D, day, 1, &t0, &t1));
  g_assert_cmpfloat(t1, ==, (double)e);

  /* Other ranges: first..last. */
  g_assert_true(stk_chart_geom_domain(STK_RANGE_1M, day, 2, &t0, &t1));
  g_assert_cmpfloat(t0, ==, day[0]);
  g_assert_cmpfloat(t1, ==, day[1]);
  g_assert_true(stk_chart_geom_domain(STK_RANGE_1Y, day, 1, &t0, &t1));
  g_assert_cmpfloat(t0, ==, t1);

  /* 1D on a Saturday (no session): falls back to first..last. */
  double sat[2] = { (double)utc_ms(2026, 10, 10, 15, 0), (double)utc_ms(2026, 10, 10, 16, 0) };
  g_assert_true(stk_chart_geom_domain(STK_RANGE_1D, sat, 2, &t0, &t1));
  g_assert_cmpfloat(t0, ==, sat[0]);
  g_assert_cmpfloat(t1, ==, sat[1]);

  /* 1D outside the calendar's supported years: also first..last. */
  double old[1] = { 1000.0 };
  g_assert_true(stk_chart_geom_domain(STK_RANGE_1D, old, 1, &t0, &t1));
  g_assert_cmpfloat(t0, ==, 1000.0);
}

static void
test_y_range(void)
{
  double lo = 0, hi = 0;
  g_assert_false(stk_chart_geom_y_range(NULL, 0, NAN, 0.1, &lo, &hi));
  g_assert_false(stk_chart_geom_y_range(NULL, 0, INFINITY, 0.1, &lo, &hi));
  g_assert_cmpfloat(lo, ==, 0.0);

  double v[3] = { 10.0, 20.0, 15.0 };
  g_assert_true(stk_chart_geom_y_range(v, 3, NAN, 0.1, &lo, &hi));
  g_assert_cmpfloat_with_epsilon(lo, 9.0, 1e-12);
  g_assert_cmpfloat_with_epsilon(hi, 21.0, 1e-12);

  /* include widens the range before padding */
  g_assert_true(stk_chart_geom_y_range(v, 3, 30.0, 0.1, &lo, &hi));
  g_assert_cmpfloat_with_epsilon(lo, 8.0, 1e-12);
  g_assert_cmpfloat_with_epsilon(hi, 32.0, 1e-12);
  g_assert_true(stk_chart_geom_y_range(v, 3, 5.0, 0.0, &lo, &hi));
  g_assert_cmpfloat(lo, ==, 5.0);
  g_assert_cmpfloat(hi, ==, 20.0);

  /* single point and flat series: hi > lo, centred on the value */
  double one[1] = { 100.0 };
  g_assert_true(stk_chart_geom_y_range(one, 1, NAN, 0.08, &lo, &hi));
  g_assert_cmpfloat(hi, >, lo);
  g_assert_cmpfloat_with_epsilon(lo, 99.0, 1e-12);
  g_assert_cmpfloat_with_epsilon(hi, 101.0, 1e-12);
  double zero[2] = { 0.0, 0.0 };
  g_assert_true(stk_chart_geom_y_range(zero, 2, NAN, 0.08, &lo, &hi));
  g_assert_cmpfloat_with_epsilon(lo, -0.01, 1e-12);
  g_assert_cmpfloat_with_epsilon(hi, 0.01, 1e-12);

  /* only the include value */
  g_assert_true(stk_chart_geom_y_range(NULL, 0, 50.0, 0.1, &lo, &hi));
  g_assert_cmpfloat(hi, >, lo);

  /* non-finite entries are ignored; negative pad treated as 0 */
  double bad[4] = { NAN, 3.0, INFINITY, 4.0 };
  g_assert_true(stk_chart_geom_y_range(bad, 4, NAN, -1.0, &lo, &hi));
  g_assert_cmpfloat(lo, ==, 3.0);
  g_assert_cmpfloat(hi, ==, 4.0);
  double all_bad[2] = { NAN, INFINITY };
  g_assert_false(stk_chart_geom_y_range(all_bad, 2, NAN, 0.1, &lo, &hi));
}

static void
test_map(void)
{
  g_assert_cmpfloat(stk_chart_geom_map(5, 0, 10, 100, 200), ==, 150.0);
  g_assert_cmpfloat(stk_chart_geom_map(0, 0, 10, 100, 200), ==, 100.0);
  g_assert_cmpfloat(stk_chart_geom_map(10, 0, 10, 100, 200), ==, 200.0);
  g_assert_cmpfloat(stk_chart_geom_map(2.5, 0, 10, 80, 0), ==, 60.0); /* flipped */
  g_assert_cmpfloat(stk_chart_geom_map(20, 0, 10, 0, 100), ==, 200.0); /* no clamp */
  g_assert_cmpfloat(stk_chart_geom_map(7, 3, 3, 0, 100), ==, 50.0);    /* degenerate */
  double x = stk_chart_geom_map(37.0, 20, 60, 5, 405);
  g_assert_cmpfloat_with_epsilon(stk_chart_geom_map(x, 5, 405, 20, 60), 37.0, 1e-12);
}

/* ---- decimation ---------------------------------------------------- */

static void
test_decimate_trivial(void)
{
  gsize out[8];
  g_assert_cmpuint(stk_chart_geom_decimate(NULL, NULL, 0, 0, 1, 4, out, 8), ==, 0);

  double t1[1] = { 5.0 }, v1[1] = { 1.0 };
  g_assert_cmpuint(stk_chart_geom_decimate(t1, v1, 1, 5, 5, 4, out, 8), ==, 1);
  g_assert_cmpuint(out[0], ==, 0);
  g_assert_cmpuint(stk_chart_geom_decimate(t1, v1, 1, 0, 10, 0, out, 8), ==, 0);
  g_assert_cmpuint(stk_chart_geom_decimate(t1, v1, 1, 0, 10, 4, out, 0), ==, 0);

  /* two points far apart keep both */
  double t2[2] = { 0, 10 }, v2[2] = { 3, 1 };
  g_assert_cmpuint(stk_chart_geom_decimate(t2, v2, 2, 0, 10, 10, out, 8), ==, 2);
  g_assert_cmpuint(out[0], ==, 0);
  g_assert_cmpuint(out[1], ==, 1);

  /* flat series in one column: one index (the first) */
  double t3[3] = { 0, 1, 2 }, v3[3] = { 7, 7, 7 };
  g_assert_cmpuint(stk_chart_geom_decimate(t3, v3, 3, 0, 100, 10, out, 8), ==, 1);
  g_assert_cmpuint(out[0], ==, 0);

  /* degenerate domain: everything in column 0 -> min and max only */
  double t4[4] = { 5, 5, 5, 5 }, v4[4] = { 2, 9, 1, 4 };
  g_assert_cmpuint(stk_chart_geom_decimate(t4, v4, 4, 5, 5, 10, out, 8), ==, 2);
  g_assert_cmpuint(out[0], ==, 1); /* max at 1 comes before min at 2 */
  g_assert_cmpuint(out[1], ==, 2);

  /* non-finite values are skipped */
  double t5[3] = { 0, 1, 2 }, v5[3] = { NAN, 5, INFINITY };
  g_assert_cmpuint(stk_chart_geom_decimate(t5, v5, 3, 0, 2, 3, out, 8), ==, 1);
  g_assert_cmpuint(out[0], ==, 1);

  /* truncation at out_cap, never beyond */
  gsize small[3] = { 99, 99, 99 };
  double t6[6] = { 0, 1, 2, 3, 4, 5 }, v6[6] = { 1, 2, 3, 4, 5, 6 };
  g_assert_cmpuint(stk_chart_geom_decimate(t6, v6, 6, 0, 5, 6, small, 2), ==, 2);
  g_assert_cmpuint(small[2], ==, 99);
}

static void
test_decimate_large(void)
{
  enum { N = 10000, COLS = 300 };
  double *t = g_new(double, N), *v = g_new(double, N);
  GRand *rnd = g_rand_new_with_seed(42);
  for (int i = 0; i < N; i++) {
    t[i] = 1000.0 + i * 10.0;
    v[i] = 100.0 + 10.0 * sin(i / 200.0) + g_rand_double_range(rnd, -1, 1);
  }
  v[6123] = 500.0; /* global max spike */
  v[3777] = -50.0; /* global min dip   */
  g_rand_free(rnd);

  gsize out[2 * COLS];
  gsize n = stk_chart_geom_decimate(t, v, N, t[0], t[N - 1], COLS, out, G_N_ELEMENTS(out));
  g_assert_cmpuint(n, <=, 2 * COLS);
  g_assert_cmpuint(n, >, COLS / 2);

  gboolean has_max = FALSE, has_min = FALSE;
  for (gsize k = 0; k < n; k++) {
    g_assert_cmpuint(out[k], <, N);
    if (k > 0)
      g_assert_cmpuint(out[k], >, out[k - 1]);
    has_max |= out[k] == 6123;
    has_min |= out[k] == 3777;
  }
  g_assert_true(has_max);
  g_assert_true(has_min);

  /* at most 2 per pixel column */
  gsize per_col[COLS] = { 0 };
  double span = t[N - 1] - t[0];
  for (gsize k = 0; k < n; k++) {
    gsize c = (gsize)((t[out[k]] - t[0]) / span * COLS);
    if (c >= COLS)
      c = COLS - 1;
    per_col[c]++;
  }
  for (gsize c = 0; c < COLS; c++)
    g_assert_cmpuint(per_col[c], <=, 2);

  g_free(t);
  g_free(v);
}

static void
test_decimate_window_domain(void)
{
  /* samples before/after the domain clamp into the edge columns */
  double t[5] = { -100, 5, 6, 7, 500 }, v[5] = { 1, 2, 3, 4, 5 };
  gsize out[8];
  gsize n = stk_chart_geom_decimate(t, v, 5, 0, 10, 2, out, 8);
  g_assert_cmpuint(n, ==, 3);
  /* column 0 = t < 5: only -100 -> idx 0; column 1 = t >= 5: idx 1 (min) and 4 (max) */
  g_assert_cmpuint(out[0], ==, 0);
  g_assert_cmpuint(out[1], ==, 1);
  g_assert_cmpuint(out[2], ==, 4);
}

/* ---- ticks --------------------------------------------------------- */

static void
test_nice_ticks_basic(void)
{
  double out[MAX_TICKS];
  g_assert_cmpuint(stk_chart_geom_nice_ticks(0, 100, 5, 0, out), ==, 3);
  g_assert_cmpfloat(out[0], ==, 0.0);
  g_assert_cmpfloat(out[1], ==, 50.0);
  g_assert_cmpfloat(out[2], ==, 100.0);

  gsize n = stk_chart_geom_nice_ticks(99.5, 101.5, 5, 0.01, out);
  g_assert_cmpuint(n, ==, 5);
  g_assert_cmpfloat_with_epsilon(out[0], 99.5, 1e-9);
  g_assert_cmpfloat_with_epsilon(out[4], 101.5, 1e-9);

  /* min_step is honoured */
  n = stk_chart_geom_nice_ticks(1.000, 1.004, 5, 0.01, out);
  for (gsize i = 1; i < n; i++)
    g_assert_cmpfloat(out[i] - out[i - 1], >=, 0.01 - 1e-12);

  /* invalid input */
  g_assert_cmpuint(stk_chart_geom_nice_ticks(5, 5, 5, 0, out), ==, 0);
  g_assert_cmpuint(stk_chart_geom_nice_ticks(6, 5, 5, 0, out), ==, 0);
  g_assert_cmpuint(stk_chart_geom_nice_ticks(NAN, 5, 5, 0, out), ==, 0);
  g_assert_cmpuint(stk_chart_geom_nice_ticks(0, INFINITY, 5, 0, out), ==, 0);
  g_assert_cmpuint(stk_chart_geom_nice_ticks(0, 10, 0, 0, out), ==, 0);

  /* max_ticks beyond the cap is clamped */
  n = stk_chart_geom_nice_ticks(0, 1000, 100, 0, out);
  g_assert_cmpuint(n, <=, MAX_TICKS);
  g_assert_cmpuint(n, >, 0);

  /* a single tick when only one fits */
  n = stk_chart_geom_nice_ticks(0.3, 0.7, 1, 0, out);
  g_assert_cmpuint(n, ==, 1);
  g_assert_cmpfloat(out[0], >=, 0.3);
  g_assert_cmpfloat(out[0], <=, 0.7);
}

static void
test_nice_ticks_property(void)
{
  GRand *rnd = g_rand_new_with_seed(7);
  for (int iter = 0; iter < 5000; iter++) {
    double mag = pow(10.0, g_rand_double_range(rnd, -2, 5));
    double lo = g_rand_double_range(rnd, 0, mag * 10);
    double span = mag * g_rand_double_range(rnd, 0.01, 3);
    gsize max = (gsize)g_rand_int_range(rnd, 1, 9);
    double out[MAX_TICKS];
    double hi = lo + span;
    gsize n = stk_chart_geom_nice_ticks(lo, hi, max, 0.01, out);
    g_assert_cmpuint(n, <=, max);
    if (span < 0.02)
      continue; /* min_step may legitimately leave no tick */
    /* With 3+ ticks allowed, some 1-2-5 spacing always fits a multiple. */
    if (max >= 3)
      g_assert_cmpuint(n, >=, 1);
    for (gsize i = 0; i < n; i++) {
      g_assert_cmpfloat(out[i], >=, lo - 1e-6 * span);
      g_assert_cmpfloat(out[i], <=, hi + 1e-6 * span);
      if (i > 0)
        g_assert_cmpfloat(out[i], >, out[i - 1]);
    }
    if (n >= 2) {
      /* spacing is 1, 2 or 5 times a power of ten */
      double step = out[1] - out[0];
      double e = pow(10.0, floor(log10(step) + 1e-9));
      double m = step / e;
      g_assert_true(fabs(m - 1) < 1e-6 || fabs(m - 2) < 1e-6 || fabs(m - 5) < 1e-6 ||
                    fabs(m - 10) < 1e-6);
      g_assert_cmpfloat(step, >=, 0.01 - 1e-12);
    }
  }
  g_rand_free(rnd);
}

static void
test_time_ticks_1d(void)
{
  gint64 s = utc_ms(2026, 10, 8, 13, 30), e = utc_ms(2026, 10, 8, 20, 0);
  gint64 out[MAX_TICKS];
  char label[STK_CHART_GEOM_LABEL_LEN];

  gsize n = stk_chart_geom_time_ticks(STK_RANGE_1D, s, e, 5, out);
  g_assert_cmpuint(n, ==, 4); /* 10, 12, 2, 4 */
  const char *want[4] = { "10 AM", "12 PM", "2 PM", "4 PM" };
  for (gsize i = 0; i < n; i++) {
    stk_chart_geom_format_tick(STK_RANGE_1D, out[i], label, sizeof label);
    g_assert_cmpstr(label, ==, want[i]);
  }

  n = stk_chart_geom_time_ticks(STK_RANGE_1D, s, e, 8, out);
  g_assert_cmpuint(n, ==, 7); /* 10..16 hourly */
  g_assert_cmpint(out[1] - out[0], ==, 3600 * 1000);

  n = stk_chart_geom_time_ticks(STK_RANGE_1D, s, e, 1, out);
  g_assert_cmpuint(n, ==, 1);
  g_assert_cmpint(ny_hour(out[0]) % 100, ==, 0);

  /* early-close style domain 9:30-13:00 */
  n = stk_chart_geom_time_ticks(STK_RANGE_1D, s, utc_ms(2026, 10, 8, 17, 0), 5, out);
  g_assert_cmpuint(n, ==, 4); /* 10, 11, 12, 1 */

  g_assert_cmpuint(stk_chart_geom_time_ticks(STK_RANGE_1D, e, s, 5, out), ==, 0);
  g_assert_cmpuint(stk_chart_geom_time_ticks(STK_RANGE_1D, s, s, 5, out), ==, 0);
  g_assert_cmpuint(stk_chart_geom_time_ticks(STK_RANGE_1D, s, e, 0, out), ==, 0);
  /* a 10-minute domain without a whole hour inside */
  g_assert_cmpuint(stk_chart_geom_time_ticks(STK_RANGE_1D, s, s + 600000, 5, out), ==, 0);
  /* a 200-year domain terminates quickly and fits nothing hourly... */
  stk_chart_geom_time_ticks(STK_RANGE_1D, 0, utc_ms(2200, 1, 1, 0, 0), 5, out);
}

static void
test_time_ticks_days(void)
{
  gint64 out[MAX_TICKS];
  gint64 t0 = utc_ms(2026, 10, 10, 0, 0), t1 = utc_ms(2026, 11, 9, 0, 0);
  StkRange ranges[3] = { STK_RANGE_1M, STK_RANGE_3M, STK_RANGE_6M };
  for (int r = 0; r < 3; r++) {
    gint64 end = r == 0 ? t1 : r == 1 ? utc_ms(2027, 1, 10, 0, 0) : utc_ms(2027, 4, 10, 0, 0);
    for (gsize max = 1; max <= 6; max++) {
      gsize n = stk_chart_geom_time_ticks(ranges[r], t0, end, max, out);
      g_assert_cmpuint(n, <=, max);
      g_assert_cmpuint(n, >=, 1);
      for (gsize i = 0; i < n; i++) {
        g_assert_cmpint(out[i], >=, t0);
        g_assert_cmpint(out[i], <=, end);
        g_assert_cmpint(out[i] % (86400 * 1000), ==, 0); /* UTC midnight, DST-independent */
        if (i > 0)
          g_assert_cmpint(out[i], >, out[i - 1]);
      }
    }
  }
  /* 1M with 5 labels: weekly */
  gsize n = stk_chart_geom_time_ticks(STK_RANGE_1M, t0, t1, 5, out);
  g_assert_cmpuint(n, ==, 5);
  char label[STK_CHART_GEOM_LABEL_LEN];
  stk_chart_geom_format_tick(STK_RANGE_1M, out[0], label, sizeof label);
  g_assert_cmpstr(label, ==, "10 Oct"); /* t0 is exactly Oct 10 00:00Z: inclusive */
}

static void
test_time_ticks_months(void)
{
  gint64 out[MAX_TICKS];
  char label[STK_CHART_GEOM_LABEL_LEN];
  gint64 t0 = utc_ms(2025, 10, 8, 4, 0), t1 = utc_ms(2026, 10, 8, 4, 0);
  gsize n = stk_chart_geom_time_ticks(STK_RANGE_1Y, t0, t1, 6, out);
  g_assert_cmpuint(n, ==, 6); /* every 2 months: Nov, Jan, Mar, May, Jul, Sep */
  const char *want[6] = { "Nov", "Jan", "Mar", "May", "Jul", "Sep" };
  for (gsize i = 0; i < n; i++) {
    stk_chart_geom_format_tick(STK_RANGE_1Y, out[i], label, sizeof label);
    g_assert_cmpstr(label, ==, want[i]);
    g_assert_cmpint(out[i] % (86400 * 1000), ==, 0); /* 00:00 UTC */
    g_autoptr(GDateTime) dt = g_date_time_new_from_unix_utc(out[i] / 1000);
    g_assert_cmpint(g_date_time_get_day_of_month(dt), ==, 1);
  }
  n = stk_chart_geom_time_ticks(STK_RANGE_1Y, t0, t1, 2, out);
  g_assert_cmpuint(n, ==, 2);
  g_assert_cmpuint(stk_chart_geom_time_ticks(STK_RANGE_1Y, t1, t0, 6, out), ==, 0);
}

/* ---- nearest ------------------------------------------------------- */

static void
test_nearest(void)
{
  gsize i = 99;
  double t[5] = { 10, 20, 30, 50, 100 };
  g_assert_false(stk_chart_geom_nearest(NULL, 0, 5, &i));
  g_assert_cmpuint(i, ==, 99);
  g_assert_false(stk_chart_geom_nearest(t, 5, NAN, &i));
  g_assert_cmpuint(i, ==, 99);

  g_assert_true(stk_chart_geom_nearest(t, 5, -1000, &i));
  g_assert_cmpuint(i, ==, 0);
  g_assert_true(stk_chart_geom_nearest(t, 5, 10, &i));
  g_assert_cmpuint(i, ==, 0);
  g_assert_true(stk_chart_geom_nearest(t, 5, 100, &i));
  g_assert_cmpuint(i, ==, 4);
  g_assert_true(stk_chart_geom_nearest(t, 5, 1e9, &i));
  g_assert_cmpuint(i, ==, 4);
  g_assert_true(stk_chart_geom_nearest(t, 5, 24, &i));
  g_assert_cmpuint(i, ==, 1);
  g_assert_true(stk_chart_geom_nearest(t, 5, 26, &i));
  g_assert_cmpuint(i, ==, 2);
  g_assert_true(stk_chart_geom_nearest(t, 5, 25, &i)); /* tie -> earlier */
  g_assert_cmpuint(i, ==, 1);
  g_assert_true(stk_chart_geom_nearest(t, 5, 76, &i));
  g_assert_cmpuint(i, ==, 4);

  double one[1] = { 42 };
  g_assert_true(stk_chart_geom_nearest(one, 1, 0, &i));
  g_assert_cmpuint(i, ==, 0);
  g_assert_true(stk_chart_geom_nearest(one, 1, 99, &i));
  g_assert_cmpuint(i, ==, 0);

  /* every element is its own nearest */
  for (gsize k = 0; k < 5; k++) {
    g_assert_true(stk_chart_geom_nearest(t, 5, t[k], &i));
    g_assert_cmpuint(i, ==, k);
  }
}

/* ---- formatting ---------------------------------------------------- */

static void
check_price(double v, const char *want)
{
  char buf[STK_CHART_GEOM_LABEL_LEN];
  stk_chart_geom_format_price(v, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, want);
}

static void
test_format_price(void)
{
  check_price(0.0, "0.00");
  check_price(5.0, "5.00");
  check_price(12.345, "12.35"); /* 12.345 is 12.3450000000000006 */
  check_price(999.0, "999.00");
  check_price(1000.0, "1,000.00");
  check_price(1234.567, "1,234.57");
  check_price(12345.6, "12,345.60");
  check_price(123456.0, "123,456.00");
  check_price(1234567.891, "1,234,567.89");
  check_price(-5.5, "-5.50");
  check_price(-1234567.891, "-1,234,567.89");
  check_price(-0.001, "0.00");
  check_price(-0.0, "0.00");
  check_price(0.004, "0.00");
  check_price(NAN, "\xe2\x80\x94");
  check_price(INFINITY, "\xe2\x80\x94");
  check_price(1e12, "\xe2\x80\x94");
  check_price(999999999999.0, "999,999,999,999.00");

  char small[5];
  stk_chart_geom_format_price(1234.56, small, sizeof small);
  g_assert_cmpstr(small, ==, "1,23");
  char none[1] = { 'x' };
  stk_chart_geom_format_price(1.0, none, 0);
  g_assert_cmpint(none[0], ==, 'x');

  /* locale independent */
  if (setlocale(LC_ALL, "de_DE.UTF-8") != NULL || setlocale(LC_ALL, "de_DE") != NULL)
    check_price(1234.5, "1,234.50");
  setlocale(LC_ALL, "C");
}

static void
test_format_time(void)
{
  char buf[STK_CHART_GEOM_LABEL_LEN];
  /* 2026-10-08 14:35Z = 10:35 AM EDT */
  gint64 ms = utc_ms(2026, 10, 8, 14, 35);
  stk_chart_geom_format_tick(STK_RANGE_1D, ms, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "10:35 AM");
  stk_chart_geom_format_tick(STK_RANGE_1D, utc_ms(2026, 10, 8, 13, 30), buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "9:30 AM");
  stk_chart_geom_format_tick(STK_RANGE_1D, utc_ms(2026, 10, 8, 16, 0), buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "12 PM");
  stk_chart_geom_format_tick(STK_RANGE_1D, utc_ms(2026, 10, 9, 4, 0), buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "12 AM");
  /* winter: EST = UTC-5; 2026-12-01 21:00Z = 4 PM */
  stk_chart_geom_format_tick(STK_RANGE_1D, utc_ms(2026, 12, 1, 21, 0), buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "4 PM");

  stk_chart_geom_format_tick(STK_RANGE_1M, ms, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "8 Oct");
  stk_chart_geom_format_tick(STK_RANGE_3M, ms, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "8 Oct");
  stk_chart_geom_format_tick(STK_RANGE_6M, ms, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "8 Oct");
  stk_chart_geom_format_tick(STK_RANGE_1Y, ms, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "Oct");
  /* daily ranges use the UTC date (00:00Z stamps), never New York time */
  stk_chart_geom_format_tick(STK_RANGE_1M, utc_ms(2026, 1, 1, 3, 0), buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "1 Jan");

  stk_chart_geom_format_hover(STK_RANGE_1D, ms, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "Oct 8, 10:35 AM");
  stk_chart_geom_format_hover(STK_RANGE_1D, utc_ms(2026, 10, 8, 16, 0), buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "Oct 8, 12:00 PM");
  stk_chart_geom_format_hover(STK_RANGE_1M, ms, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "Oct 8, 2026");
  stk_chart_geom_format_hover(STK_RANGE_1Y, ms, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "Oct 8, 2026");

  /* out of GDateTime's range -> em dash, extremes do not overflow */
  gint64 extremes[3] = { G_MAXINT64, G_MININT64, -62135596800001 };
  for (int i = 0; i < 3; i++) {
    stk_chart_geom_format_tick(STK_RANGE_1D, extremes[i], buf, sizeof buf);
    g_assert_cmpstr(buf, ==, "\xe2\x80\x94");
    stk_chart_geom_format_hover(STK_RANGE_1D, extremes[i], buf, sizeof buf);
    g_assert_cmpstr(buf, ==, "\xe2\x80\x94");
  }

  /* truncation stays terminated; len 0 writes nothing */
  char small[4];
  stk_chart_geom_format_hover(STK_RANGE_1M, ms, small, sizeof small);
  g_assert_cmpstr(small, ==, "Oct");
  char none[1] = { 'x' };
  stk_chart_geom_format_tick(STK_RANGE_1M, ms, none, 0);
  stk_chart_geom_format_hover(STK_RANGE_1M, ms, none, 0);
  g_assert_cmpint(none[0], ==, 'x');
}

/* ---- daily timestamps are 00:00 UTC of the trading date ------------- */

static void
test_daily_utc_stamps(void)
{
  char buf[STK_CHART_GEOM_LABEL_LEN];
  /* Provider encoding: 2026-10-07 bar = 2026-10-07T00:00:00Z, which is
   * Oct 6, 8 PM in New York. A daily label must show Oct 7. */
  gint64 d = utc_ms(2026, 10, 7, 0, 0);
  StkRange daily[4] = { STK_RANGE_1M, STK_RANGE_3M, STK_RANGE_6M, STK_RANGE_1Y };
  for (int r = 0; r < 4; r++) {
    stk_chart_geom_format_hover(daily[r], d, buf, sizeof buf);
    g_assert_cmpstr(buf, ==, "Oct 7, 2026");
  }
  stk_chart_geom_format_tick(STK_RANGE_1M, d, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "7 Oct");
  stk_chart_geom_format_tick(STK_RANGE_6M, d, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "7 Oct");
  stk_chart_geom_format_tick(STK_RANGE_1Y, d, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "Oct");
  /* 1D keeps true-instant semantics: the same instant is the previous NY evening. */
  stk_chart_geom_format_hover(STK_RANGE_1D, d, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "Oct 6, 8:00 PM");

  /* year/month boundaries: Jan 2 and the first trading day of a year */
  stk_chart_geom_format_hover(STK_RANGE_1Y, utc_ms(2026, 1, 2, 0, 0), buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "Jan 2, 2026");
  stk_chart_geom_format_tick(STK_RANGE_1Y, utc_ms(2026, 1, 1, 0, 0), buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "Jan");
  /* a close-time (16:00Z..23:59Z) stamp of the same date shows the same date */
  stk_chart_geom_format_hover(STK_RANGE_1M, utc_ms(2026, 10, 7, 23, 59), buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "Oct 7, 2026");

  /* UTC-midnight ticks over a 1M domain of provider stamps */
  gint64 out[MAX_TICKS];
  gint64 t0 = utc_ms(2026, 9, 8, 0, 0), t1 = utc_ms(2026, 10, 7, 0, 0);
  gsize n = stk_chart_geom_time_ticks(STK_RANGE_1M, t0, t1, 5, out);
  g_assert_cmpuint(n, ==, 5); /* weekly: Sep 8, 15, 22, 29, Oct 6 */
  g_assert_cmpint(out[0], ==, t0);
  for (gsize i = 0; i < n; i++) {
    g_assert_cmpint(out[i] % (86400 * 1000), ==, 0);
    if (i > 0)
      g_assert_cmpint(out[i] - out[i - 1], ==, 7 * 86400 * 1000);
  }
  stk_chart_geom_format_tick(STK_RANGE_1M, out[0], buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "8 Sep");

  /* 1Y: first of the month 00:00Z */
  n = stk_chart_geom_time_ticks(STK_RANGE_1Y, utc_ms(2025, 10, 7, 0, 0), utc_ms(2026, 10, 7, 0, 0),
                                6, out);
  g_assert_cmpuint(n, ==, 6);
  g_assert_cmpint(out[0], ==, utc_ms(2025, 11, 1, 0, 0));
  g_assert_cmpint(out[1], ==, utc_ms(2026, 1, 1, 0, 0));
}

/* ---- 1D hour ticks stay wall-clock aligned across DST ---------------- */

static void
check_hours(const gint64 *out, gsize n, const int *want)
{
  for (gsize i = 0; i < n; i++)
    g_assert_cmpint(ny_hour(out[i]), ==, want[i] * 100);
}

static void
test_time_ticks_1d_dst(void)
{
  gint64 out[MAX_TICKS];
  /* Spring forward 2026-03-08: 00:00 EST (05:00Z) .. 12:00 EDT (16:00Z). */
  gint64 s = utc_ms(2026, 3, 8, 5, 0), e = utc_ms(2026, 3, 8, 16, 0);
  gsize n = stk_chart_geom_time_ticks(STK_RANGE_1D, s, e, 4, out);
  const int spring4[4] = { 0, 4, 8, 12 };
  g_assert_cmpuint(n, ==, 4);
  check_hours(out, n, spring4);
  /* step 2: the non-existent 02:00 gets no tick, the rest stay even hours */
  n = stk_chart_geom_time_ticks(STK_RANGE_1D, s, e, 8, out);
  const int spring8[6] = { 0, 4, 6, 8, 10, 12 };
  g_assert_cmpuint(n, ==, 6);
  check_hours(out, n, spring8);

  /* Fall back 2026-11-01: 00:00 EDT (04:00Z) .. 12:00 EST (17:00Z). */
  s = utc_ms(2026, 11, 1, 4, 0);
  e = utc_ms(2026, 11, 1, 17, 0);
  n = stk_chart_geom_time_ticks(STK_RANGE_1D, s, e, 4, out);
  const int fall4[4] = { 0, 4, 8, 12 };
  g_assert_cmpuint(n, ==, 4);
  check_hours(out, n, fall4);
  n = stk_chart_geom_time_ticks(STK_RANGE_1D, s, e, 7, out);
  const int fall7[7] = { 0, 2, 4, 6, 8, 10, 12 };
  g_assert_cmpuint(n, ==, 7);
  check_hours(out, n, fall7);

  /* Every tick is a whole local hour, ascending, within the cap. */
  gint64 starts[2] = { utc_ms(2026, 3, 8, 5, 0), utc_ms(2026, 11, 1, 4, 0) };
  for (int d = 0; d < 2; d++) {
    for (gsize max = 1; max <= MAX_TICKS; max++) {
      n = stk_chart_geom_time_ticks(STK_RANGE_1D, starts[d], starts[d] + 13 * 3600 * 1000, max,
                                    out);
      g_assert_cmpuint(n, <=, max);
      for (gsize i = 0; i < n; i++) {
        g_assert_cmpint(ny_hour(out[i]) % 100, ==, 0);
        if (i > 0)
          g_assert_cmpint(out[i], >, out[i - 1]);
      }
    }
  }
}

/* ---- min_step coverage bound ----------------------------------------- */

static void
test_nice_ticks_min_step(void)
{
  double out[MAX_TICKS];
  /* Not a 1-2-5 min_step above span/2.5: the next nice step (5) exceeds the
   * span and no multiple lies inside, so 0 ticks is a legitimate answer. */
  g_assert_cmpuint(stk_chart_geom_nice_ticks(0.4, 4.6, 3, 2.1, out), ==, 0);
  g_assert_cmpuint(stk_chart_geom_nice_ticks(0.4, 4.6, 8, 2.1, out), ==, 0);
  /* At the documented bound min_step == span / 2.5 a tick is found. */
  g_assert_cmpuint(stk_chart_geom_nice_ticks(0.4, 4.6, 3, 4.2 / 2.5, out), >=, 1);
  /* Span overflow is rejected, not turned into NaN ticks. */
  g_assert_cmpuint(stk_chart_geom_nice_ticks(-1e308, 1e308, 5, 0, out), ==, 0);

  GRand *rnd = g_rand_new_with_seed(11);
  for (int iter = 0; iter < 5000; iter++) {
    double span = pow(10.0, g_rand_double_range(rnd, -3, 6));
    double lo = g_rand_double_range(rnd, -2 * span, 2 * span);
    double min_step = span / 2.5 * g_rand_double_range(rnd, 0.001, 1.0);
    gsize max = (gsize)g_rand_int_range(rnd, 3, 9);
    g_assert_cmpuint(stk_chart_geom_nice_ticks(lo, lo + span, max, min_step, out), >=, 1);
  }
  g_rand_free(rnd);
}

/* Header guarantee far from zero (|lo|/span up to ~1e8+) and min_step far
 * above the span. */
static void
test_nice_ticks_far_from_zero(void)
{
  double out[MAX_TICKS];
  g_assert_cmpuint(stk_chart_geom_nice_ticks(7806275.5100000007, 7806275.7999999998, 5, 0.01, out),
                   >=, 1);
  g_assert_cmpuint(stk_chart_geom_nice_ticks(57761203596067.008, 57761203596067.477, 5,
                                             0.15306690757072991, out),
                   >=, 1);
  g_assert_cmpuint(stk_chart_geom_nice_ticks(204945288323.52002, 204945288323.53308, 3, 0, out),
                   >=, 1);
  g_assert_cmpuint(stk_chart_geom_nice_ticks(-1, 1, 5, 1e50, out), ==, 1);
  g_assert_cmpuint(stk_chart_geom_nice_ticks(-1.16e-45, 1.16e-45, 5, 0.01, out), ==, 1);

  GRand *rnd = g_rand_new_with_seed(5);
  for (int iter = 0; iter < 20000; iter++) {
    double span = pow(10.0, g_rand_double_range(rnd, -3, 3));
    double lo = g_rand_double_range(rnd, -1, 1) * 4503599627370496.0 * span / 10.0 *
                pow(10.0, g_rand_double_range(rnd, -4, 0));
    double hi = lo + span;
    if (!(hi - lo >= span * 0.99))
      continue;
    double min_step = (hi - lo) / 2.5 * g_rand_double_range(rnd, 0.0, 1.0);
    gsize max = (gsize)g_rand_int_range(rnd, 3, 9);
    gsize n = stk_chart_geom_nice_ticks(lo, hi, max, min_step, out);
    g_assert_cmpuint(n, >=, 1);
    g_assert_cmpuint(n, <=, max);
    for (gsize k = 0; k < n; k++) {
      g_assert_cmpfloat(out[k], >=, lo - 1e-9 * (hi - lo));
      g_assert_cmpfloat(out[k], <=, hi + 1e-9 * (hi - lo));
    }
  }
  g_rand_free(rnd);
}

/* ---- y_range overflow ------------------------------------------------- */

static void
test_y_range_overflow(void)
{
  double lo = 42.0, hi = 43.0;
  double wide[2] = { -1e308, 1e308 };
  g_assert_false(stk_chart_geom_y_range(wide, 2, NAN, 0.0, &lo, &hi));
  g_assert_false(stk_chart_geom_y_range(wide, 2, NAN, 0.08, &lo, &hi));
  double half[2] = { 0.0, 1.7e308 };
  g_assert_false(stk_chart_geom_y_range(half, 2, NAN, 0.08, &lo, &hi));
  double mx[1] = { G_MAXDOUBLE };
  g_assert_false(stk_chart_geom_y_range(mx, 1, NAN, 0.08, &lo, &hi));
  double fl[1] = { -G_MAXDOUBLE };
  g_assert_false(stk_chart_geom_y_range(fl, 1, NAN, 0.08, &lo, &hi));
  g_assert_cmpfloat(lo, ==, 42.0); /* outputs untouched on FALSE */
  g_assert_cmpfloat(hi, ==, 43.0);

  /* Large but representable spans still work and stay finite and ordered. */
  double big[2] = { -4e307, 4e307 };
  g_assert_true(stk_chart_geom_y_range(big, 2, NAN, 0.08, &lo, &hi));
  g_assert_true(isfinite(lo) && isfinite(hi) && isfinite(hi - lo));
  g_assert_cmpfloat(hi, >, lo);
}

/* ---- bounded sampling -------------------------------------------------- */

static void
test_sample_small(void)
{
  double t[8], v[8], t0 = -1, t1 = -1;
  g_assert_cmpuint(stk_chart_geom_sample(STK_RANGE_1M, NULL, 0, 8, t, v, &t0, &t1), ==, 0);
  StkPoint one[1] = { { utc_ms(2026, 10, 7, 0, 0), 5.0 } };
  g_assert_cmpuint(stk_chart_geom_sample(STK_RANGE_1M, one, 1, 3, t, v, &t0, &t1), ==, 0); /* cap<4 */
  g_assert_cmpuint(stk_chart_geom_sample(STK_RANGE_1M, one, 1, 8, t, v, &t0, &t1), ==, 1);
  g_assert_cmpfloat(t0, ==, t1);
  g_assert_cmpfloat(v[0], ==, 5.0);

  StkPoint few[3] = { { 1000, 1.0 }, { 2000, 3.0 }, { 3000, 2.0 } };
  g_assert_cmpuint(stk_chart_geom_sample(STK_RANGE_3M, few, 3, 8, t, v, &t0, &t1), ==, 3);
  g_assert_cmpfloat(t0, ==, 1000.0);
  g_assert_cmpfloat(t1, ==, 3000.0);
  g_assert_cmpfloat(v[1], ==, 3.0);

  /* 1D: samples outside the regular session of the last sample's date are
   * dropped (09:30-16:00 EDT = 13:30Z-20:00Z). */
  StkPoint day[4] = { { utc_ms(2026, 10, 8, 12, 0), 1.0 },
                      { utc_ms(2026, 10, 8, 14, 0), 2.0 },
                      { utc_ms(2026, 10, 8, 15, 0), 3.0 },
                      { utc_ms(2026, 10, 8, 21, 0), 4.0 } };
  g_assert_cmpuint(stk_chart_geom_sample(STK_RANGE_1D, day, 4, 8, t, v, &t0, &t1), ==, 2);
  g_assert_cmpfloat(t0, ==, (double)utc_ms(2026, 10, 8, 13, 30));
  g_assert_cmpfloat(v[0], ==, 2.0);
  /* everything outside the session */
  g_assert_cmpuint(stk_chart_geom_sample(STK_RANGE_1D, day + 3, 1, 8, t, v, &t0, &t1), ==, 0);
}

static void
test_sample_large(void)
{
  /* A series far longer than the cap is reduced without a full-size
   * temporary: output <= max_out, ascending, first/last and extremes kept. */
  const gsize n = 1000000, max_out = 64;
  StkPoint *pts = g_new(StkPoint, n);
  gint64 base = utc_ms(2020, 1, 1, 0, 0);
  for (gsize i = 0; i < n; i++) {
    pts[i].t_ms = base + (gint64)i * 60000;
    pts[i].v = 100.0 + 10.0 * sin((double)i / 5000.0);
  }
  pts[123457].v = -50.0;
  pts[777777].v = 500.0;

  double t[64], v[64], t0, t1;
  gsize k = stk_chart_geom_sample(STK_RANGE_1Y, pts, n, max_out, t, v, &t0, &t1);
  g_assert_cmpuint(k, >, 2);
  g_assert_cmpuint(k, <=, max_out);
  g_assert_cmpfloat(t[0], ==, (double)pts[0].t_ms);
  g_assert_cmpfloat(t[k - 1], ==, (double)pts[n - 1].t_ms);
  g_assert_cmpfloat(t0, ==, t[0]);
  g_assert_cmpfloat(t1, ==, t[k - 1]);
  double mn = INFINITY, mxv = -INFINITY;
  for (gsize i = 0; i < k; i++) {
    if (i > 0)
      g_assert_cmpfloat(t[i], >, t[i - 1]);
    mn = fmin(mn, v[i]);
    mxv = fmax(mxv, v[i]);
  }
  g_assert_cmpfloat(mn, ==, -50.0);
  g_assert_cmpfloat(mxv, ==, 500.0);
  g_free(pts);
}

/* ---- r3 regressions --------------------------------------------------- */

/* nice_ticks must be safe for ALL inputs: bounded writes (canary after the
 * buffer), exact multiples, ascending, inside the range. */
static void
check_nice(double lo, double hi, gsize max, double min_step)
{
  struct {
    double out[STK_CHART_GEOM_MAX_TICKS];
    double canary[4];
  } b;
  for (int i = 0; i < 4; i++)
    b.canary[i] = -12345.0;
  for (gsize i = 0; i < STK_CHART_GEOM_MAX_TICKS; i++)
    b.out[i] = 777.0;
  gsize n = stk_chart_geom_nice_ticks(lo, hi, max, min_step, b.out);
  for (int i = 0; i < 4; i++)
    g_assert_cmpfloat(b.canary[i], ==, -12345.0);
  g_assert_cmpuint(n, <=, MIN(max, (gsize)STK_CHART_GEOM_MAX_TICKS));
  if (n == 0) {
    for (gsize i = 0; i < STK_CHART_GEOM_MAX_TICKS; i++)
      g_assert_cmpfloat(b.out[i], ==, 777.0);
    return;
  }
  double slack = 1e-9 * (hi - lo);
  for (gsize i = 0; i < n; i++) {
    g_assert_true(isfinite(b.out[i]));
    g_assert_cmpfloat(b.out[i], >=, lo - slack);
    g_assert_cmpfloat(b.out[i], <=, hi + slack);
    if (i > 0)
      g_assert_cmpfloat(b.out[i], >, b.out[i - 1]);
  }
  if (n >= 2) {
    /* spacing is a 1-2-5 step: equal gaps within rounding */
    double gap = b.out[1] - b.out[0];
    double eps = 8.0 * DBL_EPSILON * MAX(fabs(b.out[0]), fabs(b.out[n - 1])); /* subtraction error */
    for (gsize i = 2; i < n; i++)
      g_assert_cmpfloat(fabs((b.out[i] - b.out[i - 1]) - gap), <=, 1e-6 * gap + eps);
  }
  if (min_step > 0 && n >= 2)
    g_assert_cmpfloat(b.out[1] - b.out[0], >=, min_step * (1 - 1e-9) - 8.0 * DBL_EPSILON * fabs(b.out[1]));
}

static void
test_nice_ticks_extreme(void)
{
  /* P1-001: subnormal spans with min_step <= 0 overflowed the buffer. */
  const double steps[] = { 0.0, -1.0, NAN, 1e-300, 1.0, INFINITY };
  const double spans[] = { 5e-324, 1e-323, 4e-323, 1e-322, 1e-320, 1e-310, 2.3e-308, 1e-300 };
  for (gsize s = 0; s < G_N_ELEMENTS(spans); s++)
    for (gsize m = 0; m < G_N_ELEMENTS(steps); m++)
      for (gsize mx = 0; mx <= 10; mx++) {
        check_nice(0.0, spans[s], mx, steps[m]);
        check_nice(-spans[s], spans[s], mx, steps[m]);
        check_nice(1.0, 1.0 + spans[s], mx, steps[m]);
      }
  double out[STK_CHART_GEOM_MAX_TICKS];
  g_assert_cmpuint(stk_chart_geom_nice_ticks(0.0, 5e-324, 5, 0.0, out), ==, 0);
  g_assert_cmpuint(stk_chart_geom_nice_ticks(0.0, 1e-323, 5, -1.0, out), ==, 0);

  /* P1-002: huge offsets relative to the span give exact ticks or none. */
  /* old code returned 5 ticks with duplicates; exact ones are 1e15, 1e15+0.5 */
  g_assert_cmpuint(stk_chart_geom_nice_ticks(1e15, 1e15 + 0.5, 8, 0.0, out), ==, 2);
  g_assert_cmpfloat(out[0], ==, 1e15);
  g_assert_cmpfloat(out[1], ==, 1e15 + 0.5);
  check_nice(1e15, 1e15 + 0.5, 8, 0.0);
  check_nice(-5.1586423358677659e23, -5.1586423358677652e23, 6, 0.0);
  for (double off = 1.0; off < 1e300; off *= 7.0) {
    for (double span = 1e-5; span < 1e6; span *= 13.0) {
      for (gsize mx = 1; mx <= 8; mx++) {
        check_nice(off, off + span, mx, 0.0);
        check_nice(-off, -off + span, mx, 0.01);
      }
    }
  }
  /* 1e19 * span offsets */
  check_nice(1e19, 1e19 + 1.0, 8, 0.0);
  check_nice(1e19 * 3e-7, 1e19 * 3e-7 + 3e-7, 8, 0.0);

  /* non-finite / degenerate */
  const double bad[] = { NAN, INFINITY, -INFINITY, 0.0, 1.0, -1.0, DBL_MAX, -DBL_MAX, DBL_MIN };
  for (gsize a = 0; a < G_N_ELEMENTS(bad); a++)
    for (gsize b = 0; b < G_N_ELEMENTS(bad); b++)
      for (gsize mx = 0; mx <= 10; mx += 5)
        check_nice(bad[a], bad[b], mx, 0.0);
  g_assert_cmpuint(stk_chart_geom_nice_ticks(-DBL_MAX, DBL_MAX, 5, 0.0, out), ==, 0);

  /* normal inputs still work */
  g_assert_cmpuint(stk_chart_geom_nice_ticks(0.0, 10.0, 5, 0.0, out), ==, 3);
  g_assert_cmpfloat(out[1], ==, 5.0);
}

static void
test_nice_ticks_fuzz(void)
{
  GRand *r = g_rand_new_with_seed(12345);
  for (int i = 0; i < 200000; i++) {
    double mag = pow(10.0, g_rand_double_range(r, -330.0, 310.0));
    double span = mag * g_rand_double_range(r, 0.5, 10.0);
    double off = pow(10.0, g_rand_double_range(r, -330.0, 310.0)) * (g_rand_boolean(r) ? 1 : -1);
    if (g_rand_int_range(r, 0, 4) == 0)
      off *= span;
    double ms = g_rand_boolean(r) ? 0.0 : pow(10.0, g_rand_double_range(r, -330.0, 310.0));
    check_nice(off, off + span, (gsize)g_rand_int_range(r, 0, 12), ms);
  }
  g_rand_free(r);
}

/* P1-003: the first (EDT) occurrence of the repeated 1 AM on 2026-11-01. */
static void
test_time_ticks_fall_back_first_hour(void)
{
  gint64 out[MAX_TICKS];
  gint64 edt1 = utc_ms(2026, 11, 1, 5, 0), est1 = utc_ms(2026, 11, 1, 6, 0);

  /* domain holds only the first 1:00 AM (00:30 EDT .. 01:30 EDT) */
  gsize n = stk_chart_geom_time_ticks(STK_RANGE_1D, utc_ms(2026, 11, 1, 4, 30),
                                      utc_ms(2026, 11, 1, 5, 30), 8, out);
  g_assert_cmpuint(n, ==, 1);
  g_assert_cmpint(out[0], ==, edt1);

  /* domain holds only the second 1:00 AM (01:30 EDT+1h .. 02:30 EST) */
  n = stk_chart_geom_time_ticks(STK_RANGE_1D, utc_ms(2026, 11, 1, 5, 30),
                                utc_ms(2026, 11, 1, 7, 30), 8, out);
  g_assert_cmpuint(n, ==, 2); /* 1:00 EST and 2:00 EST */
  g_assert_cmpint(out[0], ==, est1);

  /* domain holds both: one tick, the earliest instant */
  n = stk_chart_geom_time_ticks(STK_RANGE_1D, utc_ms(2026, 11, 1, 4, 30),
                                utc_ms(2026, 11, 1, 6, 30), 8, out);
  g_assert_cmpuint(n, ==, 1);
  g_assert_cmpint(out[0], ==, edt1);
  n = stk_chart_geom_time_ticks(STK_RANGE_1D, utc_ms(2026, 11, 1, 4, 30),
                                utc_ms(2026, 11, 1, 7, 30), 8, out);
  g_assert_cmpuint(n, ==, 2); /* 1 AM (EDT), 2 AM (EST); never 1 AM EST */
  g_assert_cmpint(out[0], ==, edt1);
  g_assert_cmpint(out[1], ==, utc_ms(2026, 11, 1, 7, 0));

  /* the reported counterexample: 21:17 EDT .. 01:42 EDT with max 3 */
  n = stk_chart_geom_time_ticks(STK_RANGE_1D, 1793495820000, 1793511720000, 3, out);
  g_assert_cmpuint(n, ==, 2); /* step 2 h: 10 PM, 12 AM... */
  n = stk_chart_geom_time_ticks(STK_RANGE_1D, 1793495820000, 1793511720000, 4, out);
  g_assert_cmpuint(n, ==, 4); /* 10 PM, 11 PM, 12 AM, 1 AM(EDT) */
  g_assert_cmpint(out[3], ==, edt1);

  n = stk_chart_geom_time_ticks(STK_RANGE_1D, 1793507400000, 1793511000000, 8, out);
  g_assert_cmpuint(n, ==, 1);
  g_assert_cmpint(out[0], ==, 1793509200000);
}

/* P1-004 */
static void
test_format_year_9999_boundary(void)
{
  char buf[STK_CHART_GEOM_LABEL_LEN];
  const gint64 last = G_GINT64_CONSTANT(253402300799999); /* 9999-12-31T23:59:59.999Z */
  stk_chart_geom_format_hover(STK_RANGE_1M, last, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "Dec 31, 9999");
  stk_chart_geom_format_hover(STK_RANGE_1M, last - 499, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "Dec 31, 9999");
  stk_chart_geom_format_tick(STK_RANGE_1Y, G_GINT64_CONSTANT(253402300799500), buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "Dec");
  stk_chart_geom_format_tick(STK_RANGE_1Y, last + 1, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "\xe2\x80\x94");
  stk_chart_geom_format_tick(STK_RANGE_1Y, G_MAXINT64, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "\xe2\x80\x94");
  stk_chart_geom_format_tick(STK_RANGE_1Y, G_MININT64, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "\xe2\x80\x94");
  /* lower bound: 0001-01-01T00:00:00.000Z is valid, 1 ms earlier is not */
  const gint64 first = G_GINT64_CONSTANT(-62135596800000);
  stk_chart_geom_format_hover(STK_RANGE_1M, first, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "Jan 1, 1");
  stk_chart_geom_format_hover(STK_RANGE_1M, first - 1, buf, sizeof buf);
  g_assert_cmpstr(buf, ==, "\xe2\x80\x94");
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/chart-geom/is-up", test_is_up);
  g_test_add_func("/chart-geom/domain", test_domain);
  g_test_add_func("/chart-geom/ticks/nice-extreme", test_nice_ticks_extreme);
  g_test_add_func("/chart-geom/ticks/nice-fuzz", test_nice_ticks_fuzz);
  g_test_add_func("/chart-geom/ticks/time-fall-back-first-hour", test_time_ticks_fall_back_first_hour);
  g_test_add_func("/chart-geom/format/year-9999-boundary", test_format_year_9999_boundary);
  g_test_add_func("/chart-geom/y-range", test_y_range);
  g_test_add_func("/chart-geom/map", test_map);
  g_test_add_func("/chart-geom/decimate/trivial", test_decimate_trivial);
  g_test_add_func("/chart-geom/decimate/large", test_decimate_large);
  g_test_add_func("/chart-geom/decimate/outside-domain", test_decimate_window_domain);
  g_test_add_func("/chart-geom/ticks/nice-basic", test_nice_ticks_basic);
  g_test_add_func("/chart-geom/ticks/nice-property", test_nice_ticks_property);
  g_test_add_func("/chart-geom/ticks/time-1d", test_time_ticks_1d);
  g_test_add_func("/chart-geom/ticks/time-days", test_time_ticks_days);
  g_test_add_func("/chart-geom/ticks/time-months", test_time_ticks_months);
  g_test_add_func("/chart-geom/nearest", test_nearest);
  g_test_add_func("/chart-geom/format/price", test_format_price);
  g_test_add_func("/chart-geom/format/time", test_format_time);
  g_test_add_func("/chart-geom/format/daily-utc-stamps", test_daily_utc_stamps);
  g_test_add_func("/chart-geom/ticks/time-1d-dst", test_time_ticks_1d_dst);
  g_test_add_func("/chart-geom/ticks/nice-min-step", test_nice_ticks_min_step);
  g_test_add_func("/chart-geom/ticks/nice-far-from-zero", test_nice_ticks_far_from_zero);
  g_test_add_func("/chart-geom/y-range/overflow", test_y_range_overflow);
  g_test_add_func("/chart-geom/sample/small", test_sample_small);
  g_test_add_func("/chart-geom/sample/large", test_sample_large);
  return g_test_run();
}
