/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-market.h - US equity regular-session calendar (NYSE / Nasdaq).
 *
 * Regular session: 09:30-16:00 America/New_York, Monday-Friday, except NYSE
 * full-day holidays; early-close days end at 13:00. The session is the
 * half-open interval [start, end): open at 09:30:00.000, closed at 16:00:00.000.
 * Pre-/after-market trading is NOT part of the regular session.
 *
 * Holidays and early closes are computed by rule (no tables), so the calendar
 * is valid for every year in [STK_MARKET_MIN_YEAR, STK_MARKET_MAX_YEAR]
 * (checked against the published NYSE calendar for 2026-2028; 2029-2030 follow
 * from the same rules). Unscheduled
 * closures (e.g. national days of mourning) cannot be derived by rule and are
 * not modelled.
 *
 * Time zone handling uses GTimeZone "America/New_York" (DST-aware). If the
 * system has no tz database, the equivalent POSIX rule
 * "EST5EDT,M3.2.0,M11.1.0" is used.
 *
 * The America/New_York GTimeZone is created once (thread-safe) and shared; it
 * is immutable. Otherwise functions are pure (no mutable global state). Date
 * arguments are calendar dates in New York local time. Timestamps are ms since
 * the Unix epoch, UTC.
 */
#ifndef STK_MARKET_H
#define STK_MARKET_H

#include <glib.h>

G_BEGIN_DECLS

/** First supported year (Juneteenth became an NYSE holiday in 2022). */
#define STK_MARKET_MIN_YEAR 2022
/** Last supported year. GLib models America/New_York DST rules only through
 *  year 2998; 2100 is a verified bound well inside that range. */
#define STK_MARKET_MAX_YEAR 2100

/**
 * stk_market_is_holiday:
 * @date: valid GDate with year in [STK_MARKET_MIN_YEAR, STK_MARKET_MAX_YEAR].
 *
 * Returns: TRUE if @date is a weekday on which NYSE is closed all day (after
 *   observed-day shifting). Weekends return FALSE (they are not "holidays"; use
 *   stk_market_session() to ask whether the market trades).
 *
 * Holiday rules (NYSE Rule 7.2): New Year's Day (Jan 1; Sunday -> Monday;
 * Saturday -> NOT observed), MLK Day (3rd Mon Jan), Washington's Birthday
 * (3rd Mon Feb), Good Friday, Memorial Day (last Mon May), Juneteenth (Jun 19),
 * Independence Day (Jul 4), Labor Day (1st Mon Sep), Thanksgiving (4th Thu
 * Nov), Christmas (Dec 25). Juneteenth/Independence/Christmas on Saturday ->
 * preceding Friday; on Sunday -> following Monday.
 */
gboolean stk_market_is_holiday(const GDate *date);

/**
 * stk_market_is_early_close:
 * @date: valid GDate with year in [STK_MARKET_MIN_YEAR, STK_MARKET_MAX_YEAR].
 *
 * Returns: TRUE if the market trades on @date but closes at 13:00 instead of
 *   16:00: the day after Thanksgiving; July 3 when it is Monday-Thursday
 *   (i.e. July 4 is Tuesday-Friday); December 24 when it is Monday-Thursday.
 */
gboolean stk_market_is_early_close(const GDate *date);

/**
 * stk_market_session:
 * @date: valid GDate (New York local date), year in
 *   [STK_MARKET_MIN_YEAR, STK_MARKET_MAX_YEAR].
 * @start_ms: (out) (optional): receives the 09:30 open as epoch ms (UTC).
 * @end_ms: (out) (optional): receives the 16:00 (or 13:00 early) close as
 *   epoch ms (UTC); always > *start_ms.
 *
 * Returns: TRUE if the market holds a regular session on @date. FALSE for
 *   weekends and full-day holidays; the outputs are then untouched.
 */
gboolean stk_market_session(const GDate *date, gint64 *start_ms, gint64 *end_ms);

/**
 * stk_market_local_date:
 * @ms: instant, epoch ms (UTC), whose New York year is in
 *   [STK_MARKET_MIN_YEAR, STK_MARKET_MAX_YEAR].
 * @out: (out): non-NULL GDate; set to the New York calendar date of @ms.
 *
 * Returns: TRUE on success; FALSE (and @out untouched) if @ms is outside the
 *   supported range.
 */
gboolean stk_market_local_date(gint64 ms, GDate *out);

/**
 * stk_market_is_open:
 * @now_ms: instant, epoch ms (UTC), within the supported range.
 *
 * Returns: TRUE iff start <= @now_ms < end of that New York day's session.
 *   FALSE outside the supported range (a critical is logged).
 */
gboolean stk_market_is_open(gint64 now_ms);

/**
 * stk_market_next_open_ms:
 * @now_ms: instant, epoch ms (UTC), within the supported range.
 *
 * Returns: the first session start strictly later than @now_ms (so while the
 *   market is open this is the next trading day's open), epoch ms UTC.
 *   Returns -1 WITHOUT logging if that session would fall after
 *   STK_MARKET_MAX_YEAR (end of the supported calendar). Returns -1 and logs a
 *   critical if @now_ms itself is outside the supported range.
 */
gint64 stk_market_next_open_ms(gint64 now_ms);

G_END_DECLS

#endif /* STK_MARKET_H */
