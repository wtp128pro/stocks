/*
 * test-scheduler.c - offline unit tests of StkScheduler (src/ui/stk-scheduler.h).
 *
 * Pure logic: no display, no network, no real clock. Every test drives the
 * scheduler with a fake StkSchedTime. The fixture checks, after every call,
 * the invariants that must hold at all times: at most one outstanding request
 * per symbol (cancelled ones included), at most STK_SCHED_MAX_INFLIGHT in
 * total, unique non-zero ids, CANCEL only for outstanding requests, and no
 * 1D series for a fund.
 *
 * The /scheduler/regress/ tests pin the defects found by the panel review:
 * freshness is per job (a job that was cancelled, dropped or failed is never
 * skipped because the symbol's quote is recent), failed jobs of any symbol
 * are retried by the next round that can (periodic while the market is open,
 * else the next event round), and the stale marks change only where
 * stk-scheduler.h says they do (checked over randomised input sequences).
 */
#include <glib.h>
#include <string.h>

#include "stk-market.h"
#include "stk-scheduler.h"

#define SEC (G_USEC_PER_SEC)

/* ------------------------------------------------------------------------ */
/* Fixture                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
  guint64 id;
  char symbol[STK_SCHED_SYMBOL_BYTES];
  gboolean cancelled;
} Outstanding;

typedef struct {
  StkScheduler *s;
  gint64 mono_us;
  gint64 wall_ms;
  GArray *all;  /* StkSchedAction: everything emitted so far     */
  GArray *last; /* StkSchedAction: emitted by the latest call    */
  GArray *out;  /* Outstanding                                   */
  GArray *oks;  /* StkSchedAction: requests that completed OK    */
} Fx;

static const StkSchedSymbol k_three[] = {
    {"AAPL", STK_KIND_STOCK}, {"SPY", STK_KIND_ETF}, {"FXAIX", STK_KIND_FUND}};

static gint64
wall_utc(int y, int mo, int d, int h, int mi, int sec)
{
  g_autoptr(GDateTime) dt = g_date_time_new_utc(y, mo, d, h, mi, sec);
  g_assert_nonnull(dt);
  return g_date_time_to_unix(dt) * 1000;
}

/* Thursday 2026-10-08 10:00 ET (EDT): market open until 16:00 ET. */
#define WALL_OPEN wall_utc(2026, 10, 8, 14, 0, 0)
/* Thursday 2026-10-08 18:00 ET: closed. */
#define WALL_AFTER_HOURS wall_utc(2026, 10, 8, 22, 0, 0)

static StkSchedTime
now_of(const Fx *fx)
{
  StkSchedTime t = {fx->mono_us, fx->wall_ms};
  return t;
}

static void
collect(Fx *fx)
{
  g_clear_pointer(&fx->last, g_array_unref);
  fx->last = stk_scheduler_take_actions(fx->s);
  if (fx->last != NULL) {
    g_assert_cmpuint(fx->last->len, >, 0);
    for (guint i = 0; i < fx->last->len; i++) {
      StkSchedAction a = g_array_index(fx->last, StkSchedAction, i);
      g_array_append_val(fx->all, a);
      if (a.type == STK_SCHED_CANCEL) {
        gboolean found = FALSE;
        for (guint j = 0; j < fx->out->len; j++) {
          Outstanding *o = &g_array_index(fx->out, Outstanding, j);
          if (o->id == a.id) {
            found = TRUE;
            g_assert_false(o->cancelled); /* never cancelled twice */
            o->cancelled = TRUE;
          }
        }
        g_assert_true(found);
        g_assert_cmpstr(a.symbol, ==, "");
        continue;
      }
      g_assert_cmpuint(a.id, !=, 0);
      Outstanding o;
      memset(&o, 0, sizeof o);
      o.id = a.id;
      g_strlcpy(o.symbol, a.symbol, sizeof o.symbol);
      for (guint j = 0; j < fx->out->len; j++) {
        const Outstanding *p = &g_array_index(fx->out, Outstanding, j);
        g_assert_cmpuint(p->id, !=, a.id);
        g_assert_cmpstr(p->symbol, !=, a.symbol); /* one request per symbol */
      }
      for (guint j = 0; j + 1 < fx->all->len; j++)
        g_assert_cmpuint(g_array_index(fx->all, StkSchedAction, j).id, !=, a.id);
      if (a.type == STK_SCHED_FETCH_SERIES && a.kind == STK_KIND_FUND)
        g_assert_cmpint(a.range, !=, STK_RANGE_1D);
      g_array_append_val(fx->out, o);
      g_assert_cmpuint(fx->out->len, <=, STK_SCHED_MAX_INFLIGHT);
    }
  }
  g_assert_cmpuint(stk_scheduler_n_inflight(fx->s), ==, fx->out->len);
}

static Fx *
fx_new(gint64 wall_ms)
{
  Fx *fx = g_new0(Fx, 1);
  fx->s = stk_scheduler_new();
  fx->mono_us = 1000 * SEC;
  fx->wall_ms = wall_ms;
  fx->all = g_array_new(FALSE, FALSE, sizeof(StkSchedAction));
  fx->out = g_array_new(FALSE, FALSE, sizeof(Outstanding));
  fx->oks = g_array_new(FALSE, FALSE, sizeof(StkSchedAction));
  return fx;
}

static void
fx_free(Fx *fx)
{
  g_clear_pointer(&fx->last, g_array_unref);
  g_array_unref(fx->all);
  g_array_unref(fx->out);
  g_array_unref(fx->oks);
  stk_scheduler_free(fx->s);
  g_free(fx);
}

static void
fx_advance(Fx *fx, gint64 seconds)
{
  fx->mono_us += seconds * SEC;
  /* The clock-range tests start at the extremes of gint64. */
  if (fx->wall_ms > G_MAXINT64 - seconds * 1000)
    fx->wall_ms = G_MAXINT64;
  else
    fx->wall_ms += seconds * 1000;
}

static void
fx_symbols(Fx *fx, const StkSchedSymbol *syms, gsize n)
{
  stk_scheduler_set_symbols(fx->s, now_of(fx), syms, n);
  collect(fx);
}

static void
fx_select(Fx *fx, const char *symbol, StkRange range)
{
  stk_scheduler_set_selection(fx->s, now_of(fx), symbol, range);
  collect(fx);
}

static void
fx_visible(Fx *fx, gboolean v)
{
  stk_scheduler_set_visible(fx->s, now_of(fx), v);
  collect(fx);
}

static void
fx_online(Fx *fx, gboolean v)
{
  stk_scheduler_set_online(fx->s, now_of(fx), v);
  collect(fx);
}

static void
fx_focus(Fx *fx)
{
  stk_scheduler_focus_regained(fx->s, now_of(fx));
  collect(fx);
}

static void
fx_tick(Fx *fx)
{
  stk_scheduler_tick(fx->s, now_of(fx));
  collect(fx);
}

static void
fx_done(Fx *fx, guint64 id, StkSchedResult r)
{
  stk_scheduler_request_done(fx->s, now_of(fx), id, r);
  for (guint j = 0; j < fx->out->len; j++) {
    if (g_array_index(fx->out, Outstanding, j).id == id) {
      g_array_remove_index(fx->out, j);
      break;
    }
  }
  if (r == STK_SCHED_RESULT_OK) {
    for (guint j = 0; j < fx->all->len; j++) {
      const StkSchedAction *a = &g_array_index(fx->all, StkSchedAction, j);
      if (a->id == id && a->type != STK_SCHED_CANCEL) {
        g_array_append_val(fx->oks, *a);
        break;
      }
    }
  }
  collect(fx);
}

/* Completes everything outstanding (and whatever that starts) with @r. */
static void
fx_finish_all(Fx *fx, StkSchedResult r)
{
  for (guint guard = 0; fx->out->len > 0; guard++) {
    g_assert_cmpuint(guard, <, 100000);
    fx_done(fx, g_array_index(fx->out, Outstanding, 0).id, r);
  }
}

static guint64
out_id(const Fx *fx, const char *symbol)
{
  for (guint j = 0; j < fx->out->len; j++) {
    const Outstanding *o = &g_array_index(fx->out, Outstanding, j);
    if (strcmp(o->symbol, symbol) == 0)
      return o->id;
  }
  return 0;
}

