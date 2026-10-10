/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-chart-geom.c - pure chart geometry and label formatting (no GTK).
 * See stk-chart-geom.h for the contracts.
 */
#include "stk-chart-geom.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "stk-market.h"

#define EM_DASH "\xe2\x80\x94"

/* ------------------------------------------------------------------------ */
/* Time zone helpers                                                        */
/* ------------------------------------------------------------------------ */

/**
 * ny_tz:
 *
 * Shared immutable America/New_York zone (same fallback rule as stk-market),
 * created once on first use and never freed. If neither the zone name nor the
 * POSIX fallback rule can be created, NULL is remembered and returned; no
 * substitute zone such as UTC is used (it would shift every label by 4-5
 * hours).
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - returns the same value on every call. This replaces a former
 *    g_assert(tz != NULL): callers (zone_datetime(), hence every 1D label and
 *    tick function) now treat NULL as their normal failure value (em dash / 0
 *    ticks).
 * Thread affinity: any thread (one-time initialisation via g_once_init_enter).
 * Ownership: (transfer none) - owned by this file for the process lifetime;
 *   the caller must not unref it.
 *
 * Returns: (nullable): the shared zone, or NULL if it is unavailable.
 */
static GTimeZone *
ny_tz(void)
{
  static gsize tz_once = 0;
  if (g_once_init_enter(&tz_once)) {
    GTimeZone *tz = g_time_zone_new_identifier("America/New_York");
    if (tz == NULL)
      tz = g_time_zone_new_identifier("EST5EDT,M3.2.0,M11.1.0");
    /* 1 is the "unavailable" marker: g_once_init_leave() needs a non-zero value
     * and no valid (aligned) pointer equals 1. */
    g_once_init_leave(&tz_once, tz != NULL ? (gsize)tz : 1);
  }
  return tz_once == 1 ? NULL : (GTimeZone *)tz_once;
}

/* Unix seconds of 0001-01-01T00:00:00Z and 9999-12-31T23:59:59Z: GDateTime's
 * range. Milliseconds are floored to seconds BEFORE comparing, so every
 * instant up to 9999-12-31T23:59:59.999Z is valid. */
#define MIN_VALID_SECS G_GINT64_CONSTANT(-62135596800)
#define MAX_VALID_SECS G_GINT64_CONSTANT(253402300799)

/**
 * zone_datetime:
 * @tz: (nullable): zone to express the instant in.
 * @ms: instant, epoch ms.
 *
 * Local date-time of @ms in @tz, or NULL outside GDateTime's range.
 *
 * Preconditions:
 *  - none; any @ms is accepted (the floor to seconds cannot overflow).
 * Postconditions:
 *  - returns NULL when @tz is NULL (a NULL zone, i.e. an unavailable New York
 *    zone, was previously a g_assert() abort in ny_tz()), when @ms is outside
 *    the years 1..9999, or on a GLib failure. No state is changed.
 * Thread affinity: any thread (the shared zones are immutable).
 * Ownership: @tz is borrowed; the returned date-time is owned by the caller.
 *
 * Returns: (transfer full) (nullable): the local date-time; release with
 *   g_date_time_unref().
 */
static GDateTime *
zone_datetime(GTimeZone *tz, gint64 ms)
{
  if (tz == NULL)
    return NULL;
  gint64 secs = ms / 1000; /* floor, overflow-free for every gint64 */
  if (ms % 1000 < 0)
    secs--;
  if (secs < MIN_VALID_SECS || secs > MAX_VALID_SECS)
    return NULL;
  GDateTime *utc = g_date_time_new_from_unix_utc(secs);
  if (utc == NULL)
    return NULL;
  GDateTime *local = g_date_time_to_timezone(utc, tz);
  g_date_time_unref(utc);
  return local;
}

/**
 * utc_tz:
 *
 * Shared immutable UTC zone, created once on first use and never freed.
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - returns the same zone on every call (g_time_zone_new_utc() cannot fail).
 * Thread affinity: any thread (one-time initialisation via g_once_init_enter).
 * Ownership: (transfer none) - owned by this file for the process lifetime;
 *   the caller must not unref it.
 *
 * Returns: (not nullable): the shared UTC zone.
 */
static GTimeZone *
utc_tz(void)
{
  static gsize tz_once = 0;
  if (g_once_init_enter(&tz_once)) {
    GTimeZone *tz = g_time_zone_new_utc();
    g_once_init_leave(&tz_once, (gsize)tz);
  }
  return (GTimeZone *)tz_once;
}

