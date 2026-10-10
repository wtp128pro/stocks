/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-chart-geom.h - pure chart geometry and label formatting (no GTK).
 *
 * Everything here works on plain arrays of doubles. Timestamps are epoch
 * milliseconds stored as double (exact for |t| < 2^53, far beyond any real
 * date). Functions are pure apart from shared, immutable GTimeZone objects
 * (America/New_York, UTC) that are created once and never freed
 * (thread-safe); none keeps mutable state, so they may be called from any
 * thread.
 *
 * Timestamp convention (binding across the app): for STK_RANGE_1D, t_ms are
 * true UTC instants, labelled in America/New_York time. For the daily ranges
 * 1M/3M/6M/1Y, t_ms is 00:00:00 UTC of the trading date, so the sample's
 * DATE is its UTC date; daily labels and ticks are therefore computed in UTC
 * and never converted to New York time.
 *
 * The series fed to these functions is expected to be what StkSeries
 * guarantees: finite values and non-decreasing timestamps. Functions that
 * depend on ordering say so; none reads or writes out of bounds when the
 * promise is broken.
 */
#ifndef STK_CHART_GEOM_H
#define STK_CHART_GEOM_H

#include <glib.h>

#include "stk-types.h"

G_BEGIN_DECLS

/** Upper bound for every "max_ticks" argument; output arrays of this size
 *  (plus nothing else) are always large enough for the tick functions. */
#define STK_CHART_GEOM_MAX_TICKS 8

/** Largest number of samples a chart keeps and processes, however long the
 *  input series is (see stk_chart_geom_sample()). */
#define STK_CHART_GEOM_MAX_SAMPLES 4096

/** Largest number of pixel columns used for decimation, however wide the
 *  widget is. */
#define STK_CHART_GEOM_MAX_COLUMNS 8192

/** Buffer size sufficient for any label written by the format functions. */
#define STK_CHART_GEOM_LABEL_LEN 40

/**
 * stk_chart_geom_is_up:
 * @v: (nullable): @n values; may be NULL only when @n is less than 2.
 * @n: number of values.
 *
 * Direction of a series: last value against first value.
 *
 * Preconditions:
 *  - when @n >= 2, @v is non-NULL and points to @n readable doubles (a NULL
 *    @v with @n >= 2 is dereferenced; it is a caller error and is not
 *    detected).
 * Postconditions:
 *  - no state is changed; @v is only read (v[0] and v[n - 1]). A NaN end value
 *    makes the comparison false (reported as "down").
 * Thread affinity: any thread (pure).
 * Ownership: @v is borrowed for the call; nothing is retained.
 *
 * Returns: TRUE if the last value is >= the first value. A series with fewer
 *   than two values counts as up.
 */
gboolean stk_chart_geom_is_up(const double *v, gsize n);

/**
 * stk_chart_geom_is_up_vs_close:
 * @v: (nullable): @n values; may be NULL only when @n is 0.
 * @n: number of values.
 * @prev_close: the previous close, or a non-finite value (NaN, +inf, -inf)
 *   when there is none.
 *
 * Direction of the chart widget's colour for a chart that may carry a previous close. When
 * @prev_close is finite, returns TRUE if the last value is >= @prev_close
 * (equality counts as up; a single value is compared too); n == 0 returns
 * TRUE. When @prev_close is not finite, returns exactly
 * stk_chart_geom_is_up(@v, @n) (last value >= first value).
 *
 * Preconditions:
 *  - when @n >= 1, @v is non-NULL and points to @n readable doubles (a NULL
 *    @v with @n >= 1 is dereferenced; it is a caller error and is not
 *    detected).
 * Postconditions:
 *  - no state is changed; @v is only read.
 * Thread affinity: any thread (pure).
 * Ownership: @v is borrowed for the call; nothing is retained.
 *
 * Returns: TRUE if the series is up as described above, FALSE otherwise.
 */
gboolean stk_chart_geom_is_up_vs_close(const double *v, gsize n, double prev_close);