/* Counts actions of @type in @arr from index @from on; @symbol NULL = any;
 * @range -1 = any. */
static guint
count_from(const GArray *arr, guint from, StkSchedActionType type, const char *symbol, int range)
{
  guint n = 0;
  if (arr == NULL)
    return 0;
  for (guint i = from; i < arr->len; i++) {
    const StkSchedAction *a = &g_array_index(arr, StkSchedAction, i);
    if (a->type != type)
      continue;
    if (symbol != NULL && strcmp(a->symbol, symbol) != 0)
      continue;
    if (range >= 0 && (int)a->range != range)
      continue;
    n++;
  }
  return n;
}

static guint
count_in(const GArray *arr, StkSchedActionType type, const char *symbol, int range)
{
  return count_from(arr, 0, type, symbol, range);
}

#define COUNT_ALL(fx, type, sym, range) count_in((fx)->all, type, sym, range)
#define COUNT_LAST(fx, type, sym, range) count_in((fx)->last, type, sym, range)
/* Requests of @type issued since @mark (a value of (fx)->all->len). */
#define COUNT_SINCE(fx, mark, type, sym, range) \
  count_from((fx)->all, mark, type, sym, range)
/* Requests of @type that completed OK so far. */
#define COUNT_OK(fx, type, sym, range) count_in((fx)->oks, type, sym, range)
#define NO_ACTIONS(fx) g_assert_null((fx)->last)

/* Visible + online + three symbols, started at @wall_ms, nothing completed. */
static Fx *
fx_started(gint64 wall_ms)
{
  Fx *fx = fx_new(wall_ms);
  fx_symbols(fx, k_three, G_N_ELEMENTS(k_three));
  fx_online(fx, TRUE);
  fx_visible(fx, TRUE);
  return fx;
}

/* ------------------------------------------------------------------------ */
/* Tests                                                                    */
/* ------------------------------------------------------------------------ */

static void
test_idle_has_no_actions_no_timer(void)
{
  Fx *fx = fx_new(WALL_OPEN);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
  fx_symbols(fx, k_three, G_N_ELEMENTS(k_three));
  NO_ACTIONS(fx);
  fx_visible(fx, TRUE); /* visible but offline */
  NO_ACTIONS(fx);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
  fx_advance(fx, 600);
  fx_tick(fx);
  NO_ACTIONS(fx);
  fx_focus(fx);
  NO_ACTIONS(fx);
  fx_online(fx, TRUE); /* now active: exactly one round */
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 3);
  fx_free(fx);

  stk_scheduler_free(NULL); /* allowed */
}

static void
test_start_round_once_per_job(void)
{
  Fx *fx = fx_started(WALL_OPEN);
  /* First wave: the three quotes (one request per symbol). */
  g_assert_cmpuint(fx->last->len, ==, 3);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 3);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, fx->mono_us + STK_SCHED_PERIOD_US);

  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  const char *syms[] = {"AAPL", "SPY", "FXAIX"};
  for (guint i = 0; i < 3; i++) {
    g_assert_cmpuint(COUNT_ALL(fx, STK_SCHED_FETCH_QUOTE, syms[i], -1), ==, 1);
    g_assert_cmpuint(COUNT_ALL(fx, STK_SCHED_FETCH_SERIES, syms[i], STK_SCHED_SPARK_RANGE), ==, 1);
    g_assert_cmpuint(COUNT_ALL(fx, STK_SCHED_FETCH_SERIES, syms[i], -1), ==, 1);
    g_assert_false(stk_scheduler_is_stale(fx->s, syms[i]));
  }
  g_assert_cmpuint(fx->all->len, ==, 6);
  g_assert_cmpuint(stk_scheduler_n_inflight(fx->s), ==, 0);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, fx->mono_us + STK_SCHED_PERIOD_US);

  /* Showing again while already visible, or going online again: nothing. */
  fx_visible(fx, TRUE);
  fx_online(fx, TRUE);
  NO_ACTIONS(fx);
  fx_free(fx);
}

static void
test_periodic_round_open_market(void)
{
  Fx *fx = fx_started(WALL_OPEN);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  gint64 deadline = stk_scheduler_next_wakeup_us(fx->s);
  guint before = fx->all->len;

  fx_advance(fx, 30); /* early tick: nothing happens, deadline unchanged */
  fx_tick(fx);
  NO_ACTIONS(fx);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, deadline);

  fx_advance(fx, 30);
  g_assert_cmpint(fx->mono_us, ==, deadline);
  fx_tick(fx);
  /* Quotes of the stock and the ETF only: funds publish once a day. */
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 2);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, "FXAIX", -1), ==, 0);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_SERIES, NULL, -1), ==, 0);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, deadline + STK_SCHED_PERIOD_US);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_cmpuint(fx->all->len, ==, before + 2);

  /* Woken very late (e.g. a long stall): one round, next one a period later. */
  fx_advance(fx, 3600);
  fx_tick(fx);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 2);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, fx->mono_us + STK_SCHED_PERIOD_US);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  fx_tick(fx); /* the same instant again: no catch-up round */
  NO_ACTIONS(fx);
  fx_free(fx);
}

static void
test_periodic_skips_fund_only_list(void)
{
  Fx *fx = fx_new(WALL_OPEN);
  const StkSchedSymbol funds[] = {{"FXAIX", STK_KIND_FUND}, {"FSKAX", STK_KIND_FUND}};
  fx_symbols(fx, funds, G_N_ELEMENTS(funds));
  fx_online(fx, TRUE);
  fx_visible(fx, TRUE);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 2);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1); /* nothing periodic to do */
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  fx_advance(fx, 600);
  fx_tick(fx);
  NO_ACTIONS(fx);
  fx_free(fx);
}

static void
test_no_timer_when_market_closed(void)
{
  struct {
    const char *what;
    gint64 wall_ms;
  } cases[] = {
      {"after hours", WALL_AFTER_HOURS},
      {"before the open", wall_utc(2026, 10, 8, 13, 29, 59) + 999},
      {"at the close", wall_utc(2026, 10, 8, 20, 0, 0)},
      {"saturday", wall_utc(2026, 10, 10, 15, 0, 0)},
      {"thanksgiving", wall_utc(2026, 11, 26, 15, 0, 0)},
      {"before the supported calendar", 0},
      {"after the supported calendar", wall_utc(2101, 6, 1, 15, 0, 0)},
      {"far future", G_MAXINT64},
      {"far past", G_MININT64},
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_test_message("%s", cases[i].what);
    Fx *fx = fx_started(cases[i].wall_ms);
    /* The event round still happens: a closed market shows the latest close. */
    g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 3);
    g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
    fx_finish_all(fx, STK_SCHED_RESULT_OK);
    g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
    fx_advance(fx, 600);
    fx_tick(fx);
    NO_ACTIONS(fx);
    fx_free(fx);
  }
}

static void
test_market_open_boundaries(void)
{
  gint64 open_ms = wall_utc(2026, 10, 8, 13, 30, 0);
  gint64 close_ms = wall_utc(2026, 10, 8, 20, 0, 0);
  gint64 walls[] = {open_ms, close_ms - 1};
  for (guint i = 0; i < G_N_ELEMENTS(walls); i++) {
    Fx *fx = fx_started(walls[i]);
    g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, fx->mono_us + STK_SCHED_PERIOD_US);
    fx_free(fx);
  }
}

static void
test_market_closes_while_ticking(void)
{
  /* 15:59:30 ET: the first period ends after the 16:00 close. */
  Fx *fx = fx_started(wall_utc(2026, 10, 8, 19, 59, 30));
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, fx->mono_us + STK_SCHED_PERIOD_US);
  fx_advance(fx, 60);
  fx_tick(fx);
  NO_ACTIONS(fx); /* closed by now: no round and no timer */
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
  fx_free(fx);
}