/**
 * range_tz:
 * @range: the series range.
 *
 * The zone whose wall clock labels the samples of @range: New York for the
 * intraday range (true UTC instants), UTC for the daily ranges (their
 * timestamps are 00:00:00 UTC of the trading date, so the UTC date IS the
 * trading date).
 *
 * Preconditions:
 *  - none; any @range value is accepted (unknown values select UTC).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (the shared zones are immutable).
 * Ownership: (transfer none) - the zone is owned by this file; do not unref.
 *
 * Returns: (nullable): the shared zone; NULL only for STK_RANGE_1D when the
 *   New York zone is unavailable (see ny_tz()).
 */
static GTimeZone *
range_tz(StkRange range)
{
  return range == STK_RANGE_1D ? ny_tz() : utc_tz();
}

/**
 * datetime_ms:
 * @dt: (not nullable): date-time to convert.
 *
 * Whole Unix seconds of @dt times 1000.
 *
 * Preconditions:
 *  - @dt is non-NULL. Every GDateTime lies in the years 1..9999, so the
 *    product is within +-2.6e14 and cannot overflow gint64.
 * Postconditions:
 *  - @dt is not modified or unreferenced.
 * Thread affinity: any thread (pure).
 * Ownership: @dt is borrowed; nothing is retained.
 *
 * Returns: the instant in epoch milliseconds (sub-second part dropped).
 */
static gint64
datetime_ms(GDateTime *dt)
{
  return g_date_time_to_unix(dt) * 1000;
}

/* ------------------------------------------------------------------------ */
/* Series helpers                                                           */
/* ------------------------------------------------------------------------ */

/* Documented in stk-chart-geom.h. */
gboolean
stk_chart_geom_is_up(const double *v, gsize n)
{
  return n < 2 || v[n - 1] >= v[0];
}

/* Documented in stk-chart-geom.h. */
gboolean
stk_chart_geom_is_up_vs_close(const double *v, gsize n, double prev_close)
{
  if (n == 0)
    return TRUE;
  if (isfinite(prev_close))
    return v[n - 1] >= prev_close;
  return stk_chart_geom_is_up(v, n);
}

/* Documented in stk-chart-geom.h. */
gboolean
stk_chart_geom_domain(StkRange range, const double *t, gsize n, double *t0, double *t1)
{
  if (n == 0)
    return FALSE;

  if (range == STK_RANGE_1D) {
    GDate date;
    gint64 start_ms, end_ms;
    /* (gint64) of a NaN, an infinity or a value of magnitude >= 2^63 is
     * undefined: such an instant has no New York date, so it takes the
     * first..last path below. (double)G_MAXINT64 is 2^63 itself, hence the
     * strict bound. */
    double last_t = t[n - 1];
    if (fabs(last_t) < 9223372036854775808.0 && stk_market_local_date((gint64)last_t, &date) &&
        stk_market_session(&date, &start_ms, &end_ms)) {
      *t0 = (double)start_ms;
      *t1 = (double)end_ms;
      return TRUE;
    }
  }
  *t0 = t[0];
  *t1 = t[n - 1];
  return TRUE;
}

/**
 * sample_emit:
 * @w: (not nullable): the sampled slice.
 * @i: index into @w to append.
 * @max_out: capacity of @t and @v.
 * @count: (inout): number of samples already appended.
 * @last: (inout): slice index of the last appended sample; read only when
 *   *@count > 0.
 * @t: (out caller-allocates): receives the timestamp (as double).
 * @v: (out caller-allocates): receives the value.
 *
 * Appends index @i (relative to the sampled slice) unless it is not beyond the
 * last appended one; keeps the output ascending without duplicates.
 *
 * Preconditions:
 *  - @i is a valid index of @w (every call in stk_chart_geom_sample() passes a
 *    value below the window size); @t and @v hold @max_out entries; @count and
 *    @last are non-NULL.
 * Postconditions:
 *  - appends only when *@count < @max_out, so @t and @v are never written at
 *    an index >= @max_out; otherwise nothing changes.
 * Thread affinity: any thread (pure).
 * Ownership: @w is borrowed; @t, @v, @count and @last are caller-owned.
 *
 * Returns: void.
 */
static void
sample_emit(const StkPoint *w, gsize i, gsize max_out, gsize *count, gsize *last, double *t,
            double *v)
{
  if ((*count > 0 && i <= *last) || *count >= max_out)
    return;
  t[*count] = (double)w[i].t_ms;
  v[*count] = w[i].v;
  *last = i;
  (*count)++;
}