/**
 * stk_chart_geom_domain:
 * @range: the series range.
 * @t: (nullable): @n ascending timestamps; may be NULL only when @n is 0.
 * @n: number of timestamps.
 * @t0: (out): receives the left edge of the x axis.
 * @t1: (out): receives the right edge of the x axis.
 *
 * For STK_RANGE_1D the domain is the regular session (09:30-16:00, or the
 * early close, America/New_York) of the New York date of the LAST timestamp,
 * so that a partial trading day occupies only the left part of the axis. If
 * that date has no regular session (weekend, holiday, outside the calendar's
 * supported years) and for every other range, the domain is
 * [first, last] of @t.
 *
 * Preconditions:
 *  - when @n >= 1, @t is non-NULL and points to @n readable doubles; @t0 and
 *    @t1 are non-NULL and writable (violations are dereferenced and not
 *    detected).
 *  - only for the ordering promise under Returns: @t is ascending and free of NaN. Other
 *    input is accepted and memory-safe but then *t0 <= *t1 is not guaranteed.
 * Postconditions:
 *  - on TRUE both outputs are written; on FALSE they are untouched.
 *  - a last timestamp that is NaN, infinite or of magnitude >= 2^63 has no New
 *    York date: it is never cast to gint64 (that cast is undefined) and takes
 *    the [first, last] path, exactly as an out-of-calendar date does.
 *    Results for every other input are unchanged.
 *  - if the New York time zone is unavailable the 1D case also takes the
 *    [first, last] path.
 * Thread affinity: any thread (pure; the shared zone is immutable).
 * Ownership: @t is borrowed for the call; @t0 and @t1 are caller-owned.
 *
 * Returns: FALSE (outputs untouched) when @n is 0; otherwise TRUE. For ascending,
 *   NaN-free @t, *t0 <= *t1 (equal when the first..last rule applies to a single
 *   timestamp); for other input (unordered, NaN or infinite timestamps) the order of
 *   *t0 and *t1 is not guaranteed.
 */
gboolean stk_chart_geom_domain(StkRange range, const double *t, gsize n, double *t0, double *t1);

/**
 * stk_chart_geom_sample:
 * @range: the series range (selects the domain, see stk_chart_geom_domain()).
 * @points: (nullable): @n samples, ascending t_ms; may be NULL only when @n
 *   is 0. Read in place; nothing of size @n is ever allocated.
 * @n: number of samples.
 * @max_out: capacity of @t and @v; at least 4 (otherwise 0 is returned).
 *   Callers use STK_CHART_GEOM_MAX_SAMPLES or less.
 * @t: (out caller-allocates): @max_out doubles; receives the kept t_ms.
 * @v: (out caller-allocates): @max_out doubles; receives the kept values.
 * @t0: (out): receives the left edge of the x axis (stk_chart_geom_domain()).
 * @t1: (out): receives the right edge of the x axis.
 *
 * Takes the samples that lie inside the x domain and bounds them to
 * @max_out. If at most @max_out lie inside, all are copied. Otherwise the
 * first and last sample plus the lowest and highest finite value of each of
 * (@max_out - 2) / 2 equal index buckets are kept, so the first/last
 * direction and the global minimum/maximum survive (a single pass; memory
 * is O(@max_out) regardless of @n). Output is ascending without duplicates.
 *
 * Preconditions:
 *  - when @n >= 1 and @max_out >= 4: @points points to @n readable samples
 *    and @t, @v, @t0, @t1 are non-NULL and writable (@t and @v for @max_out
 *    doubles each).
 *  - @n is below 2^32.
 *  - non-ascending or NaN timestamps are tolerated memory-safely (the binary
 *    searches keep their indices inside [0, @n]) but give an unspecified
 *    window.
 * Postconditions:
 *  - never writes beyond @max_out entries of @t and @v; reads only
 *    @points[0 .. @n - 1].
 *  - when the result is 0, @t, @v, @t0 and @t1 are untouched; otherwise @t0
 *    and @t1 are written.
 * Thread affinity: any thread (pure; the shared zone is immutable).
 * Ownership: @points is borrowed for the call; @t, @v, @t0 and @t1 are
 *   caller-owned; nothing is retained.
 *
 * Returns: number of samples written (<= @max_out), 0 (outputs untouched)
 *   when @n is 0, @points is NULL, @max_out < 4 or no sample lies inside the
 *   domain.
 */
