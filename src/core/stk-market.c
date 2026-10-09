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

/* Longest run of consecutive closed days is 5 (e.g. Thu holiday + Fri holiday
 * + weekend + Mon holiday); 14 is a safe bound for the next-open search. */
#define NEXT_OPEN_LOOKAHEAD_DAYS 14

/* Epoch seconds of 0001-01-01T00:00:00Z .. 9999-12-31T23:59:59Z, the range
 * GDateTime can represent; we additionally clamp by year below. */
#define MIN_UNIX_S G_GINT64_CONSTANT(-62135596800)
#define MAX_UNIX_S G_GINT64_CONSTANT(253402300799)

/* ------------------------------------------------------------------------ */
/* Time zone                                                                */
/* ------------------------------------------------------------------------ */

/* Shared, immutable America/New_York zone, created once on first use.
 * GTimeZone is immutable and its public API is thread-safe, so the single
 * instance is reused by every call (no per-call zoneinfo I/O) and never freed.
 * The POSIX rule is the fallback when the system has no tz database. */
static GTimeZone *
ny_tz(void)
{
  static gsize tz_once = 0;
  if (g_once_init_enter(&tz_once)) {
    GTimeZone *tz = g_time_zone_new_identifier("America/New_York");
    if (tz == NULL)
      tz = g_time_zone_new_identifier("EST5EDT,M3.2.0,M11.1.0");
    g_assert(tz != NULL);
    g_once_init_leave(&tz_once, (gsize)tz);
  }
  return (GTimeZone *)tz_once;
}

/* ------------------------------------------------------------------------ */
/* Julian-day calendar arithmetic                                           */
/* ------------------------------------------------------------------------ */

static guint32
julian_of(guint year, guint month, guint day)
{
  GDate d;
  g_date_clear(&d, 1);
  g_date_set_dmy(&d, (GDateDay)day, (GDateMonth)month, (GDateYear)year);
  return g_date_get_julian(&d);
}

static GDateWeekday
weekday_of(guint32 julian)
{
  GDate d;
  g_date_clear(&d, 1);
  g_date_set_julian(&d, julian);
  return g_date_get_weekday(&d);
}

/* n-th (1-based) @weekday of @month. */
static guint32
nth_weekday(guint year, guint month, GDateWeekday weekday, guint n)
{
  guint32 first = julian_of(year, month, 1);
  guint32 offset = ((guint32)weekday + 7 - (guint32)weekday_of(first)) % 7;
  return first + offset + 7 * (n - 1);
}

static guint32
last_weekday(guint year, guint month, GDateWeekday weekday)
{
  guint32 last = julian_of(year, month, g_date_get_days_in_month((GDateMonth)month, (GDateYear)year));
  return last - ((guint32)weekday_of(last) + 7 - (guint32)weekday) % 7;
}

/* Saturday -> Friday before, Sunday -> Monday after. */
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

static gboolean
date_supported(const GDate *date)
{
  return date != NULL && g_date_valid(date) && g_date_get_year(date) >= STK_MARKET_MIN_YEAR &&
         g_date_get_year(date) <= STK_MARKET_MAX_YEAR;
}

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

static gboolean
is_trading_day(const GDate *date)
{
  return g_date_get_weekday(date) <= G_DATE_FRIDAY && !holiday_impl(date);
}

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

static gint64
local_minute_to_ms(GTimeZone *tz, const GDate *date, gint minute_of_day)
{
  g_autoptr(GDateTime) dt = g_date_time_new(tz, g_date_get_year(date), g_date_get_month(date),
                                            g_date_get_day(date), minute_of_day / 60,
                                            minute_of_day % 60, 0.0);
  g_assert(dt != NULL);
  return g_date_time_to_unix(dt) * 1000;
}

static void
session_ms(GTimeZone *tz, const GDate *date, gint64 *start_ms, gint64 *end_ms)
{
  *start_ms = local_minute_to_ms(tz, date, OPEN_MINUTE);
  *end_ms = local_minute_to_ms(tz, date, early_close_impl(date) ? EARLY_CLOSE_MINUTE : CLOSE_MINUTE);
}

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

gboolean
stk_market_is_holiday(const GDate *date)
{
  g_return_val_if_fail(date_supported(date), FALSE);
  return g_date_get_weekday(date) <= G_DATE_FRIDAY && holiday_impl(date);
}

gboolean
stk_market_is_early_close(const GDate *date)
{
  g_return_val_if_fail(date_supported(date), FALSE);
  return early_close_impl(date);
}

gboolean
stk_market_session(const GDate *date, gint64 *start_ms, gint64 *end_ms)
{
  g_return_val_if_fail(date_supported(date), FALSE);
  if (!is_trading_day(date))
    return FALSE;

  GTimeZone *tz = ny_tz();
  gint64 start, end;
  session_ms(tz, date, &start, &end);
  if (start_ms != NULL)
    *start_ms = start;
  if (end_ms != NULL)
    *end_ms = end;
  return TRUE;
}

gboolean
stk_market_local_date(gint64 ms, GDate *out)
{
  g_return_val_if_fail(out != NULL, FALSE);
  GTimeZone *tz = ny_tz();
  GDate d;
  if (!local_date_in_tz(tz, ms, &d))
    return FALSE;
  *out = d;
  return TRUE;
}

gboolean
stk_market_is_open(gint64 now_ms)
{
  GTimeZone *tz = ny_tz();
  GDate today;
  if (!local_date_in_tz(tz, now_ms, &today))
    g_return_val_if_reached(FALSE);

  if (!is_trading_day(&today))
    return FALSE;
  gint64 start, end;
  session_ms(tz, &today, &start, &end);
  return now_ms >= start && now_ms < end;
}

gint64
stk_market_next_open_ms(gint64 now_ms)
{
  GTimeZone *tz = ny_tz();
  GDate day;
  if (!local_date_in_tz(tz, now_ms, &day))
    g_return_val_if_reached(-1);

  for (guint i = 0; i < NEXT_OPEN_LOOKAHEAD_DAYS; i++, g_date_add_days(&day, 1)) {
    if (g_date_get_year(&day) > STK_MARKET_MAX_YEAR)
      return -1; /* next session lies beyond the supported calendar: not an error */
    if (!is_trading_day(&day))
      continue;
    gint64 start, end;
    session_ms(tz, &day, &start, &end);
    if (start > now_ms)
      return start;
  }
  g_return_val_if_reached(-1);
}