/* Documented in stk-chart-geom.h. */
gsize
stk_chart_geom_sample(StkRange range, const StkPoint *points, gsize n, gsize max_out, double *t,
                      double *v, double *t0, double *t1)
{
  if (n == 0 || points == NULL || max_out < 4)
    return 0;

  double ends[2] = { (double)points[0].t_ms, (double)points[n - 1].t_ms };
  double d0, d1;
  if (!stk_chart_geom_domain(range, ends, 2, &d0, &d1))
    return 0;

  /* Window [first, end): lower bound of d0, upper bound of d1. */
  gsize a = 0, b = n;
  while (a < b) {
    gsize mid = a + (b - a) / 2;
    if ((double)points[mid].t_ms < d0)
      a = mid + 1;
    else
      b = mid;
  }
  gsize first = a;
  b = n;
  while (a < b) {
    gsize mid = a + (b - a) / 2;
    if ((double)points[mid].t_ms <= d1)
      a = mid + 1;
    else
      b = mid;
  }
  gsize win_n = a - first;
  if (win_n == 0)
    return 0;
  const StkPoint *w = points + first;
  *t0 = d0;
  *t1 = d1;

  gsize count = 0, last = 0;
  if (win_n <= max_out) {
    for (gsize i = 0; i < win_n; i++)
      sample_emit(w, i, max_out, &count, &last, t, v);
    return count;
  }

  /* More samples than the cap: keep the first and last sample plus the
   * lowest and highest finite value of each of (max_out - 2) / 2 equal index
   * buckets, so the output has <= max_out samples and the extremes survive. */
  gsize buckets = (max_out - 2) / 2;
  sample_emit(w, 0, max_out, &count, &last, t, v);
  for (gsize k = 0; k < buckets; k++) {
    gsize s = (gsize)((guint64)win_n * k / buckets);
    gsize e = (gsize)((guint64)win_n * (k + 1) / buckets);
    gboolean any = FALSE;
    gsize imin = s, imax = s;
    for (gsize i = s; i < e; i++) {
      if (!isfinite(w[i].v))
        continue;
      if (!any || w[i].v < w[imin].v)
        imin = i;
      if (!any || w[i].v > w[imax].v)
        imax = i;
      any = TRUE;
    }
    if (!any)
      continue;
    sample_emit(w, MIN(imin, imax), max_out, &count, &last, t, v);
    sample_emit(w, MAX(imin, imax), max_out, &count, &last, t, v);
  }
  sample_emit(w, win_n - 1, max_out, &count, &last, t, v);
  return count;
}

/* Documented in stk-chart-geom.h. */
gboolean
stk_chart_geom_y_range(const double *v, gsize n, double include, double pad_frac, double *lo,
                       double *hi)
{
  double mn = INFINITY, mx = -INFINITY;
  for (gsize i = 0; i < n; i++) {
    if (!isfinite(v[i]))
      continue;
    mn = fmin(mn, v[i]);
    mx = fmax(mx, v[i]);
  }
  if (isfinite(include)) {
    mn = fmin(mn, include);
    mx = fmax(mx, include);
  }
  if (!isfinite(mn) || !isfinite(mx))
    return FALSE;

  double span = mx - mn;
  double pad;
  if (span > 0.0) {
    pad = (pad_frac > 0.0 ? pad_frac : 0.0) * span;
  } else {
    pad = fmax(0.01 * fabs(mn), 0.01);
  }
  double l = mn - pad;
  double h = mx + pad;
  /* Overflow (|v| near DBL_MAX) gives inf/NaN bounds or a span that every
   * later (x - lo) / (hi - lo) would turn into NaN: reject it. */
  if (!isfinite(l) || !isfinite(h) || !isfinite(h - l) || !(h > l))
    return FALSE;
  *lo = l;
  *hi = h;
  return TRUE;
}

/* Documented in stk-chart-geom.h. */
double
stk_chart_geom_map(double x, double d0, double d1, double r0, double r1)
{
  if (d1 == d0)
    return 0.5 * (r0 + r1);
  return r0 + (x - d0) / (d1 - d0) * (r1 - r0);
}

/* ------------------------------------------------------------------------ */
/* Decimation                                                               */
/* ------------------------------------------------------------------------ */

/**
 * column_of:
 * @t: sample timestamp, epoch ms.
 * @t0: domain start.
 * @span: domain length t1 - t0; not positive or NaN puts everything in column 0.
 * @columns: number of columns.
 *
 * Maps @t to a column index in [0, @columns - 1].
 *
 * Preconditions:
 *  - @columns >= 1 (the only caller returns early on 0), so @columns - 1 does
 *    not wrap.
 * Postconditions:
 *  - the result is < @columns for every input including NaN and infinities:
 *    the (gsize) cast is only reached for 0 < c < (double) @columns, and a
 *    NaN @t, @t0 or @span takes one of the earlier returns.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: the column index.
 */
static gsize
column_of(double t, double t0, double span, gsize columns)
{
  if (!(span > 0.0))
    return 0;
  double c = (t - t0) / span * (double)columns;
  if (!(c > 0.0))
    return 0;
  if (c >= (double)columns)
    return columns - 1;
  return (gsize)c;
}