gsize stk_chart_geom_sample(StkRange range, const StkPoint *points, gsize n, gsize max_out,
                            double *t, double *v, double *t0, double *t1);

/**
 * stk_chart_geom_y_range:
 * @v: (nullable): @n values; may be NULL only when @n is 0.
 * @n: number of values.
 * @include: a value that must lie inside the range (e.g. the previous close),
 *   or NAN for none.
 * @pad_frac: fraction of the span added above and below; negative values are
 *   treated as 0.
 * @lo: (out): receives the lower bound.
 * @hi: (out): receives the upper bound.
 *
 * Computes [min, max] over the finite entries of @v and @include, then pads
 * each side by @pad_frac * (max - min). A flat range (max == min) is instead
 * widened by max(1% of |value|, 0.01) on each side so that hi > lo always.
 * Non-finite entries of @v are ignored.
 *
 * Preconditions:
 *  - when @n >= 1, @v is non-NULL and points to @n readable doubles; @lo and
 *    @hi are non-NULL and writable (violations are dereferenced and not
 *    detected).
 * Postconditions:
 *  - on TRUE both outputs are written; on FALSE they are untouched.
 * Thread affinity: any thread (pure).
 * Ownership: @v is borrowed for the call; @lo and @hi are caller-owned.
 *
 * Returns: FALSE (outputs untouched) if there is no finite value at all or
 *   if the padded bounds or their difference overflow double (|values| near
 *   DBL_MAX, e.g. -1e308..1e308); otherwise TRUE with finite *lo, *hi,
 *   finite *hi - *lo and *hi > *lo.
 */
gboolean stk_chart_geom_y_range(const double *v, gsize n, double include, double pad_frac,
                                double *lo, double *hi);

/**
 * stk_chart_geom_map:
 * @x: value in domain [@d0, @d1].
 * @d0: domain start.
 * @d1: domain end.
 * @r0: image of @d0.
 * @r1: image of @d1.
 *
 * Linear map of @x from [@d0, @d1] onto [@r0, @r1] (no clamping). @r0 may
 * exceed @r1 (flipped y axis); swapping domain and image gives the inverse
 * map. A degenerate domain (@d0 == @d1) maps everything to the midpoint of
 * the image.
 *
 * Preconditions:
 *  - none; the function is pure arithmetic and reads or writes no memory. The behaviour
 *    described above holds for finite arguments whose intermediate results do not overflow.
 *    For NaN, infinite or overflowing arguments the returned value is unspecified (it may be
 *    NaN or infinite).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: the mapped value.
 */
double stk_chart_geom_map(double x, double d0, double d1, double r0, double r1);

/**
 * stk_chart_geom_decimate:
 * @t: (nullable): @n ascending timestamps; may be NULL only when @n is 0.
 * @v: (nullable): @n values, parallel to @t; may be NULL only when @n is 0.
 * @n: number of samples.
 * @t0: x-axis domain start.
 * @t1: x-axis domain end.
 * @columns: number of pixel columns the domain is drawn on (>= 1; 0 gives 0
 *   output).
 * @out: (out caller-allocates): indices of the kept samples.
 * @out_cap: capacity of @out. For ascending @t, 2 * @columns always suffices.
 *
 * Min/max decimation: the domain is cut into @columns equal columns (sample
 * time -> floor((t - t0) / (t1 - t0) * columns), clamped to [0, columns-1];
 * everything falls into column 0 if t1 <= t0). From each non-empty column the
 * samples with the lowest and the highest value are kept (first one on ties),
 * so at most 2 samples per column survive and the global minimum and maximum
 * of the finite values always survive. Samples with non-finite values are
 * skipped. Output indices are ascending (original order). Columns are
 * processed in input order, so ascending @t is required for the 2-per-column
 * bound; output is truncated at @out_cap regardless.
 *
 * Preconditions:
 *  - when @n >= 1 and @columns >= 1: @t and @v are non-NULL and point to @n
 *    readable doubles each; @out is non-NULL when @out_cap > 0 and holds
 *    @out_cap entries.
 * Postconditions:
 *  - never writes beyond @out_cap entries of @out; every written index is
 *    below @n.
 *  - a NaN @t[i] or non-ascending @t is memory-safe (the column index is
 *    always below @columns) but may emit more or fewer samples per column
 *    than the 2-per-column bound.
 * Thread affinity: any thread (pure).
 * Ownership: @t and @v are borrowed for the call; @out is caller-owned.
 *
 * Returns: number of indices written to @out.
 */