static void
test_no_timer_when_hidden_or_offline(void)
{
  Fx *fx = fx_started(WALL_OPEN);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), !=, -1);

  fx_visible(fx, FALSE);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
  fx_advance(fx, 600);
  fx_tick(fx);
  NO_ACTIONS(fx);
  fx_focus(fx); /* a focus event while hidden means nothing */
  NO_ACTIONS(fx);

  fx_visible(fx, TRUE); /* show: one round, timer back */
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 3);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, fx->mono_us + STK_SCHED_PERIOD_US);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);

  fx_online(fx, FALSE);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
  fx_advance(fx, 600);
  fx_tick(fx);
  NO_ACTIONS(fx);
  fx_focus(fx);
  NO_ACTIONS(fx);
  fx_online(fx, TRUE); /* reconnect: one round */
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 3);
  fx_free(fx);
}

static void
test_focus_regain_refreshes_once(void)
{
  Fx *fx = fx_started(WALL_AFTER_HOURS); /* closed market: events still refresh */
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  guint n = fx->all->len;

  /* Fresh data (< 5 s old) is not refetched. */
  fx_advance(fx, 2);
  fx_focus(fx);
  NO_ACTIONS(fx);

  fx_advance(fx, 30);
  fx_focus(fx);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 3);
  /* A second focus event while that round is in flight is merged into it. */
  fx_focus(fx);
  NO_ACTIONS(fx);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_cmpuint(fx->all->len, ==, n + 6); /* 3 quotes + 3 sparklines, once each */

  fx_free(fx);
}

static void
test_inflight_dedup_and_cap(void)
{
  Fx *fx = fx_new(WALL_OPEN);
  StkSchedSymbol many[50];
  char names[50][8];
  for (guint i = 0; i < G_N_ELEMENTS(many); i++) {
    g_snprintf(names[i], sizeof names[i], "T%u", i);
    many[i].symbol = names[i];
    many[i].kind = (i % 5 == 0) ? STK_KIND_FUND : STK_KIND_STOCK;
  }
  fx_symbols(fx, many, G_N_ELEMENTS(many));
  fx_online(fx, TRUE);
  fx_visible(fx, TRUE);
  g_assert_cmpuint(stk_scheduler_n_inflight(fx->s), ==, STK_SCHED_MAX_INFLIGHT);
  fx_focus(fx);
  fx_focus(fx);
  NO_ACTIONS(fx);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  for (guint i = 0; i < G_N_ELEMENTS(many); i++) {
    g_assert_cmpuint(COUNT_ALL(fx, STK_SCHED_FETCH_QUOTE, names[i], -1), ==, 1);
    g_assert_cmpuint(COUNT_ALL(fx, STK_SCHED_FETCH_SERIES, names[i], -1), ==, 1);
  }
  g_assert_cmpuint(fx->all->len, ==, 100);
  fx_free(fx);
}

static void
test_failure_marks_stale_and_keeps_going(void)
{
  Fx *fx = fx_started(WALL_OPEN);
  guint64 aapl = out_id(fx, "AAPL");
  fx_done(fx, aapl, STK_SCHED_RESULT_FAILED);
  g_assert_true(stk_scheduler_is_stale(fx->s, "AAPL"));
  g_assert_true(stk_scheduler_is_stale(fx->s, "aapl")); /* case-insensitive */
  g_assert_false(stk_scheduler_is_stale(fx->s, "SPY"));
  g_assert_false(stk_scheduler_is_stale(fx->s, "NOPE"));
  g_assert_false(stk_scheduler_is_stale(fx->s, NULL));
  /* The failure is not retried at once: the symbol moves on to its series. */
  g_assert_cmpuint(COUNT_ALL(fx, STK_SCHED_FETCH_QUOTE, "AAPL", -1), ==, 1);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_SERIES, "AAPL", -1), ==, 1);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_true(stk_scheduler_is_stale(fx->s, "AAPL")); /* only the quote job recovers it */

  /* The next periodic round retries; success clears the mark. */
  fx_advance(fx, 60);
  fx_tick(fx);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, "AAPL", -1), ==, 1);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_false(stk_scheduler_is_stale(fx->s, "AAPL"));

  /* A failed series marks stale too, a cancelled request never does. */
  fx_advance(fx, 30);
  fx_focus(fx);
  guint64 spy_q = out_id(fx, "SPY");
  fx_done(fx, spy_q, STK_SCHED_RESULT_OK);
  guint64 spy_s = out_id(fx, "SPY");
  g_assert_cmpuint(spy_s, !=, 0);
  fx_done(fx, spy_s, STK_SCHED_RESULT_FAILED);
  g_assert_true(stk_scheduler_is_stale(fx->s, "SPY"));
  fx_finish_all(fx, STK_SCHED_RESULT_CANCELLED);
  g_assert_false(stk_scheduler_is_stale(fx->s, "AAPL"));
  fx_free(fx);

  fx = fx_started(WALL_OPEN);
  fx_finish_all(fx, STK_SCHED_RESULT_CANCELLED);
  g_assert_false(stk_scheduler_is_stale(fx->s, "AAPL"));
  fx_free(fx);
}

static void
test_hide_cancels_and_drops_waiting(void)
{
  Fx *fx = fx_started(WALL_OPEN);
  guint64 ids[3] = {out_id(fx, "AAPL"), out_id(fx, "SPY"), out_id(fx, "FXAIX")};
  fx_visible(fx, FALSE);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_CANCEL, NULL, -1), ==, 3);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 0);
  for (guint i = 0; i < 3; i++) {
    gboolean found = FALSE;
    for (guint j = 0; j < fx->last->len; j++)
      found |= g_array_index(fx->last, StkSchedAction, j).id == ids[i];
    g_assert_true(found);
  }
  g_assert_cmpuint(stk_scheduler_n_inflight(fx->s), ==, 3); /* slots held until reported */
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);

  /* Reported as cancelled: nothing else starts (waiting sparklines dropped). */
  fx_finish_all(fx, STK_SCHED_RESULT_CANCELLED);
  g_assert_cmpuint(fx->all->len, ==, 6); /* 3 quotes + 3 cancels, no series */
  NO_ACTIONS(fx);
  g_assert_false(stk_scheduler_is_stale(fx->s, "AAPL"));

  /* Hide and show again before the cancellations are reported: the new
   * requests wait for the cancelled ones to finish. */
  fx_visible(fx, TRUE);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 3);
  fx_visible(fx, FALSE);
  fx_visible(fx, TRUE);
  NO_ACTIONS(fx); /* the cancelled ones still hold the slots */
  guint64 q = out_id(fx, "SPY");
  fx_done(fx, q, STK_SCHED_RESULT_CANCELLED);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, "SPY", -1), ==, 1);
  fx_free(fx);
}

static void
test_offline_cancels(void)
{
  Fx *fx = fx_started(WALL_OPEN);
  fx_online(fx, FALSE);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_CANCEL, NULL, -1), ==, 3);
  fx_finish_all(fx, STK_SCHED_RESULT_CANCELLED);
  NO_ACTIONS(fx);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
  fx_free(fx);
}