/**
 * flush_column:
 * @imin: index of the lowest-valued sample of the finished column.
 * @imax: index of the highest-valued sample of the finished column.
 * @out: (out caller-allocates): receives the indices.
 * @written: number of entries already written to @out.
 * @out_cap: capacity of @out.
 *
 * Appends the smaller then the larger of @imin and @imax (once if equal) to
 * @out, never past @out_cap.
 *
 * Preconditions:
 *  - @out holds @out_cap entries; @written <= @out_cap.
 * Postconditions:
 *  - at most 2 entries are written, each only when @written < @out_cap, so
 *    @out is never written beyond index @out_cap - 1.
 * Thread affinity: any thread (pure).
 * Ownership: @out is caller-owned and written; nothing is retained.
 *
 * Returns: the new number of entries written (<= @out_cap).
 */
static gsize
flush_column(gsize imin, gsize imax, gsize *out, gsize written, gsize out_cap)
{
  gsize a = imin < imax ? imin : imax;
  gsize b = imin < imax ? imax : imin;
  if (written < out_cap)
    out[written++] = a;
  if (b != a && written < out_cap)
    out[written++] = b;
  return written;
}

/* Documented in stk-chart-geom.h. */
gsize
stk_chart_geom_decimate(const double *t, const double *v, gsize n, double t0, double t1,
                        gsize columns, gsize *out, gsize out_cap)
{
  if (n == 0 || columns == 0)
    return 0;

  double span = t1 - t0;
  gsize written = 0;
  gboolean have = FALSE;
  gsize cur = 0, imin = 0, imax = 0;

  for (gsize i = 0; i < n; i++) {
    if (!isfinite(v[i]))
      continue;
    gsize col = column_of(t[i], t0, span, columns);
    if (!have) {
      have = TRUE;
      cur = col;
      imin = imax = i;
    } else if (col != cur) {
      written = flush_column(imin, imax, out, written, out_cap);
      cur = col;
      imin = imax = i;
    } else {
      if (v[i] < v[imin])
        imin = i;
      if (v[i] > v[imax])
        imax = i;
    }
  }
  if (have)
    written = flush_column(imin, imax, out, written, out_cap);
  return written;
}

/* ------------------------------------------------------------------------ */
/* Ticks                                                                    */
/* ------------------------------------------------------------------------ */

/* Documented in stk-chart-geom.h. */
gsize
stk_chart_geom_nice_ticks(double lo, double hi, gsize max_ticks, double min_step, double *out)
{
  if (!isfinite(lo) || !isfinite(hi) || !(hi > lo) || max_ticks == 0)
    return 0;
  double span = hi - lo;
  if (!isfinite(span))
    return 0;
  if (max_ticks > STK_CHART_GEOM_MAX_TICKS)
    max_ticks = STK_CHART_GEOM_MAX_TICKS;

  /* A normal (not zero, not subnormal) first estimate keeps every later step
   * a normal positive number, so no division below can produce NaN/Inf. */
  double raw = span / (double)max_ticks;
  if (!(raw >= DBL_MIN))
    return 0;

  static const double mult[3] = { 1.0, 2.0, 5.0 };
  static const double exact_limit = 4503599627370496.0; /* 2^52 */
  double slack = 1e-9 * span; /* absorbs rounding of lo/step */
  double start = raw;
  if (min_step > start)
    start = min_step;
  double big = fmax(fabs(lo), fabs(hi)) / exact_limit;
  if (big > start)
    start = big;
  if (!isfinite(start))
    return 0;
  double base = pow(10.0, floor(log10(start)));

  for (int e = 0; e < 40; e++, base *= 10.0) {
    for (int m = 0; m < 3; m++) {
      double step = mult[m] * base;
      if (!isnormal(step) || step < min_step)
        continue;
      /* Multiples i * step are exact only for integer |i| <= 2^52. */
      if (!(fabs(lo) / step <= exact_limit) || !(fabs(hi) / step <= exact_limit))
        continue;
      double i0 = ceil((lo - slack) / step);
      double i1 = floor((hi + slack) / step);
      /* The rounded quotients can be one index off when |lo| >> span: correct
       * them against the exact products instead of dropping the spacing. */
      while (i0 * step < lo - slack)
        i0 += 1.0;
      while (i0 > -exact_limit && (i0 - 1.0) * step >= lo - slack)
        i0 -= 1.0;
      while (i1 * step > hi + slack)
        i1 -= 1.0;
      while (i1 < exact_limit && (i1 + 1.0) * step <= hi + slack)
        i1 += 1.0;
      double count = i1 - i0 + 1.0;
      if (!(count >= 1.0 && count <= (double)max_ticks))
        continue;
      gsize cnt = (gsize)count; /* 1 <= cnt <= max_ticks <= MAX_TICKS */

      double tmp[STK_CHART_GEOM_MAX_TICKS];
      gboolean ok = TRUE;
      for (gsize k = 0; k < cnt && ok; k++) {
        tmp[k] = (i0 + (double)k) * step;
        ok = tmp[k] >= lo - slack && tmp[k] <= hi + slack && (k == 0 || tmp[k] > tmp[k - 1]);
      }
      if (!ok)
        continue;
      memcpy(out, tmp, cnt * sizeof tmp[0]);
      return cnt;
    }
  }
  return 0;
}