gsize stk_chart_geom_decimate(const double *t, const double *v, gsize n, double t0, double t1,
                              gsize columns, gsize *out, gsize out_cap);

/**
 * stk_chart_geom_nice_ticks:
 * @lo: axis lower bound.
 * @hi: axis upper bound (> @lo, and @hi - @lo must be finite).
 * @max_ticks: maximum number of ticks wanted; clamped to
 *   STK_CHART_GEOM_MAX_TICKS.
 * @min_step: smallest allowed tick spacing; values <= 0 (and NaN) mean no
 *   minimum.
 * @out: (out caller-allocates): room for STK_CHART_GEOM_MAX_TICKS doubles.
 *
 * Chooses the smallest "nice" spacing 1, 2 or 5 times a power of ten (and
 * >= @min_step) that yields between 1 and @max_ticks multiples inside
 * [@lo, @hi] (bounds are inclusive, with a tolerance of 1e-9 of the span for
 * rounding), and writes those multiples ascending to @out.
 *
 * Guarantees for ALL inputs (any finite or non-finite doubles): at most
 * min(@max_ticks, STK_CHART_GEOM_MAX_TICKS) entries are written, and @out is
 * untouched when 0 is returned. Every written tick is i * step for an integer
 * i with |i| <= 2^52 (so each tick is the correctly rounded multiple), the
 * ticks are strictly ascending and lie inside [@lo - 1e-9 * span,
 * @hi + 1e-9 * span]. Spacings are tried from the smallest up; a spacing is
 * skipped if it is not a normal positive double, is below @min_step, if
 * |@lo| / step or |@hi| / step exceeds 2^52, or if its ticks would violate
 * the above.
 *
 * Preconditions:
 *  - @out is non-NULL and writable for STK_CHART_GEOM_MAX_TICKS doubles
 *    whenever the call can succeed (the function writes at most that many,
 *    and only via a final copy after all checks passed).
 * Postconditions:
 *  - see above: the count written is the return value; @out is untouched when
 *    0 is returned. The only float-to-integer cast, (gsize)count, is reached
 *    only with 1 <= count <= @max_ticks <= STK_CHART_GEOM_MAX_TICKS.
 * Thread affinity: any thread (pure).
 * Ownership: @out is caller-owned; nothing is retained.
 *
 * Returns: number of ticks written. 0 if @lo/@hi are not finite, @hi <= @lo,
 *   @hi - @lo overflows, @max_ticks is 0, (@hi - @lo) / @max_ticks (after
 *   clamping) is below DBL_MIN (about 2.2e-308, i.e. subnormal spans), or no
 *   allowed spacing puts between 1 and @max_ticks multiples inside the range
 *   (e.g. offsets so large relative to the span, such as 1e19 * span, that
 *   no exact multiple exists). Consecutive 1-2-5 spacings are at most 2.5
 *   times apart, so with @max_ticks >= 3, @min_step <= (@hi - @lo) / 2.5 and
 *   |@lo|, |@hi| <= 2^52 * (@hi - @lo) / @max_ticks a tick is always found;
 *   otherwise 0 may be returned.
 */
gsize stk_chart_geom_nice_ticks(double lo, double hi, gsize max_ticks, double min_step,
                                double *out);