static void
test_selection_detail_and_cancel_on_range_change(void)
{
  Fx *fx = fx_new(WALL_OPEN);
  fx_symbols(fx, k_three, G_N_ELEMENTS(k_three));
  fx_select(fx, "aapl", STK_RANGE_1D);
  NO_ACTIONS(fx); /* not active yet */
  fx_online(fx, TRUE);
  fx_visible(fx, TRUE);
  /* The selection's quote goes first. */
  g_assert_cmpstr(g_array_index(fx->last, StkSchedAction, 0).symbol, ==, "AAPL");
  g_assert_cmpint(g_array_index(fx->last, StkSchedAction, 0).type, ==, STK_SCHED_FETCH_QUOTE);
  fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_OK);
  /* then its 1D detail series, before any sparkline */
  g_assert_cmpuint(fx->last->len, ==, 1);
  g_assert_cmpint(g_array_index(fx->last, StkSchedAction, 0).type, ==, STK_SCHED_FETCH_SERIES);
  g_assert_cmpint(g_array_index(fx->last, StkSchedAction, 0).range, ==, STK_RANGE_1D);
  guint64 detail_1d = out_id(fx, "AAPL");

  /* Range change: the 1D request is cancelled, its replacement waits. */
  fx_select(fx, "AAPL", STK_RANGE_3M);
  g_assert_cmpuint(fx->last->len, ==, 1);
  g_assert_cmpint(g_array_index(fx->last, StkSchedAction, 0).type, ==, STK_SCHED_CANCEL);
  g_assert_cmpuint(g_array_index(fx->last, StkSchedAction, 0).id, ==, detail_1d);
  fx_select(fx, "AAPL", STK_RANGE_3M); /* unchanged: no-op */
  NO_ACTIONS(fx);
  fx_done(fx, detail_1d, STK_SCHED_RESULT_CANCELLED);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_SERIES, "AAPL", STK_RANGE_3M), ==, 1);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_SERIES, "AAPL", STK_RANGE_1D), ==, 0);
  g_assert_false(stk_scheduler_is_stale(fx->s, "AAPL"));

  /* Back to 1M: the 3M request goes; the replacement serves the sparkline too. */
  fx_select(fx, "AAPL", STK_RANGE_1M);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_CANCEL, NULL, -1), ==, 1);
  fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_CANCELLED);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_SERIES, "AAPL", STK_RANGE_1M), ==, 1);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_cmpuint(COUNT_ALL(fx, STK_SCHED_FETCH_SERIES, "AAPL", STK_RANGE_1M), ==, 1);

  /* Selecting another symbol cancels the old symbol's detail request. */
  fx_select(fx, "SPY", STK_RANGE_1Y);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_SERIES, "SPY", STK_RANGE_1Y), ==, 1);
  fx_select(fx, "AAPL", STK_RANGE_1Y);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_CANCEL, NULL, -1), ==, 1);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_SERIES, "AAPL", STK_RANGE_1Y), ==, 1);
  /* Clearing the selection cancels too. */
  fx_select(fx, NULL, STK_RANGE_1Y);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_CANCEL, NULL, -1), ==, 1);
  fx_select(fx, "no/such", STK_RANGE_1Y); /* invalid symbol = no selection */
  NO_ACTIONS(fx);
  fx_finish_all(fx, STK_SCHED_RESULT_CANCELLED);
  fx_free(fx);
}

static void
test_sparkline_request_survives_range_change(void)
{
  Fx *fx = fx_new(WALL_OPEN);
  const StkSchedSymbol one[] = {{"AAPL", STK_KIND_STOCK}};
  fx_symbols(fx, one, 1);
  fx_online(fx, TRUE);
  fx_visible(fx, TRUE);
  fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_OK); /* starts the 1M sparkline request */
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_SERIES, "AAPL", STK_RANGE_1M), ==, 1);
  guint64 spark = out_id(fx, "AAPL");

  /* Selecting AAPL at 1M adopts that request as the DETAIL job. */
  fx_select(fx, "AAPL", STK_RANGE_1M);
  NO_ACTIONS(fx);
  /* Changing the range does not cancel a request that was started as the
   * sparkline job; the 3M job waits for the slot. */
  fx_select(fx, "AAPL", STK_RANGE_3M);
  NO_ACTIONS(fx);
  fx_done(fx, spark, STK_SCHED_RESULT_OK);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_SERIES, "AAPL", STK_RANGE_3M), ==, 1);
  g_assert_cmpuint(COUNT_ALL(fx, STK_SCHED_CANCEL, NULL, -1), ==, 0);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  fx_free(fx);
}

static void
test_fund_has_no_1d_detail(void)
{
  Fx *fx = fx_started(WALL_OPEN);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  fx_advance(fx, 60);
  fx_select(fx, "FXAIX", STK_RANGE_1D);
  NO_ACTIONS(fx); /* the harness also asserts that no fund 1D series is ever emitted */
  fx_select(fx, "FXAIX", STK_RANGE_6M);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_SERIES, "FXAIX", STK_RANGE_6M), ==, 1);
  fx_done(fx, out_id(fx, "FXAIX"), STK_SCHED_RESULT_OK);
  fx_select(fx, "FXAIX", STK_RANGE_1D);
  NO_ACTIONS(fx);
  fx_advance(fx, 600);
  fx_focus(fx); /* an event round must not request 1D either */
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  fx_free(fx);
}

static void
test_symbol_set_changes(void)
{
  Fx *fx = fx_started(WALL_OPEN);
  guint64 aapl = out_id(fx, "AAPL");

  /* Removing a symbol cancels its request; re-adding waits for it. */
  const StkSchedSymbol without_aapl[] = {{"SPY", STK_KIND_ETF}, {"FXAIX", STK_KIND_FUND}};
  fx_symbols(fx, without_aapl, G_N_ELEMENTS(without_aapl));
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_CANCEL, NULL, -1), ==, 1);
  g_assert_cmpuint(g_array_index(fx->last, StkSchedAction, 0).id, ==, aapl);
  fx_symbols(fx, k_three, G_N_ELEMENTS(k_three));
  NO_ACTIONS(fx);
  g_assert_cmpuint(stk_scheduler_n_inflight(fx->s), ==, 3);
  fx_done(fx, aapl, STK_SCHED_RESULT_CANCELLED);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, "AAPL", -1), ==, 1);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);

  /* Removal while idle frees the entry at once; a new symbol is fetched. */
  fx_symbols(fx, NULL, 0);
  NO_ACTIONS(fx);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1); /* nothing left to refresh */
  fx_advance(fx, 600);
  const StkSchedSymbol msft[] = {{"msft", STK_KIND_STOCK}};
  fx_symbols(fx, msft, 1);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, "MSFT", -1), ==, 1);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, fx->mono_us + STK_SCHED_PERIOD_US);
  /* Kept symbols are not refetched by a re-announcement of the same set. */
  fx_symbols(fx, msft, 1);
  NO_ACTIONS(fx);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  fx_free(fx);
}

static void
test_input_validation(void)
{
  Fx *fx = fx_new(WALL_OPEN);
  const StkSchedSymbol junk[] = {
      {NULL, STK_KIND_STOCK},
      {"", STK_KIND_STOCK},
      {"../etc", STK_KIND_STOCK},
      {"WAYTOOLONGSYMBOLXX", STK_KIND_STOCK},
      {"A B", STK_KIND_STOCK},
      {"\xc3\xa9", STK_KIND_STOCK},
      {"ok", STK_KIND_STOCK},
      {"OK", STK_KIND_STOCK}, /* repeat */
      {"BRK.B", STK_KIND_STOCK},
      {"EURUSD=X", STK_KIND_STOCK},
  };
  fx_symbols(fx, junk, G_N_ELEMENTS(junk));
  fx_online(fx, TRUE);
  fx_visible(fx, TRUE);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 3);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, "OK", -1), ==, 1);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, "BRK.B", -1), ==, 1);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, "EURUSD=X", -1), ==, 1);

  /* Ids that were never issued, or are already finished, are ignored. */
  stk_scheduler_request_done(fx->s, now_of(fx), 0, STK_SCHED_RESULT_FAILED);
  stk_scheduler_request_done(fx->s, now_of(fx), 123456, STK_SCHED_RESULT_FAILED);
  collect(fx);
  NO_ACTIONS(fx);
  guint64 id = out_id(fx, "OK");
  fx_done(fx, id, STK_SCHED_RESULT_OK);
  stk_scheduler_request_done(fx->s, now_of(fx), id, STK_SCHED_RESULT_FAILED);
  collect(fx);
  g_assert_false(stk_scheduler_is_stale(fx->s, "OK"));
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  fx_free(fx);
}

static void
test_clock_forward_jump_makes_deadline_due(void)
{
  Fx *fx = fx_started(wall_utc(2026, 10, 8, 18, 0, 0)); /* 14:00 ET */
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  gint64 deadline = stk_scheduler_next_wakeup_us(fx->s);
  g_assert_cmpint(deadline, ==, fx->mono_us + STK_SCHED_PERIOD_US);

  /* The wall clock jumps 1 h ahead while the monotonic clock moves 20 s (a
   * suspend). The next call re-arms the deadline for "now". */
  fx->mono_us += 20 * SEC;
  fx->wall_ms += 3600 * 1000;
  stk_scheduler_set_selection(fx->s, now_of(fx), NULL, STK_RANGE_1D);
  collect(fx);
  NO_ACTIONS(fx);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, fx->mono_us);
  fx_tick(fx);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 2); /* one round only */
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, fx->mono_us + STK_SCHED_PERIOD_US);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);

  /* A forward jump straight out of the session: no round, no timer. */
  fx->mono_us += 20 * SEC;
  fx->wall_ms += 8 * 3600 * 1000;
  fx_tick(fx);
  NO_ACTIONS(fx);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
  fx_free(fx);

  /* Ordinary drift (below the tolerance) does not trigger anything. */
  fx = fx_started(WALL_OPEN);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  deadline = stk_scheduler_next_wakeup_us(fx->s);
  fx->mono_us += 20 * SEC;
  fx->wall_ms += 20 * 1000 + STK_SCHED_JUMP_TOLERANCE_US / 1000 - 1;
  fx_tick(fx);
  NO_ACTIONS(fx);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, deadline);
  fx_free(fx);
}

