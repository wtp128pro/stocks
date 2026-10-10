/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * NYSE calendar sources (rules are encoded below, not copied as tables):
 *  - NYSE "Holidays & Trading Hours": https://www.nyse.com/trade/hours-calendars
 *    (published 2026-2028 calendars used for verification in test-core.c).
 *  - NYSE Rule 7.2 "Holidays" (observed-day rules: a holiday falling on a
 *    Saturday is observed the preceding Friday, one on a Sunday the following
 *    Monday; exception: when Jan 1 is a Saturday the market is open on
 *    Friday Dec 31).
 *  - Early closes (13:00 ET): the day after Thanksgiving; the day before
 *    Independence Day (July 3) when that is a Mon-Thu trading day, i.e. no
 *    early close when July 3 is itself the observed holiday (July 4 on
 *    Saturday) or when July 4 is on Sunday/Monday; Christmas Eve when it is a
 *    Mon-Thu trading day.
 *  - Good Friday = Easter Sunday - 2 days; Easter by the anonymous Gregorian
 *    algorithm (Meeus/Jones/Butcher).
 */
#include "stk-market.h"

#define OPEN_MINUTE (9 * 60 + 30)
#define CLOSE_MINUTE (16 * 60)
#define EARLY_CLOSE_MINUTE (13 * 60)

/* A closed run is a weekend plus at most an adjacent holiday, i.e. a few days;
 * 14 is a deliberately generous bound for the next-open search. */
#define NEXT_OPEN_LOOKAHEAD_DAYS 14

/* Epoch seconds of 0001-01-01T00:00:00Z .. 9999-12-31T23:59:59Z, the range
 * GDateTime can represent; we additionally clamp by year below. */
#define MIN_UNIX_S G_GINT64_CONSTANT(-62135596800)
#define MAX_UNIX_S G_GINT64_CONSTANT(253402300799)

/* ------------------------------------------------------------------------ */
/* Time zone                                                                */
/* ------------------------------------------------------------------------ */

/**
 * ny_tz:
 *
 * Shared, immutable America/New_York zone, created once on first use.
 * GTimeZone is immutable and its public API is thread-safe, so the single
 * instance is reused by every call (no per-call zoneinfo I/O) and never freed.
 * The POSIX rule is the fallback when the system has no tz database. If both
 * lookups fail, NULL is returned (and remembered); no substitute zone such as
 * UTC is used, because it would silently shift every session by 4-5 hours.
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - returns the same value on every call: the zone, owned by this file for
 *    the process lifetime, or NULL if the zone could not be created. Callers
 *    MUST check for NULL and report their normal failure value.
 * Thread affinity: any thread (one-time initialisation via g_once_init_enter).
 * Ownership: (transfer none) - the caller must not unref the zone.
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

/* ------------------------------------------------------------------------ */
/* Julian-day calendar arithmetic                                           */
/* ------------------------------------------------------------------------ */

/**
 * julian_of:
 * @year: calendar year, 1..G_MAXUINT16.
 * @month: month, 1..12.
 * @day: day of month, valid for @month and @year.
 *
 * Julian day number of a Gregorian date.
 *
 * Preconditions:
 *  - the arguments form a valid date whose parts fit GDateYear/GDateMonth/
 *    GDateDay (so the narrowing casts cannot truncate). The complete set of
 *    call sites, all of which guarantee it: @year always comes from
 *    g_date_get_year() of a date inside [STK_MARKET_MIN_YEAR,
 *    STK_MARKET_MAX_YEAR] (checked by date_supported() or local_date_in_tz()),
 *    directly or via nth_weekday()/last_weekday()/easter_sunday()/
 *    holiday_impl(); @month/@day are (a) Jan 1, Jun 19, Jul 4 and Dec 25
 *    (holiday_impl()), (b) day 1 of a month 1..12 (nth_weekday()), (c) the
 *    last day of a month 1..12, taken from g_date_get_days_in_month() for that
 *    month and year, 28..31 (last_weekday(), called with May 31 for Memorial
 *    Day), and (d) the Easter month/day, which is always between Mar 22 and
 *    Apr 25 (easter_sunday()).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: the Julian day (>= 1) of the date.
 */