/**
 * stk_chart_geom_time_ticks:
 * @range: determines the tick unit: STK_RANGE_1D whole America/New_York hours
 *   (spacing 1, 2, 3, 4, 6, 12 or 24 h, at the New York wall-clock hours
 *   0, step, 2*step, ... of each local day, also on DST-change days: a
 *   non-existent local hour gets no tick, and a repeated one (fall-back, 1 AM)
 *   gets ONE tick, at its first (EDT) instant if that lies in the domain,
 *   else at its second (EST) instant if that does); 1M/3M/6M UTC midnights (spacing
 *   1, 2, 5, 7, 14, 28, 56, 84, 168 or 365 days from the first midnight >=
 *   @t0_ms); 1Y 00:00 UTC on the first of the month (spacing 1, 2, 3, 4, 6
 *   or 12 months). The daily ranges use UTC because their sample timestamps
 *   are 00:00 UTC of the trading date.
 * @t0_ms: domain start, epoch ms.
 * @t1_ms: domain end, epoch ms.
 * @max_ticks: maximum number of ticks; clamped to STK_CHART_GEOM_MAX_TICKS.
 * @out: (out caller-allocates): room for STK_CHART_GEOM_MAX_TICKS values.
 *
 * Writes ascending tick instants inside [@t0_ms, @t1_ms], using the finest
 * spacing from the lists above that gives at most @max_ticks.
 *
 * Preconditions:
 *  - @out is non-NULL and writable for STK_CHART_GEOM_MAX_TICKS gint64 values
 *    whenever the call can succeed (at most min(@max_ticks,
 *    STK_CHART_GEOM_MAX_TICKS) are written).
 * Postconditions:
 *  - @out is written only on a non-zero return, and then only with the
 *    returned number of entries. Every GDateTime created internally is
 *    released. Termination is bounded: each generator stops after at most
 *    @max_ticks + 1 emitted ticks, at the first instant past @t1_ms, or at
 *    year 9999.
 *  - if the New York time zone is unavailable, STK_RANGE_1D returns 0 (this
 *    replaces a former g_assert()).
 * Thread affinity: any thread (pure; the shared zones are immutable).
 * Ownership: @out is caller-owned; nothing is retained.
 *
 * Returns: number of ticks; 0 if the domain is empty or inverted, @max_ticks
 *   is 0, the domain lies outside the representable date range (see
 *   stk_chart_geom_format_tick()),
 *   contains no tick instant at all, or even the coarsest spacing gives more
 *   than @max_ticks.
 */
gsize stk_chart_geom_time_ticks(StkRange range, gint64 t0_ms, gint64 t1_ms, gsize max_ticks,
                                gint64 *out);

/**
 * stk_chart_geom_nearest:
 * @t: (nullable): @n ascending timestamps; may be NULL only when @n is 0.
 * @n: number of timestamps.
 * @target: instant to look up.
 * @idx: (out): receives the index of the closest timestamp.
 *
 * Binary search; on an exact tie between two neighbours the earlier one wins.
 * A target before the first / after the last timestamp yields index 0 / n-1.
 *
 * Preconditions:
 *  - when @n >= 1, @t is non-NULL and points to @n readable doubles and @idx
 *    is non-NULL and writable (violations are dereferenced and not detected).
 *  - @t is ascending and free of NaN. With NaN or unordered entries the
 *    search stays memory-safe (the written index is always < @n) but the
 *    chosen index is unspecified.
 * Postconditions:
 *  - on TRUE *@idx < @n; on FALSE @idx is untouched.
 * Thread affinity: any thread (pure).
 * Ownership: @t is borrowed for the call; @idx is caller-owned.
 *
 * Returns: FALSE (@idx untouched) if @n is 0 or @target is NaN.
 */
gboolean stk_chart_geom_nearest(const double *t, gsize n, double target, gsize *idx);