static void
test_clock_backward_jump_changes_nothing(void)
{
  Fx *fx = fx_started(wall_utc(2026, 10, 8, 18, 0, 0)); /* 14:00 ET */
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  gint64 deadline = stk_scheduler_next_wakeup_us(fx->s);

  fx->mono_us += 10 * SEC;
  fx->wall_ms -= 3600 * 1000; /* 13:00 ET: still open */
  fx_tick(fx);
  NO_ACTIONS(fx);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, deadline);

  fx->mono_us = deadline;
  fx->wall_ms += 50 * 1000;
  fx_tick(fx);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 2);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);

  /* Backwards into closed hours: the timer goes away. */
  fx->mono_us += 5 * SEC;
  fx->wall_ms = wall_utc(2026, 10, 8, 3, 0, 0);
  fx_tick(fx);
  NO_ACTIONS(fx);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
  fx_free(fx);
}

static void
test_extreme_clock_values(void)
{
  Fx *fx = fx_new(WALL_OPEN);
  fx->mono_us = G_MAXINT64 - 5;
  fx_symbols(fx, k_three, G_N_ELEMENTS(k_three));
  fx_online(fx, TRUE);
  fx_visible(fx, TRUE);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, G_MAXINT64); /* saturated */
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  fx->wall_ms = G_MININT64;
  fx->mono_us = 0;
  fx_tick(fx);
  fx->wall_ms = G_MAXINT64;
  fx->mono_us = G_MININT64;
  fx_tick(fx);
  fx_focus(fx);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  fx_free(fx);
}

/* ------------------------------------------------------------------------ */
/* Regressions: per-job freshness, retries, stale-mark contract             */
/* ------------------------------------------------------------------------ */

/* A series that was cancelled by hiding the window (or by losing the network)
 * never arrived. Showing again (or reconnecting) less than
 * STK_SCHED_MIN_EVENT_GAP_US after the successful quote must fetch it, whatever
 * the state of the market: only the quote, which did arrive, is skipped. */
static void
test_dropped_series_refetched_within_gap(void)
{
  const StkSchedSymbol one[] = {{"AAPL", STK_KIND_STOCK}};
  for (guint variant = 0; variant < 8; variant++) {
    gboolean by_network = (variant & 1) != 0;  /* flap the network, not the window    */
    gboolean late_report = (variant & 2) != 0; /* show before the cancel is reported  */
    gboolean open = (variant & 4) != 0;        /* market open (else closed)           */
    g_test_message("variant %u", variant);

    Fx *fx = fx_new(open ? WALL_OPEN : WALL_AFTER_HOURS);
    fx_symbols(fx, one, 1);
    fx_online(fx, TRUE);
    fx_visible(fx, TRUE);
    fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_OK); /* the quote arrives */
    g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_SERIES, "AAPL", STK_SCHED_SPARK_RANGE), ==, 1);
    guint64 spark = out_id(fx, "AAPL");

    fx_advance(fx, 1);
    if (by_network)
      fx_online(fx, FALSE);
    else
      fx_visible(fx, FALSE);
    g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_CANCEL, NULL, -1), ==, 1);
    if (!late_report)
      fx_done(fx, spark, STK_SCHED_RESULT_CANCELLED);

    fx_advance(fx, 1);
    guint mark = fx->all->len;
    if (by_network)
      fx_online(fx, TRUE);
    else
      fx_visible(fx, TRUE);
    if (late_report) {
      NO_ACTIONS(fx); /* the cancelled request still holds the symbol's slot */
      fx_done(fx, spark, STK_SCHED_RESULT_CANCELLED);
    }
    g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_QUOTE, "AAPL", -1), ==, 0); /* 2 s old */
    g_assert_cmpuint(
        COUNT_SINCE(fx, mark, STK_SCHED_FETCH_SERIES, "AAPL", STK_SCHED_SPARK_RANGE), ==, 1);
    fx_finish_all(fx, STK_SCHED_RESULT_OK);
    g_assert_cmpuint(COUNT_OK(fx, STK_SCHED_FETCH_SERIES, "AAPL", STK_SCHED_SPARK_RANGE), ==, 1);
    g_assert_false(stk_scheduler_is_stale(fx->s, "AAPL"));

    /* Everything has arrived now: another event within the gap does nothing. */
    fx_advance(fx, 1);
    fx_focus(fx);
    NO_ACTIONS(fx);
    fx_free(fx);
  }
}

/* Same for the DETAIL job of the selection, a fund among others: the series
 * of the selected range is fetched again; nothing that arrived is. */
static void
test_dropped_detail_refetched_within_gap(void)
{
  for (guint variant = 0; variant < 4; variant++) {
    gboolean by_network = (variant & 1) != 0;
    StkRange range = (variant & 2) != 0 ? STK_RANGE_6M : STK_SCHED_SPARK_RANGE;
    g_test_message("variant %u", variant);

    Fx *fx = fx_new(WALL_OPEN);
    fx_symbols(fx, k_three, G_N_ELEMENTS(k_three));
    fx_select(fx, "FXAIX", range);
    fx_online(fx, TRUE);
    fx_visible(fx, TRUE);
    fx_done(fx, out_id(fx, "FXAIX"), STK_SCHED_RESULT_OK);
    fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_OK);
    fx_done(fx, out_id(fx, "SPY"), STK_SCHED_RESULT_OK);
    g_assert_cmpuint(COUNT_ALL(fx, STK_SCHED_FETCH_SERIES, "FXAIX", range), ==, 1);
    g_assert_cmpuint(fx->out->len, ==, 3); /* the three series are in flight */

    fx_advance(fx, 1);
    if (by_network)
      fx_online(fx, FALSE);
    else
      fx_visible(fx, FALSE);
    g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_CANCEL, NULL, -1), ==, 3);
    fx_finish_all(fx, STK_SCHED_RESULT_CANCELLED);

    fx_advance(fx, 1);
    guint mark = fx->all->len;
    if (by_network)
      fx_online(fx, TRUE);
    else
      fx_visible(fx, TRUE);
    fx_finish_all(fx, STK_SCHED_RESULT_OK);
    g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 0);
    g_assert_cmpuint(COUNT_OK(fx, STK_SCHED_FETCH_SERIES, "FXAIX", range), ==, 1);
    g_assert_cmpuint(COUNT_OK(fx, STK_SCHED_FETCH_SERIES, "FXAIX", STK_SCHED_SPARK_RANGE), ==, 1);
    g_assert_cmpuint(COUNT_OK(fx, STK_SCHED_FETCH_SERIES, "AAPL", STK_SCHED_SPARK_RANGE), ==, 1);
    g_assert_cmpuint(COUNT_OK(fx, STK_SCHED_FETCH_SERIES, "SPY", STK_SCHED_SPARK_RANGE), ==, 1);
    fx_focus(fx);
    NO_ACTIONS(fx);
    fx_free(fx);
  }
}

/* Jobs that were still waiting for a slot when the window was hidden are
 * dropped; showing again within the gap must start them: after the round
 * every symbol has exactly one successful quote and one successful sparkline
 * (none lost, none fetched twice). */