static guint32
julian_of(guint year, guint month, guint day)
{
  GDate d;
  g_date_clear(&d, 1);
  g_date_set_dmy(&d, (GDateDay)day, (GDateMonth)month, (GDateYear)year);
  return g_date_get_julian(&d);
}

/**
 * weekday_of:
 * @julian: Julian day number.
 *
 * Preconditions:
 *  - @julian is a valid Julian day (not G_DATE_BAD_JULIAN). The call sites
 *    pass only values derived from valid dates of the supported years.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: the weekday G_DATE_MONDAY..G_DATE_SUNDAY (1..7).
 */
static GDateWeekday
weekday_of(guint32 julian)
{
  GDate d;
  g_date_clear(&d, 1);
  g_date_set_julian(&d, julian);
  return g_date_get_weekday(&d);
}

/**
 * nth_weekday:
 * @year: calendar year (see julian_of()).
 * @month: month 1..12.
 * @weekday: G_DATE_MONDAY..G_DATE_SUNDAY.
 * @n: 1-based occurrence.
 *
 * Finds the n-th (1-based) @weekday of @month.
 *
 * Preconditions:
 *  - the arguments satisfy julian_of(); @n >= 1 (@n == 0 would make `n - 1`
 *    wrap) and the month has at least @n such weekdays. The call sites pass
 *    the constants n = 1, 3 or 4, which every month has.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: the Julian day of the n-th @weekday.
 */
static guint32
nth_weekday(guint year, guint month, GDateWeekday weekday, guint n)
{
  guint32 first = julian_of(year, month, 1);
  guint32 offset = ((guint32)weekday + 7 - (guint32)weekday_of(first)) % 7;
  return first + offset + 7 * (n - 1);
}

/**
 * last_weekday:
 * @year: calendar year (see julian_of()).
 * @month: month 1..12.
 * @weekday: G_DATE_MONDAY..G_DATE_SUNDAY.
 *
 * Finds the last @weekday of @month.
 *
 * Preconditions:
 *  - the arguments satisfy julian_of(); the only call site is holiday_impl()
 *    with May and Monday (Memorial Day). Its julian_of() argument is the
 *    month's last day from g_date_get_days_in_month(), which is valid for the
 *    month and year.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: the Julian day of the last @weekday of the month.
 */
static guint32
last_weekday(guint year, guint month, GDateWeekday weekday)
{
  guint32 last = julian_of(year, month, g_date_get_days_in_month((GDateMonth)month, (GDateYear)year));
  return last - ((guint32)weekday_of(last) + 7 - (guint32)weekday) % 7;
}

/**
 * observed:
 * @julian: Julian day of a fixed-date holiday.
 *
 * Applies the NYSE observed-day rule: Saturday -> Friday before, Sunday ->
 * Monday after.
 *
 * Preconditions:
 *  - @julian is a valid Julian day of a supported year (so julian +- 1 cannot
 *    wrap; see weekday_of()).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: the observed Julian day.
 */
static guint32
observed(guint32 julian)
{
  switch (weekday_of(julian)) {
  case G_DATE_SATURDAY:
    return julian - 1;
  case G_DATE_SUNDAY:
    return julian + 1;
  default:
    return julian;
  }
}

/**
 * easter_sunday:
 * @year: Gregorian year (the call site passes a supported year, 2022..2100).
 *
 * Easter Sunday by the anonymous Gregorian algorithm (Meeus/Jones/Butcher).
 * The unsigned arithmetic cannot wrap for any year >= 0: in `19*a + b - d - g
 * + 15` the term b - d - g is >= 0 because d = b/4 and g <= (b+1)/3; in
 * `32 + 2*e + 2*i - h - k` h <= 29 and k <= 3 so the sum is >= 0; and m <= 1
 * with h + l >= 7 whenever m == 1, so `h + l - 7*m + 114` is positive.
 *
 * Preconditions:
 *  - @year satisfies julian_of() (call site: a year of a date inside
 *    [STK_MARKET_MIN_YEAR, STK_MARKET_MAX_YEAR]).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: the Julian day of Easter Sunday of @year.
 */