typedef enum { UNIT_HOUR, UNIT_DAY, UNIT_MONTH } TickUnit;

/**
 * is_wall_hour:
 * @ms: instant to test, epoch ms.
 * @year: expected New York calendar year.
 * @month: expected New York calendar month, 1..12.
 * @day: expected New York day of month.
 * @hour: expected New York hour, 0..23.
 *
 * Tests whether @ms is exactly @hour:00:00 on the New York calendar day
 * (@year, @month, @day).
 *
 * Preconditions:
 *  - none; any @ms is accepted.
 * Postconditions:
 *  - FALSE when @ms is outside the years 1..9999 or the New York zone is
 *    unavailable. No state is changed.
 * Thread affinity: any thread (the shared zone is immutable).
 * Ownership: none (no pointers).
 *
 * Returns: TRUE if @ms is that wall-clock instant, FALSE otherwise.
 */
static gboolean
is_wall_hour(gint64 ms, int year, int month, int day, int hour)
{
  GDateTime *dt = zone_datetime(ny_tz(), ms);
  if (dt == NULL)
    return FALSE;
  gboolean yes = g_date_time_get_year(dt) == year && g_date_time_get_month(dt) == month &&
                 g_date_time_get_day_of_month(dt) == day && g_date_time_get_hour(dt) == hour &&
                 g_date_time_get_minute(dt) == 0 && g_date_time_get_second(dt) == 0;
  g_date_time_unref(dt);
  return yes;
}

/**
 * generate_hour_ticks:
 * @step: spacing in hours; one of 1, 2, 3, 4, 6, 12, 24 (divides 24).
 * @t0: domain start, epoch ms.
 * @t1: domain end, epoch ms.
 * @max_ticks: capacity of @out.
 * @out: (out caller-allocates): receives the ticks.
 *
 * Hour ticks: every @step hours of New York WALL-CLOCK time, i.e. at the
 * local hours 0, step, 2*step, ... of every local day, so that DST changes
 * cannot shift the alignment (every allowed step divides 24). Local times
 * that do not exist (spring-forward gap) are skipped. A local hour that
 * occurs twice (fall-back) yields ONE tick: the earlier (daylight-time)
 * instant if it lies in [t0, t1], otherwise the later one if that does.
 * Writes at most @max_ticks entries and returns the count, or max_ticks + 1
 * as soon as there would be more.
 *
 * Preconditions:
 *  - @step is one of the values listed (so 24 / @step >= 1 and no division by
 *    zero; the only caller takes it from its hour_steps table); @out holds
 *    @max_ticks entries.
 * Postconditions:
 *  - returns 0 (nothing written) when the New York zone is unavailable or
 *    @t0 is outside the years 1..9999. Every GDateTime created here is
 *    unreferenced before returning; the ticks are ascending.
 *  - termination: the slot counter grows by 1 per pass and the wall time of
 *    slot k is non-decreasing in k, so the loop stops at the first tick
 *    > @t1, or when GLib returns NULL for a day beyond year 9999 (so
 *    slot / (24 / @step) stays far below INT_MAX, and the (int) cast of the
 *    day offset is lossless), or after @max_ticks + 1 emitted ticks. The hour
 *    (slot % (24 / @step)) * @step is below 24.
 * Thread affinity: any thread (the shared zone is immutable).
 * Ownership: @out is caller-owned and written; nothing is retained.
 *
 * Returns: number of ticks written, or @max_ticks + 1 if there would be more.
 */