static void
test_dropped_waiting_jobs_refetched_within_gap(void)
{
  enum { N = 30 };
  StkSchedSymbol syms[N];
  char names[N][8];
  for (guint i = 0; i < N; i++) {
    g_snprintf(names[i], sizeof names[i], "T%u", i);
    syms[i].symbol = names[i];
    syms[i].kind = (i % 5 == 0) ? STK_KIND_FUND : STK_KIND_STOCK;
  }
  Fx *fx = fx_new(WALL_OPEN);
  fx_symbols(fx, syms, N);
  fx_online(fx, TRUE);
  fx_visible(fx, TRUE);
  for (guint i = 0; i < 12; i++) /* most of the round is still waiting */
    fx_done(fx, g_array_index(fx->out, Outstanding, 0).id, STK_SCHED_RESULT_OK);
  fx_advance(fx, 1);
  fx_visible(fx, FALSE);
  fx_finish_all(fx, STK_SCHED_RESULT_CANCELLED);
  fx_advance(fx, 1);
  fx_visible(fx, TRUE);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  for (guint i = 0; i < N; i++) {
    g_assert_cmpuint(COUNT_OK(fx, STK_SCHED_FETCH_QUOTE, names[i], -1), ==, 1);
    g_assert_cmpuint(COUNT_OK(fx, STK_SCHED_FETCH_SERIES, names[i], STK_SCHED_SPARK_RANGE), ==, 1);
  }
  fx_free(fx);
}

/* A failure forgets the job's earlier success: a failed job is never "fresh",
 * however recent its previous success was. */
static void
test_failed_job_is_not_fresh(void)
{
  const StkSchedSymbol one[] = {{"AAPL", STK_KIND_STOCK}};
  Fx *fx = fx_new(WALL_AFTER_HOURS);
  fx_symbols(fx, one, 1);
  fx_online(fx, TRUE);
  fx_visible(fx, TRUE);
  fx_finish_all(fx, STK_SCHED_RESULT_OK); /* quote and sparkline arrive */
  fx_advance(fx, 1);
  fx_select(fx, "AAPL", STK_SCHED_SPARK_RANGE); /* a new request for the series, 1 s later */
  guint64 detail_1m = out_id(fx, "AAPL");
  g_assert_cmpuint(detail_1m, !=, 0);
  fx_advance(fx, 1);
  fx_select(fx, "AAPL", STK_RANGE_3M); /* cancels it; the sparkline job is wanted again */
  fx_done(fx, detail_1m, STK_SCHED_RESULT_CANCELLED);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_SERIES, "AAPL", STK_RANGE_3M), ==, 1);
  fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_OK);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_SERIES, "AAPL", STK_SCHED_SPARK_RANGE), ==, 1);
  fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_FAILED); /* 2 s after the earlier success */
  g_assert_true(stk_scheduler_is_stale(fx->s, "AAPL"));

  fx_advance(fx, 1);
  guint mark = fx->all->len;
  fx_focus(fx); /* only the sparkline is due: its latest request failed */
  g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 0);
  g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_SERIES, NULL, -1), ==, 1);
  g_assert_cmpuint(
      COUNT_SINCE(fx, mark, STK_SCHED_FETCH_SERIES, "AAPL", STK_SCHED_SPARK_RANGE), ==, 1);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_false(stk_scheduler_is_stale(fx->s, "AAPL"));
  fx_free(fx);
}

/* The DETAIL job belongs to one (symbol, range): a success of the 3M series
 * says nothing about the 1Y series selected while the window was hidden. */
static void
test_selection_change_resets_detail_freshness(void)
{
  const StkSchedSymbol one[] = {{"AAPL", STK_KIND_STOCK}};
  Fx *fx = fx_new(WALL_AFTER_HOURS);
  fx_symbols(fx, one, 1);
  fx_select(fx, "AAPL", STK_RANGE_3M);
  fx_online(fx, TRUE);
  fx_visible(fx, TRUE);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_cmpuint(COUNT_OK(fx, STK_SCHED_FETCH_SERIES, "AAPL", STK_RANGE_3M), ==, 1);
  fx_advance(fx, 1);
  fx_visible(fx, FALSE);
  fx_select(fx, "AAPL", STK_RANGE_1Y); /* hidden: nothing is requested */
  NO_ACTIONS(fx);
  fx_advance(fx, 1);
  guint mark = fx->all->len;
  fx_visible(fx, TRUE);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_SERIES, "AAPL", STK_RANGE_1Y), ==, 1);
  g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_SERIES, NULL, -1), ==, 1);
  g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 0);
  fx_free(fx);
}

/* A sparkline request adopted as the DETAIL job (same range) satisfies both
 * jobs: an event within the gap must not fetch that series a second time. */
static void
test_adopted_request_counts_for_both_jobs(void)
{
  const StkSchedSymbol one[] = {{"AAPL", STK_KIND_STOCK}};
  Fx *fx = fx_new(WALL_AFTER_HOURS);
  fx_symbols(fx, one, 1);
  fx_online(fx, TRUE);
  fx_visible(fx, TRUE);
  fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_OK); /* starts the 1M sparkline request */
  fx_select(fx, "AAPL", STK_SCHED_SPARK_RANGE);          /* adopts it */
  NO_ACTIONS(fx);
  fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_OK);
  fx_advance(fx, 2);
  fx_focus(fx);
  NO_ACTIONS(fx);
  fx_advance(fx, 10);
  fx_focus(fx); /* old enough now: quote and the one 1M series */
  g_assert_cmpuint(fx->last->len, ==, 1);           /* the quote of the selection...  */
  fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_OK);
  g_assert_cmpuint(fx->last->len, ==, 1);           /* ... then one series for both jobs */
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_cmpuint(COUNT_ALL(fx, STK_SCHED_FETCH_SERIES, "AAPL", STK_SCHED_SPARK_RANGE), ==, 2);

  /* The same adopted request failing marks the symbol stale; the mark of the
   * DETAIL job goes with the selection, the sparkline's stays. */
  fx_advance(fx, 10);
  fx_focus(fx);
  fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_OK);
  fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_FAILED);
  g_assert_true(stk_scheduler_is_stale(fx->s, "AAPL"));
  fx_select(fx, NULL, STK_SCHED_SPARK_RANGE);
  g_assert_true(stk_scheduler_is_stale(fx->s, "AAPL"));
  fx_free(fx);
}

/* A failed sparkline is retried by the periodic round (market open), which
 * issues nothing else for a healthy symbol. */
static void
test_failed_sparkline_retried_by_periodic_round(void)
{
  Fx *fx = fx_started(WALL_OPEN);
  fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_OK); /* quote; the sparkline starts */
  fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_FAILED);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_true(stk_scheduler_is_stale(fx->s, "AAPL"));
  g_assert_false(stk_scheduler_is_stale(fx->s, "SPY"));

  fx_advance(fx, 60);
  guint mark = fx->all->len;
  fx_tick(fx);
  /* One request per symbol: the sparkline follows the symbol's quote. */
  g_assert_cmpuint(fx->last->len, ==, 2);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, "AAPL", -1), ==, 1);
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, "SPY", -1), ==, 1);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_cmpuint(
      COUNT_SINCE(fx, mark, STK_SCHED_FETCH_SERIES, "AAPL", STK_SCHED_SPARK_RANGE), ==, 1);
  g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_SERIES, NULL, -1), ==, 1);
  g_assert_false(stk_scheduler_is_stale(fx->s, "AAPL"));

  /* Healthy again: periodic rounds are quotes only. */
  for (guint round = 0; round < 3; round++) {
    fx_advance(fx, 60);
    mark = fx->all->len;
    fx_tick(fx);
    fx_finish_all(fx, STK_SCHED_RESULT_OK);
    g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_SERIES, NULL, -1), ==, 0);
    g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 2);
  }
  fx_free(fx);
}

/* Failed jobs of a fund (its quote, its sparkline, the detail series of the
 * selection) are retried by the periodic round too, which also arms the timer
 * for a list that has no other periodic work; a healthy fund is left alone. */