static guint32
easter_sunday(guint year)
{
  guint a = year % 19;
  guint b = year / 100;
  guint c = year % 100;
  guint d = b / 4;
  guint e = b % 4;
  guint f = (b + 8) / 25;
  guint g = (b - f + 1) / 3;
  guint h = (19 * a + b - d - g + 15) % 30;
  guint i = c / 4;
  guint k = c % 4;
  guint l = (32 + 2 * e + 2 * i - h - k) % 7;
  guint m = (a + 11 * h + 22 * l) / 451;
  guint month = (h + l - 7 * m + 114) / 31;
  guint day = (h + l - 7 * m + 114) % 31 + 1;
  return julian_of(year, month, day);
}

/* ------------------------------------------------------------------------ */
/* Calendar predicates                                                      */
/* ------------------------------------------------------------------------ */

/**
 * date_supported:
 * @date: (nullable): date to test.
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: @date is borrowed; nothing is retained.
 *
 * Returns: TRUE iff @date is non-NULL, valid and its year lies in
 *   [STK_MARKET_MIN_YEAR, STK_MARKET_MAX_YEAR].
 */
static gboolean
date_supported(const GDate *date)
{
  return date != NULL && g_date_valid(date) && g_date_get_year(date) >= STK_MARKET_MIN_YEAR &&
         g_date_get_year(date) <= STK_MARKET_MAX_YEAR;
}

/**
 * holiday_impl:
 * @date: valid date.
 *
 * Tests @date against the NYSE full-day holiday rules (weekends are not
 * excluded here).
 *
 * Preconditions:
 *  - @date is non-NULL, valid and in the supported years. The call sites
 *    guarantee it: date_supported() (stk_market_is_holiday(),
 *    stk_market_is_early_close(), stk_market_session()), local_date_in_tz()
 *    (stk_market_is_open(), stk_market_next_open_ms()) or, in the next-open
 *    loop, a date that only moves forward from such a date and is tested
 *    against STK_MARKET_MAX_YEAR before use. Hence the helpers' preconditions
 *    hold and easter_sunday() >= 80, so `easter - 2` cannot wrap.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: @date is borrowed; nothing is retained.
 *
 * Returns: TRUE iff @date is a holiday date after observed-day shifting.
 */
static gboolean
holiday_impl(const GDate *date)
{
  guint year = g_date_get_year(date);
  guint32 j = g_date_get_julian(date);

  /* New Year's Day: Saturday is not observed (NYSE Rule 7.2). */
  guint32 new_year = julian_of(year, 1, 1);
  if (weekday_of(new_year) != G_DATE_SATURDAY && j == observed(new_year))
    return TRUE;

  return j == nth_weekday(year, 1, G_DATE_MONDAY, 3) ||  /* MLK Day */
         j == nth_weekday(year, 2, G_DATE_MONDAY, 3) ||  /* Washington's Birthday */
         j == easter_sunday(year) - 2 ||                 /* Good Friday */
         j == last_weekday(year, 5, G_DATE_MONDAY) ||    /* Memorial Day */
         j == observed(julian_of(year, 6, 19)) ||        /* Juneteenth */
         j == observed(julian_of(year, 7, 4)) ||         /* Independence Day */
         j == nth_weekday(year, 9, G_DATE_MONDAY, 1) ||  /* Labor Day */
         j == nth_weekday(year, 11, G_DATE_THURSDAY, 4) || /* Thanksgiving */
         j == observed(julian_of(year, 12, 25));         /* Christmas */
}

/**
 * is_trading_day:
 * @date: valid date.
 *
 * Preconditions:
 *  - @date is non-NULL, valid and in the supported years (see holiday_impl()).
 *    Being valid, g_date_get_weekday() returns 1..7 and never
 *    G_DATE_BAD_WEEKDAY (0), so the test `<= G_DATE_FRIDAY` means Monday..Friday.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: @date is borrowed; nothing is retained.
 *
 * Returns: TRUE iff @date is a weekday and not a holiday.
 */
static gboolean
is_trading_day(const GDate *date)
{
  return g_date_get_weekday(date) <= G_DATE_FRIDAY && !holiday_impl(date);
}

/**
 * early_close_impl:
 * @date: valid date.
 *
 * Preconditions:
 *  - @date is non-NULL, valid and in the supported years (see holiday_impl()).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: @date is borrowed; nothing is retained.
 *
 * Returns: TRUE iff @date is a trading day that closes at 13:00.
 */