static gsize
generate_hour_ticks(int step, gint64 t0, gint64 t1, gsize max_ticks, gint64 *out)
{
  GTimeZone *tz = ny_tz();
  GDateTime *a = zone_datetime(tz, t0);
  if (a == NULL)
    return 0;
  GDateTime *base = g_date_time_new(tz, g_date_time_get_year(a), g_date_time_get_month(a),
                                    g_date_time_get_day_of_month(a), 0, 0, 0.0);
  g_date_time_unref(a);
  if (base == NULL)
    return 0;

  gsize per_day = (gsize)(24 / step);
  gint64 last = G_MININT64;
  gsize n = 0;
  for (gsize slot = 0;; slot++) {
    GDateTime *day = g_date_time_add_days(base, (int)(slot / per_day));
    if (day == NULL)
      break;
    int hour = (int)(slot % per_day) * step;
    int y = g_date_time_get_year(day), mo = g_date_time_get_month(day);
    int d = g_date_time_get_day_of_month(day);
    GDateTime *cur = g_date_time_new(tz, y, mo, d, hour, 0, 0.0);
    g_date_time_unref(day);
    if (cur == NULL)
      break;
    gboolean exists = g_date_time_get_hour(cur) == hour;
    gint64 ms = datetime_ms(cur);
    g_date_time_unref(cur);
    if (exists) {
      /* GLib resolves an ambiguous time to one of its two instants; the other
       * one is exactly one hour away. Take the earlier as the candidate. */
      gint64 other = ms - 3600000;
      if (!is_wall_hour(other, y, mo, d, hour))
        other = ms + 3600000;
      if (is_wall_hour(other, y, mo, d, hour) && other != ms) {
        gint64 first = MIN(ms, other), second = MAX(ms, other);
        ms = first >= t0 ? first : second;
        if (first > t1)
          break;
      }
    }
    if (ms > t1)
      break;
    if (!exists || ms < t0 || ms <= last)
      continue;
    if (n == max_ticks) {
      g_date_time_unref(base);
      return max_ticks + 1;
    }
    out[n++] = ms;
    last = ms;
  }
  g_date_time_unref(base);
  return n;
}

/**
 * generate_calendar_ticks:
 * @tz: (not nullable): zone whose local midnights are the ticks (the UTC zone).
 * @unit: UNIT_DAY (every @step days) or UNIT_MONTH (first of every @step-th
 *   month).
 * @step: spacing in days or months; at least 1.
 * @t0: domain start, epoch ms.
 * @t1: domain end, epoch ms.
 * @max_ticks: capacity of @out.
 * @out: (out caller-allocates): receives the ticks.
 *
 * Day / month ticks in @tz at local midnights every @step days (counted from
 * the first midnight >= @t0) or on the first of every @step-th month. Writes
 * at most @max_ticks entries; returns the count, or max_ticks + 1 as soon as
 * there would be more (so huge domains terminate quickly).
 *
 * Preconditions:
 *  - @tz is non-NULL; @step >= 1; @out holds @max_ticks entries (the only
 *    caller passes STK_CHART_GEOM_MAX_TICKS at most).
 * Postconditions:
 *  - returns 0 (nothing written) when @t0 is outside the years 1..9999. Every
 *    GDateTime created here is unreferenced before returning; the ticks are
 *    ascending and inside [@t0, @t1].
 *  - termination: the search for the first midnight >= @t0 starts at the
 *    midnight of @t0's own date (<= @t0) and so advances at most once; the
 *    emit loop advances by >= 1 day / month per pass, stops at the first tick
 *    > @t1 or when GLib returns NULL beyond year 9999, and returns after at
 *    most @max_ticks + 1 passes.
 * Thread affinity: any thread (the shared zone is immutable).
 * Ownership: @tz is borrowed; @out is caller-owned and written.
 *
 * Returns: number of ticks written, or @max_ticks + 1 if there would be more.
 */
static gsize
generate_calendar_ticks(GTimeZone *tz, TickUnit unit, int step, gint64 t0, gint64 t1,
                        gsize max_ticks, gint64 *out)
{
  GDateTime *a = zone_datetime(tz, t0);
  if (a == NULL)
    return 0;
  GDateTime *cur = g_date_time_new(tz, g_date_time_get_year(a), g_date_time_get_month(a),
                                   unit == UNIT_MONTH ? 1 : g_date_time_get_day_of_month(a), 0, 0,
                                   0.0);
  g_date_time_unref(a);

  /* Advance one unit at a time to the first instant >= t0. */
  while (cur != NULL && datetime_ms(cur) < t0) {
    GDateTime *next = unit == UNIT_MONTH ? g_date_time_add_months(cur, 1)
                                         : g_date_time_add_days(cur, 1);
    g_date_time_unref(cur);
    cur = next;
  }

  gsize n = 0;
  while (cur != NULL && datetime_ms(cur) <= t1) {
    if (n == max_ticks) {
      g_date_time_unref(cur);
      return max_ticks + 1;
    }
    out[n++] = datetime_ms(cur);
    GDateTime *next = unit == UNIT_MONTH ? g_date_time_add_months(cur, step)
                                         : g_date_time_add_days(cur, step);
    g_date_time_unref(cur);
    cur = next;
  }
  if (cur != NULL)
    g_date_time_unref(cur);
  return n;
}