static void
test_failed_fund_jobs_retried_by_periodic_round(void)
{
  const StkSchedSymbol funds[] = {{"FXAIX", STK_KIND_FUND}, {"FSKAX", STK_KIND_FUND}};
  Fx *fx = fx_new(WALL_OPEN);
  fx_symbols(fx, funds, G_N_ELEMENTS(funds));
  fx_select(fx, "FXAIX", STK_RANGE_6M);
  fx_online(fx, TRUE);
  fx_visible(fx, TRUE);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1); /* nothing periodic yet */
  fx_done(fx, out_id(fx, "FXAIX"), STK_SCHED_RESULT_OK);        /* quote; 6M detail starts */
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_SERIES, "FXAIX", STK_RANGE_6M), ==, 1);
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
  fx_done(fx, out_id(fx, "FXAIX"), STK_SCHED_RESULT_FAILED); /* the detail series fails */
  g_assert_true(stk_scheduler_is_stale(fx->s, "FXAIX"));
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, fx->mono_us + STK_SCHED_PERIOD_US);
  fx_done(fx, out_id(fx, "FSKAX"), STK_SCHED_RESULT_FAILED); /* a quote fails */
  g_assert_true(stk_scheduler_is_stale(fx->s, "FSKAX"));
  fx_finish_all(fx, STK_SCHED_RESULT_OK); /* the sparklines arrive */
  g_assert_true(stk_scheduler_is_stale(fx->s, "FXAIX"));
  g_assert_true(stk_scheduler_is_stale(fx->s, "FSKAX"));

  fx_advance(fx, 60);
  guint mark = fx->all->len;
  fx_tick(fx);
  g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_SERIES, "FXAIX", STK_RANGE_6M), ==, 1);
  g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_QUOTE, "FSKAX", -1), ==, 1);
  g_assert_cmpuint(fx->all->len - mark, ==, 2); /* and nothing else */
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_false(stk_scheduler_is_stale(fx->s, "FXAIX"));
  g_assert_false(stk_scheduler_is_stale(fx->s, "FSKAX"));
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1); /* healthy: no timer again */

  /* Retries that fail again are retried again, one request per symbol. */
  fx_advance(fx, 600);
  fx_focus(fx);
  fx_done(fx, out_id(fx, "FSKAX"), STK_SCHED_RESULT_FAILED);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  for (guint round = 0; round < 3; round++) {
    g_assert_true(stk_scheduler_is_stale(fx->s, "FSKAX"));
    fx_advance(fx, 60);
    mark = fx->all->len;
    fx_tick(fx);
    g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_QUOTE, "FSKAX", -1), ==, 1);
    g_assert_cmpuint(fx->all->len - mark, ==, 1);
    fx_finish_all(fx, round < 2 ? STK_SCHED_RESULT_FAILED : STK_SCHED_RESULT_OK);
  }
  g_assert_false(stk_scheduler_is_stale(fx->s, "FSKAX"));
  fx_free(fx);
}

/* With the market closed (or the window hidden, or offline) a failure has no
 * timer: the next event round retries it, and only that job (the rest is
 * fresher than the gap or fetched by the round anyway). */
static void
test_failed_jobs_without_timer_retried_by_event_round(void)
{
  const StkSchedSymbol mixed[] = {{"AAPL", STK_KIND_STOCK}, {"FXAIX", STK_KIND_FUND}};
  gint64 walls[] = {WALL_AFTER_HOURS, WALL_OPEN};
  for (guint w = 0; w < G_N_ELEMENTS(walls); w++) {
    Fx *fx = fx_new(walls[w]);
    fx_symbols(fx, mixed, G_N_ELEMENTS(mixed));
    fx_online(fx, TRUE);
    fx_visible(fx, TRUE);
    fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_OK);
    fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_FAILED); /* sparkline */
    fx_done(fx, out_id(fx, "FXAIX"), STK_SCHED_RESULT_OK);
    fx_done(fx, out_id(fx, "FXAIX"), STK_SCHED_RESULT_FAILED); /* sparkline */
    g_assert_true(stk_scheduler_is_stale(fx->s, "AAPL"));
    g_assert_true(stk_scheduler_is_stale(fx->s, "FXAIX"));

    /* Hidden / offline: nothing is requested and there is no timer. */
    fx_visible(fx, FALSE);
    g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
    fx_advance(fx, 600);
    fx_tick(fx);
    NO_ACTIONS(fx);
    fx_online(fx, FALSE);
    fx_visible(fx, TRUE);
    NO_ACTIONS(fx);
    g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
    guint mark = fx->all->len;
    fx_online(fx, TRUE); /* the event round retries both failed sparklines */
    fx_finish_all(fx, STK_SCHED_RESULT_OK);
    g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_SERIES, "AAPL", STK_SCHED_SPARK_RANGE),
                     ==, 1);
    g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_SERIES, "FXAIX", STK_SCHED_SPARK_RANGE),
                     ==, 1);
    g_assert_false(stk_scheduler_is_stale(fx->s, "AAPL"));
    g_assert_false(stk_scheduler_is_stale(fx->s, "FXAIX"));
    fx_free(fx);
  }

  /* Closed market, window left alone: the failure persists, no timer. */
  Fx *fx = fx_new(WALL_AFTER_HOURS);
  fx_symbols(fx, mixed, G_N_ELEMENTS(mixed));
  fx_online(fx, TRUE);
  fx_visible(fx, TRUE);
  fx_done(fx, out_id(fx, "FXAIX"), STK_SCHED_RESULT_FAILED);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_true(stk_scheduler_is_stale(fx->s, "FXAIX"));
  g_assert_cmpint(stk_scheduler_next_wakeup_us(fx->s), ==, -1);
  fx_advance(fx, 3600);
  fx_tick(fx);
  NO_ACTIONS(fx);
  fx_advance(fx, 10);
  fx_focus(fx); /* the next event round retries it */
  g_assert_cmpuint(COUNT_LAST(fx, STK_SCHED_FETCH_QUOTE, "FXAIX", -1), ==, 1);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  g_assert_false(stk_scheduler_is_stale(fx->s, "FXAIX"));
  fx_free(fx);
}

/* The selection moving away from a symbol drops the stale mark of its DETAIL
 * job. The owner relies on exactly this: it re-reads the mark of the previous
 * and the new selection after set_selection() (see the stale-mark contract in
 * stk-scheduler.h). */
static void
test_selection_change_drops_detail_stale_mark(void)
{
  Fx *fx = fx_started(WALL_AFTER_HOURS);
  fx_finish_all(fx, STK_SCHED_RESULT_OK);
  fx_advance(fx, 10);
  fx_select(fx, "AAPL", STK_RANGE_1Y);
  fx_done(fx, out_id(fx, "AAPL"), STK_SCHED_RESULT_FAILED);
  g_assert_true(stk_scheduler_is_stale(fx->s, "AAPL"));
  fx_select(fx, "SPY", STK_RANGE_1Y);
  g_assert_false(stk_scheduler_is_stale(fx->s, "AAPL"));
  fx_done(fx, out_id(fx, "SPY"), STK_SCHED_RESULT_FAILED);
  g_assert_true(stk_scheduler_is_stale(fx->s, "SPY"));
  fx_select(fx, "SPY", STK_RANGE_3M); /* a range change drops it as well */
  g_assert_false(stk_scheduler_is_stale(fx->s, "SPY"));
  fx_select(fx, NULL, STK_RANGE_3M);
  fx_finish_all(fx, STK_SCHED_RESULT_CANCELLED);
  fx_free(fx);
}

/* ------------------------------------------------------------------------ */
/* Randomised walk over every input                                         */
/* ------------------------------------------------------------------------ */

static const StkSchedSymbol k_pool[] = {{"AAPL", STK_KIND_STOCK},
                                        {"SPY", STK_KIND_ETF},
                                        {"FXAIX", STK_KIND_FUND},
                                        {"MSFT", STK_KIND_STOCK},
                                        {"FSKAX", STK_KIND_FUND}};
#define POOL G_N_ELEMENTS(k_pool)

/* Like fx_finish_all(), but as the provider reports them: requests the
 * scheduler cancelled end CANCELLED, all others succeed. */
static void
fx_finish_all_sane(Fx *fx)
{
  for (guint guard = 0; fx->out->len > 0; guard++) {
    g_assert_cmpuint(guard, <, 100000);
    const Outstanding o = g_array_index(fx->out, Outstanding, 0);
    fx_done(fx, o.id, o.cancelled ? STK_SCHED_RESULT_CANCELLED : STK_SCHED_RESULT_OK);
  }
}