static gboolean
early_close_impl(const GDate *date)
{
  if (!is_trading_day(date))
    return FALSE;

  GDateWeekday wd = g_date_get_weekday(date);
  GDateMonth month = g_date_get_month(date);
  GDateDay day = g_date_get_day(date);
  guint year = g_date_get_year(date);

  if (g_date_get_julian(date) == nth_weekday(year, 11, G_DATE_THURSDAY, 4) + 1)
    return TRUE; /* day after Thanksgiving */
  if (wd <= G_DATE_THURSDAY && ((month == G_DATE_JULY && day == 3) ||
                                (month == G_DATE_DECEMBER && day == 24)))
    return TRUE;
  return FALSE;
}

/* ------------------------------------------------------------------------ */
/* Instants                                                                 */
/* ------------------------------------------------------------------------ */

/**
 * local_minute_to_ms:
 * @tz: (not nullable): time zone of the wall-clock time.
 * @date: valid date in @tz, in the supported years.
 * @minute_of_day: minutes since local midnight, 0..1439 (the call sites pass
 *   OPEN_MINUTE, CLOSE_MINUTE or EARLY_CLOSE_MINUTE).
 * @out_ms: (out): receives the instant as epoch ms (UTC).
 *
 * Converts a local wall-clock time to epoch milliseconds. The former
 * assertion on GDateTime creation is a checked failure, and the
 * seconds-to-milliseconds multiplication is checked for overflow (it cannot
 * overflow for supported years, whose epoch seconds are below 2^33).
 *
 * Preconditions:
 *  - @tz and @out_ms are non-NULL; @date is non-NULL, valid and in the
 *    supported years (call sites: session_ms(), whose own callers pass a date
 *    from date_supported(), local_date_in_tz() or the next-open loop).
 * Postconditions:
 *  - on TRUE *@out_ms is set; on FALSE *@out_ms is untouched.
 * Thread affinity: any thread (pure).
 * Ownership: @tz and @date are borrowed; @out_ms is caller-owned and written;
 *   nothing is retained.
 *
 * Returns: TRUE on success; FALSE if the time cannot be represented.
 */
static gboolean
local_minute_to_ms(GTimeZone *tz, const GDate *date, gint minute_of_day, gint64 *out_ms)
{
  g_autoptr(GDateTime) dt = g_date_time_new(tz, g_date_get_year(date), g_date_get_month(date),
                                            g_date_get_day(date), minute_of_day / 60,
                                            minute_of_day % 60, 0.0);
  if (dt == NULL)
    return FALSE;
  gint64 secs = g_date_time_to_unix(dt);
  if (secs > G_MAXINT64 / 1000 || secs < G_MININT64 / 1000)
    return FALSE;
  *out_ms = secs * 1000;
  return TRUE;
}

/**
 * session_ms:
 * @tz: (not nullable): the New York zone.
 * @date: valid trading day.
 * @start_ms: (out): receives the open instant (epoch ms UTC).
 * @end_ms: (out): receives the close instant (epoch ms UTC).
 *
 * Preconditions:
 *  - @tz, @start_ms and @end_ms are non-NULL; @date is non-NULL, valid and in
 *    the supported years.
 * Postconditions:
 *  - on TRUE both outputs are set (end > start); on FALSE they are unspecified
 *    and must be ignored.
 * Thread affinity: any thread (pure).
 * Ownership: @tz and @date are borrowed; @start_ms and @end_ms are
 *   caller-owned and written; nothing is retained.
 *
 * Returns: TRUE on success; FALSE if either instant could not be computed.
 */
static gboolean
session_ms(GTimeZone *tz, const GDate *date, gint64 *start_ms, gint64 *end_ms)
{
  return local_minute_to_ms(tz, date, OPEN_MINUTE, start_ms) &&
         local_minute_to_ms(tz, date, early_close_impl(date) ? EARLY_CLOSE_MINUTE : CLOSE_MINUTE,
                            end_ms);
}

/**
 * local_date_in_tz:
 * @tz: (not nullable): time zone to convert to.
 * @ms: instant, epoch ms (UTC).
 * @out: (out): receives the local calendar date on success.
 *
 * Preconditions:
 *  - @tz and @out are non-NULL.
 * Postconditions:
 *  - on TRUE *@out is a valid date whose year lies in
 *    [STK_MARKET_MIN_YEAR, STK_MARKET_MAX_YEAR]; on FALSE *@out is untouched
 *    and must be ignored.
 * Thread affinity: any thread (pure).
 * Ownership: @tz is borrowed; @out is caller-owned and written; nothing is
 *   retained.
 *
 * Returns: TRUE on success; FALSE if @ms is outside the representable or
 *   supported range.
 */