/**
 * generate_ticks:
 * @unit: tick unit; selects the generator.
 * @step: spacing in units of @unit.
 * @t0: domain start, epoch ms.
 * @t1: domain end, epoch ms.
 * @max_ticks: capacity of @out and tick limit.
 * @out: (out caller-allocates): receives the ticks.
 *
 * Dispatches to generate_hour_ticks() (UNIT_HOUR, New York) or
 * generate_calendar_ticks() (UNIT_DAY / UNIT_MONTH, UTC).
 *
 * Preconditions:
 *  - those of the generator selected: @step comes from the step tables of
 *    stk_chart_geom_time_ticks(), @max_ticks <= STK_CHART_GEOM_MAX_TICKS and
 *    @out holds @max_ticks entries.
 * Postconditions:
 *  - as the generator selected: the tick count, or @max_ticks + 1 when more
 *    ticks exist.
 * Thread affinity: any thread (shared zones are immutable).
 * Ownership: @out is caller-owned and written; nothing is retained.
 *
 * Returns: number of ticks written, or @max_ticks + 1 if there would be more.
 */
static gsize
generate_ticks(TickUnit unit, int step, gint64 t0, gint64 t1, gsize max_ticks, gint64 *out)
{
  if (unit == UNIT_HOUR)
    return generate_hour_ticks(step, t0, t1, max_ticks, out);
  return generate_calendar_ticks(utc_tz(), unit, step, t0, t1, max_ticks, out);
}

/* Documented in stk-chart-geom.h. */
gsize
stk_chart_geom_time_ticks(StkRange range, gint64 t0_ms, gint64 t1_ms, gsize max_ticks,
                          gint64 *out)
{
  if (t1_ms <= t0_ms || max_ticks == 0)
    return 0;
  if (max_ticks > STK_CHART_GEOM_MAX_TICKS)
    max_ticks = STK_CHART_GEOM_MAX_TICKS;

  static const int hour_steps[] = { 1, 2, 3, 4, 6, 12, 24 };
  static const int day_steps[] = { 1, 2, 5, 7, 14, 28, 56, 84, 168, 365 };
  static const int month_steps[] = { 1, 2, 3, 4, 6, 12 };

  TickUnit unit;
  const int *steps;
  gsize n_steps;
  switch (range) {
  case STK_RANGE_1D:
    unit = UNIT_HOUR;
    steps = hour_steps;
    n_steps = G_N_ELEMENTS(hour_steps);
    break;
  case STK_RANGE_1Y:
    unit = UNIT_MONTH;
    steps = month_steps;
    n_steps = G_N_ELEMENTS(month_steps);
    break;
  case STK_RANGE_1M:
  case STK_RANGE_3M:
  case STK_RANGE_6M:
  default:
    unit = UNIT_DAY;
    steps = day_steps;
    n_steps = G_N_ELEMENTS(day_steps);
    break;
  }

  gint64 tmp[STK_CHART_GEOM_MAX_TICKS];
  for (gsize i = 0; i < n_steps; i++) {
    gsize n = generate_ticks(unit, steps[i], t0_ms, t1_ms, max_ticks, tmp);
    if (n <= max_ticks) {
      memcpy(out, tmp, n * sizeof tmp[0]);
      return n;
    }
  }
  return 0;
}