/**
 * stk_chart_geom_format_price:
 * @v: price.
 * @buf: (out caller-allocates): output buffer.
 * @len: size of @buf; STK_CHART_GEOM_LABEL_LEN always suffices.
 *
 * Locale-independent "1,234.56": exactly 2 decimals, ',' thousands grouping,
 * leading '-' for negative values ("-0.00" is never produced). Non-finite
 * values or |v| >= 1e12 produce an em dash. Output is always NUL-terminated
 * (truncated if @len is too small); @len 0 writes nothing.
 *
 * Preconditions:
 *  - when @len > 0, @buf is non-NULL and writable for @len bytes.
 * Postconditions:
 *  - for finite |@v| < 1e12 the intermediate text is at most 18 bytes
 *    including the NUL (raw[40]) and the grouped text at most 22 (out[64]), so
 *    the internal buffers cannot overflow; a larger magnitude or a
 *    non-finite value is replaced by the em dash before any formatting.
 *  - @buf is NUL-terminated whenever @len > 0 (truncated to @len - 1 bytes if
 *    needed; a truncated em dash may end in a partial UTF-8 sequence).
 * Thread affinity: any thread (pure).
 * Ownership: @buf is caller-owned and written; nothing is retained.
 *
 * Returns: void.
 */
void stk_chart_geom_format_price(double v, char *buf, gsize len);

/**
 * stk_chart_geom_format_tick:
 * @range: selects the style: 1D "10 AM" or "9:30 AM" (America/New_York
 *   hour, 12-hour clock, minutes only when non-zero); 1M/3M/6M "8 Oct";
 *   1Y "Oct". English abbreviations, independent of the locale.
 * @ms: timestamp, epoch ms: for 1D a UTC instant shown in New York time; for
 *   the daily ranges the UTC calendar date of @ms is shown (see the file
 *   comment).
 * @buf: (out caller-allocates): output buffer.
 * @len: size of @buf; STK_CHART_GEOM_LABEL_LEN always suffices.
 *
 * Axis-label text. Output is always NUL-terminated; an instant whose date-time in the label's
 * zone (UTC for the daily ranges, America/New_York for STK_RANGE_1D) lies outside the years
 * 1-9999 produces an em dash: for the daily ranges that is before 0001-01-01T00:00:00Z or
 * after 9999-12-31T23:59:59.999Z; for 1D the lower limit is the same wall-clock time in New
 * York, i.e. a few hours after 0001-01-01T00:00:00Z.
 *
 * Preconditions:
 *  - when @len > 0, @buf is non-NULL and writable for @len bytes.
 * Postconditions:
 *  - @len 0 writes nothing; otherwise @buf is NUL-terminated (truncated to
 *    @len - 1 bytes if needed). The month index into the name table is
 *    always 0..11 (GLib months are 1..12).
 *  - for STK_RANGE_1D with an unavailable New York time zone the em dash is
 *    written (this replaces a former g_assert()).
 * Thread affinity: any thread (pure; the shared zones are immutable).
 * Ownership: @buf is caller-owned and written; nothing is retained.
 *
 * Returns: void.
 */
void stk_chart_geom_format_tick(StkRange range, gint64 ms, char *buf, gsize len);

/**
 * stk_chart_geom_format_hover:
 * @range: 1D gives "Oct 8, 10:35 AM"; the other ranges "Oct 8, 2026".
 * @ms: sample timestamp, epoch ms (see the file comment): for 1D a true UTC
 *   instant shown in New York time; for the daily ranges 00:00:00 UTC of the
 *   trading date, shown as that UTC calendar date (never converted).
 * @buf: (out caller-allocates): output buffer.
 * @len: size of @buf; STK_CHART_GEOM_LABEL_LEN always suffices.
 *
 * Tooltip text for the sample under the pointer. Same termination and
 * out-of-range rules as stk_chart_geom_format_tick().
 *
 * Preconditions:
 *  - when @len > 0, @buf is non-NULL and writable for @len bytes.
 * Postconditions:
 *  - as stk_chart_geom_format_tick(): @len 0 writes nothing; otherwise @buf is
 *    NUL-terminated; an out-of-range instant, or STK_RANGE_1D with an
 *    unavailable New York time zone, gives the em dash (the latter replaces a
 *    former g_assert()).
 * Thread affinity: any thread (pure; the shared zones are immutable).
 * Ownership: @buf is caller-owned and written; nothing is retained.
 *
 * Returns: void.
 */
void stk_chart_geom_format_hover(StkRange range, gint64 ms, char *buf, gsize len);

G_END_DECLS

#endif /* STK_CHART_GEOM_H */