static gboolean
local_date_in_tz(GTimeZone *tz, gint64 ms, GDate *out)
{
  if (ms < MIN_UNIX_S * 1000 || ms > MAX_UNIX_S * 1000 + 999)
    return FALSE;
  gint64 secs = ms >= 0 ? ms / 1000 : -((-ms + 999) / 1000); /* floor division */

  g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc(secs);
  if (utc == NULL)
    return FALSE;
  g_autoptr(GDateTime) local = g_date_time_to_timezone(utc, tz);
  if (local == NULL)
    return FALSE;

  /* The year check makes the (GDateYear) cast safe; GDateTime month (1..12) and
   * day (1..31) always fit GDateMonth/GDateDay. */
  gint year = g_date_time_get_year(local);
  if (year < STK_MARKET_MIN_YEAR || year > STK_MARKET_MAX_YEAR)
    return FALSE;

  g_date_clear(out, 1);
  g_date_set_dmy(out, (GDateDay)g_date_time_get_day_of_month(local),
                 (GDateMonth)g_date_time_get_month(local), (GDateYear)year);
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

/* Documented in stk-market.h. */
gboolean
stk_market_is_holiday(const GDate *date)
{
  g_return_val_if_fail(date_supported(date), FALSE);
  return g_date_get_weekday(date) <= G_DATE_FRIDAY && holiday_impl(date);
}

/* Documented in stk-market.h. */
gboolean
stk_market_is_early_close(const GDate *date)
{
  g_return_val_if_fail(date_supported(date), FALSE);
  return early_close_impl(date);
}

/* Documented in stk-market.h. */
gboolean
stk_market_session(const GDate *date, gint64 *start_ms, gint64 *end_ms)
{
  g_return_val_if_fail(date_supported(date), FALSE);
  if (!is_trading_day(date))
    return FALSE;

  GTimeZone *tz = ny_tz();
  if (tz == NULL)
    return FALSE;
  gint64 start, end;
  if (!session_ms(tz, date, &start, &end))
    return FALSE;
  if (start_ms != NULL)
    *start_ms = start;
  if (end_ms != NULL)
    *end_ms = end;
  return TRUE;
}

/* Documented in stk-market.h. */
gboolean
stk_market_local_date(gint64 ms, GDate *out)
{
  g_return_val_if_fail(out != NULL, FALSE);
  GTimeZone *tz = ny_tz();
  if (tz == NULL)
    return FALSE;
  GDate d;
  if (!local_date_in_tz(tz, ms, &d))
    return FALSE;
  *out = d;
  return TRUE;
}

/* Documented in stk-market.h. */
gboolean
stk_market_is_open(gint64 now_ms)
{
  GTimeZone *tz = ny_tz();
  if (tz == NULL)
    return FALSE;
  GDate today;
  if (!local_date_in_tz(tz, now_ms, &today))
    g_return_val_if_reached(FALSE);

  if (!is_trading_day(&today))
    return FALSE;
  gint64 start, end;
  if (!session_ms(tz, &today, &start, &end))
    return FALSE;
  return now_ms >= start && now_ms < end;
}

/* Documented in stk-market.h. */
gint64
stk_market_next_open_ms(gint64 now_ms)
{
  GTimeZone *tz = ny_tz();
  if (tz == NULL)
    return -1;
  GDate day;
  if (!local_date_in_tz(tz, now_ms, &day))
    g_return_val_if_reached(-1);

  for (guint i = 0; i < NEXT_OPEN_LOOKAHEAD_DAYS; i++, g_date_add_days(&day, 1)) {
    if (g_date_get_year(&day) > STK_MARKET_MAX_YEAR)
      return -1; /* next session lies beyond the supported calendar: not an error */
    if (!is_trading_day(&day))
      continue;
    gint64 start, end;
    if (!session_ms(tz, &day, &start, &end))
      return -1;
    if (start > now_ms)
      return start;
  }
  g_return_val_if_reached(-1);
}