/* Documented in stk-chart-geom.h. */
gboolean
stk_chart_geom_nearest(const double *t, gsize n, double target, gsize *idx)
{
  if (n == 0 || isnan(target))
    return FALSE;

  /* first index with t >= target */
  gsize lo = 0, hi = n;
  while (lo < hi) {
    gsize mid = lo + (hi - lo) / 2;
    if (t[mid] < target)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo == 0) {
    *idx = 0;
  } else if (lo == n) {
    *idx = n - 1;
  } else {
    *idx = (target - t[lo - 1]) <= (t[lo] - target) ? lo - 1 : lo;
  }
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Formatting                                                               */
/* ------------------------------------------------------------------------ */

/**
 * put_dash:
 * @buf: (out caller-allocates): output buffer.
 * @len: size of @buf in bytes.
 *
 * Writes the em dash placeholder "—" into @buf.
 *
 * Preconditions:
 *  - @buf is writable for @len bytes and @len > 0 (every caller returns early
 *    on len == 0).
 * Postconditions:
 *  - @buf holds the NUL-terminated dash, truncated by g_strlcpy() to @len - 1
 *    bytes if @len is too small (a partial UTF-8 sequence is then possible).
 * Thread affinity: any thread (pure).
 * Ownership: @buf is caller-owned and written; nothing is retained.
 *
 * Returns: void.
 */
static void
put_dash(char *buf, gsize len)
{
  g_strlcpy(buf, EM_DASH, len);
}

/* Documented in stk-chart-geom.h. */
void
stk_chart_geom_format_price(double v, char *buf, gsize len)
{
  if (len == 0)
    return;
  if (!isfinite(v) || fabs(v) >= 1e12) {
    put_dash(buf, len);
    return;
  }

  /* Bound proof: |v| < 1e12 here, so "%.2f" yields at most '-' + 13 digits
   * (rounding may carry 999999999999.995 up to 1000000000000.00) + '.' + 2
   * decimals = 17 bytes + NUL, well inside raw[40]; g_ascii_formatd()
   * NUL-terminates what it writes, so the strchr/strlen/strspn calls below run
   * on a string created by GLib in this function. out[64] receives at most
   * 1 + 13 + 4 commas + 3 + NUL = 22 bytes. */
  char raw[40];
  g_ascii_formatd(raw, sizeof raw, "%.2f", v);

  const char *p = raw;
  gboolean neg = *p == '-';
  if (neg)
    p++;
  const char *dot = strchr(p, '.');
  gsize int_len = dot != NULL ? (gsize)(dot - p) : strlen(p);

  /* "-0.00" -> "0.00" */
  if (neg && int_len == 1 && p[0] == '0' && dot != NULL && strspn(dot + 1, "0") == strlen(dot + 1))
    neg = FALSE;

  char out[64];
  gsize o = 0;
  if (neg)
    out[o++] = '-';
  for (gsize i = 0; i < int_len; i++) {
    out[o++] = p[i];
    gsize remaining = int_len - 1 - i;
    if (remaining > 0 && remaining % 3 == 0)
      out[o++] = ',';
  }
  if (dot != NULL) {
    gsize rest = strlen(dot);
    memcpy(out + o, dot, rest);
    o += rest;
  }
  out[o] = '\0';
  g_strlcpy(buf, out, len);
}

/* Index proof for month_names[g_date_time_get_month(dt) - 1] in the two
 * format functions: g_date_time_get_month() is documented to return 1..12 for
 * every valid GDateTime, and every GDateTime used there is non-NULL (checked
 * by the callers), so the index lies in 0..11. */
static const char *const month_names[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/**
 * format_clock:
 * @dt: (not nullable): local date-time whose hour and minute are printed.
 * @buf: (out caller-allocates): output buffer.
 * @len: size of @buf in bytes.
 * @always_minutes: TRUE to print ":MM" even when the minute is 0.
 *
 * Writes the 12-hour clock text "H:MM AM" / "H AM" (English, locale
 * independent) of @dt.
 *
 * Preconditions:
 *  - @dt is non-NULL; @buf is writable for @len bytes and @len > 0 (both
 *    callers return early on len == 0).
 * Postconditions:
 *  - @buf holds a NUL-terminated string, truncated by g_snprintf() to @len - 1
 *    bytes if @len is too small; @dt is not modified or unreferenced.
 * Thread affinity: any thread (pure).
 * Ownership: @dt is borrowed; @buf is caller-owned and written; nothing is
 *   retained.
 *
 * Returns: void.
 */
static void
format_clock(GDateTime *dt, char *buf, gsize len, gboolean always_minutes)
{
  int hour = g_date_time_get_hour(dt);
  int minute = g_date_time_get_minute(dt);
  int h12 = hour % 12 == 0 ? 12 : hour % 12;
  const char *ampm = hour < 12 ? "AM" : "PM";
  if (minute != 0 || always_minutes)
    g_snprintf(buf, len, "%d:%02d %s", h12, minute, ampm);
  else
    g_snprintf(buf, len, "%d %s", h12, ampm);
}

/* Documented in stk-chart-geom.h. */
void
stk_chart_geom_format_tick(StkRange range, gint64 ms, char *buf, gsize len)
{
  if (len == 0)
    return;
  GDateTime *dt = zone_datetime(range_tz(range), ms);
  if (dt == NULL) {
    put_dash(buf, len);
    return;
  }

  const char *month = month_names[g_date_time_get_month(dt) - 1];
  switch (range) {
  case STK_RANGE_1D:
    format_clock(dt, buf, len, FALSE);
    break;
  case STK_RANGE_1Y:
    g_snprintf(buf, len, "%s", month);
    break;
  case STK_RANGE_1M:
  case STK_RANGE_3M:
  case STK_RANGE_6M:
  default:
    g_snprintf(buf, len, "%d %s", g_date_time_get_day_of_month(dt), month);
    break;
  }
  g_date_time_unref(dt);
}

/* Documented in stk-chart-geom.h. */
void
stk_chart_geom_format_hover(StkRange range, gint64 ms, char *buf, gsize len)
{
  if (len == 0)
    return;
  GDateTime *dt = zone_datetime(range_tz(range), ms);
  if (dt == NULL) {
    put_dash(buf, len);
    return;
  }

  const char *month = month_names[g_date_time_get_month(dt) - 1];
  int day = g_date_time_get_day_of_month(dt);
  if (range == STK_RANGE_1D) {
    char clock[STK_CHART_GEOM_LABEL_LEN];
    format_clock(dt, clock, sizeof clock, TRUE);
    g_snprintf(buf, len, "%s %d, %s", month, day, clock);
  } else {
    g_snprintf(buf, len, "%s %d, %d", month, day, g_date_time_get_year(dt));
  }
  g_date_time_unref(dt);
}