/* Applies a long random sequence of inputs and checks after every step:
 *  - the harness invariants (one request per symbol, at most four, ...);
 *  - no FETCH, and no timer, while hidden or offline; no timer while the
 *    market is closed; the timer is never more than one period away;
 *  - the stale-mark contract: is_stale() of a symbol changes only when the
 *    input is a completion of that symbol's request, a selection change from
 *    or to it, or a change of the tracked set that adds or removes it;
 * and at the end that one event round with every request succeeding leaves
 * no symbol stale and nothing outstanding. */
static void
test_random_inputs_keep_invariants(void)
{
  const gint64 starts[] = {WALL_OPEN, wall_utc(2026, 10, 8, 19, 52, 0), WALL_AFTER_HOURS};
  for (guint seed = 1; seed <= 90; seed++) {
    g_test_message("seed %u", seed);
    GRand *rnd = g_rand_new_with_seed(seed);
    Fx *fx = fx_new(starts[seed % G_N_ELEMENTS(starts)]);
    gboolean visible = FALSE;
    gboolean online = FALSE;
    gboolean tracked[POOL] = {FALSE};
    gboolean has_sel = FALSE;
    guint sel = 0;

    for (guint step = 0; step < 400; step++) {
      fx_advance(fx, g_rand_int_range(rnd, 0, 4));
      gboolean before[POOL];
      gboolean may_change[POOL] = {FALSE};
      for (guint i = 0; i < POOL; i++)
        before[i] = stk_scheduler_is_stale(fx->s, k_pool[i].symbol);
      guint mark = fx->all->len;
      gboolean called = TRUE;

      switch (g_rand_int_range(rnd, 0, 10)) {
      case 0: {
        StkSchedSymbol list[POOL];
        gsize n = 0;
        for (guint i = 0; i < POOL; i++) {
          gboolean in = g_rand_boolean(rnd);
          if (in)
            list[n++] = k_pool[i];
          if (in != tracked[i])
            may_change[i] = TRUE;
          tracked[i] = in;
        }
        fx_symbols(fx, list, n);
        break;
      }
      case 1: {
        guint pick = (guint)g_rand_int_range(rnd, 0, POOL + 1); /* POOL: none */
        StkRange range = (StkRange)g_rand_int_range(rnd, STK_RANGE_1D, STK_RANGE_1Y + 1);
        if (has_sel)
          may_change[sel] = TRUE;
        if (pick < POOL)
          may_change[pick] = TRUE;
        has_sel = pick < POOL;
        sel = pick;
        fx_select(fx, has_sel ? k_pool[pick].symbol : NULL, range);
        break;
      }
      case 2:
        visible = !visible;
        fx_visible(fx, visible);
        break;
      case 3:
        online = !online;
        fx_online(fx, online);
        break;
      case 4:
        fx_focus(fx);
        break;
      case 5:
        fx_advance(fx, g_rand_int_range(rnd, 0, 90));
        fx_tick(fx);
        break;
      default:
        if (fx->out->len == 0) {
          called = FALSE;
          break;
        }
        {
          Outstanding o =
              g_array_index(fx->out, Outstanding, g_rand_int_range(rnd, 0, fx->out->len));
          StkSchedResult r = o.cancelled ? STK_SCHED_RESULT_CANCELLED
                                         : (g_rand_int_range(rnd, 0, 10) < 7
                                                ? STK_SCHED_RESULT_OK
                                                : STK_SCHED_RESULT_FAILED);
          for (guint i = 0; i < POOL; i++) {
            if (strcmp(k_pool[i].symbol, o.symbol) == 0)
              may_change[i] = TRUE;
          }
          fx_done(fx, o.id, r);
        }
        break;
      }
      if (!called)
        continue;

      for (guint i = 0; i < POOL; i++) {
        if (!may_change[i])
          g_assert_cmpint(stk_scheduler_is_stale(fx->s, k_pool[i].symbol), ==, before[i]);
      }
      gint64 wake = stk_scheduler_next_wakeup_us(fx->s);
      if (!(visible && online)) {
        g_assert_cmpint(wake, ==, -1);
        g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_QUOTE, NULL, -1), ==, 0);
        g_assert_cmpuint(COUNT_SINCE(fx, mark, STK_SCHED_FETCH_SERIES, NULL, -1), ==, 0);
      }
      if (wake != -1) {
        g_assert_true(visible && online);
        g_assert_true(stk_market_is_open(fx->wall_ms));
        g_assert_cmpint(wake, <=, fx->mono_us + STK_SCHED_PERIOD_US);
      }
    }

    /* One more event round, every request succeeds: nothing stays stale. */
    fx_visible(fx, TRUE);
    fx_online(fx, TRUE);
    fx_advance(fx, 10);
    fx_focus(fx);
    fx_finish_all_sane(fx);
    for (guint i = 0; i < POOL; i++)
      g_assert_false(stk_scheduler_is_stale(fx->s, k_pool[i].symbol));
    g_assert_cmpuint(stk_scheduler_n_inflight(fx->s), ==, 0);
    fx_free(fx);
    g_rand_free(rnd);
  }
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/scheduler/idle", test_idle_has_no_actions_no_timer);
  g_test_add_func("/scheduler/start-round", test_start_round_once_per_job);
  g_test_add_func("/scheduler/periodic", test_periodic_round_open_market);
  g_test_add_func("/scheduler/funds-only", test_periodic_skips_fund_only_list);
  g_test_add_func("/scheduler/no-timer/market-closed", test_no_timer_when_market_closed);
  g_test_add_func("/scheduler/market-boundaries", test_market_open_boundaries);
  g_test_add_func("/scheduler/market-closes", test_market_closes_while_ticking);
  g_test_add_func("/scheduler/no-timer/hidden-offline", test_no_timer_when_hidden_or_offline);
  g_test_add_func("/scheduler/focus-regain", test_focus_regain_refreshes_once);
  g_test_add_func("/scheduler/inflight-dedup-cap", test_inflight_dedup_and_cap);
  g_test_add_func("/scheduler/failure-stale", test_failure_marks_stale_and_keeps_going);
  g_test_add_func("/scheduler/hide-cancels", test_hide_cancels_and_drops_waiting);
  g_test_add_func("/scheduler/offline-cancels", test_offline_cancels);
  g_test_add_func("/scheduler/selection", test_selection_detail_and_cancel_on_range_change);
  g_test_add_func("/scheduler/sparkline-request", test_sparkline_request_survives_range_change);
  g_test_add_func("/scheduler/fund-no-1d", test_fund_has_no_1d_detail);
  g_test_add_func("/scheduler/symbol-set", test_symbol_set_changes);
  g_test_add_func("/scheduler/input-validation", test_input_validation);
  g_test_add_func("/scheduler/clock/forward", test_clock_forward_jump_makes_deadline_due);
  g_test_add_func("/scheduler/clock/backward", test_clock_backward_jump_changes_nothing);
  g_test_add_func("/scheduler/clock/extreme", test_extreme_clock_values);
  g_test_add_func("/scheduler/regress/dropped-series-gap",
                  test_dropped_series_refetched_within_gap);
  g_test_add_func("/scheduler/regress/dropped-detail-gap",
                  test_dropped_detail_refetched_within_gap);
  g_test_add_func("/scheduler/regress/dropped-waiting-gap",
                  test_dropped_waiting_jobs_refetched_within_gap);
  g_test_add_func("/scheduler/regress/failed-not-fresh", test_failed_job_is_not_fresh);
  g_test_add_func("/scheduler/regress/adopted-request", test_adopted_request_counts_for_both_jobs);
  g_test_add_func("/scheduler/regress/detail-freshness",
                  test_selection_change_resets_detail_freshness);
  g_test_add_func("/scheduler/regress/retry-sparkline-periodic",
                  test_failed_sparkline_retried_by_periodic_round);
  g_test_add_func("/scheduler/regress/retry-fund-periodic",
                  test_failed_fund_jobs_retried_by_periodic_round);
  g_test_add_func("/scheduler/regress/retry-event-round",
                  test_failed_jobs_without_timer_retried_by_event_round);
  g_test_add_func("/scheduler/regress/selection-stale",
                  test_selection_change_drops_detail_stale_mark);
  g_test_add_func("/scheduler/regress/random", test_random_inputs_keep_invariants);
  return g_test_run();
}
