/* stk-scheduler.c - see stk-scheduler.h for the rules. Pure logic. */
#include "stk-scheduler.h"

#include <string.h>

#include "stk-market.h"

/* The three jobs of a symbol (see stk-scheduler.h); the WANT_* values are the
 * bits used by Entry.wants, .failed and .fresh, the JOB_* values index
 * Entry.ok_us. */
enum { JOB_QUOTE, JOB_SPARK, JOB_DETAIL, N_JOBS };
enum {
  WANT_QUOTE = 1 << JOB_QUOTE,
  WANT_SPARK = 1 << JOB_SPARK,
  WANT_DETAIL = 1 << JOB_DETAIL
};

typedef struct {
  char symbol[STK_SCHED_SYMBOL_BYTES];
  StkKind kind;
  /* Removed from the tracked set but its cancelled request has not been
   * reported finished yet: kept only to hold the slot. */
  gboolean removed;
  guint wants;  /* WANT_* jobs waiting for a slot            */
  guint failed; /* WANT_* jobs whose latest request failed   */

  /* Freshness is tracked per job: a successful QUOTE says nothing about the
   * SPARK or DETAIL job, which may have been cancelled or dropped. */
  guint fresh;          /* WANT_* jobs whose latest request succeeded (ok_us valid) */
  gint64 ok_us[N_JOBS]; /* monotonic time of that success, indexed by JOB_*         */

  guint64 req_id; /* 0: nothing in flight */
  gboolean req_is_quote;
  StkRange req_range;      /* series request only */
  gboolean req_for_detail; /* series request started for the DETAIL job */
  gboolean req_cancelling;
} Entry;

struct _StkScheduler {
  GPtrArray *entries; /* Entry*, display order, removed ones last */
  gboolean has_sel;
  char sel_symbol[STK_SCHED_SYMBOL_BYTES];
  StkRange sel_range;
  gboolean visible;
  gboolean online;
  guint n_inflight;
  guint64 next_id;
  gint64 deadline_us; /* -1: none */
  gboolean has_last;
  StkSchedTime last;
  GArray *actions; /* StkSchedAction, NULL when empty */
};

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static gint64
sat_add(gint64 a, gint64 b)
{
  if (b > 0 && a > G_MAXINT64 - b)
    return G_MAXINT64;
  if (b < 0 && a < G_MININT64 - b)
    return G_MININT64;
  return a + b;
}

static gint64
sat_sub(gint64 a, gint64 b)
{
  if (b < 0 && a > G_MAXINT64 + b)
    return G_MAXINT64;
  if (b > 0 && a < G_MININT64 + b)
    return G_MININT64;
  return a - b;
}

/* Copies the upper-cased symbol into @out; FALSE if it is not valid. */
static gboolean
normalise(const char *symbol, char out[STK_SCHED_SYMBOL_BYTES])
{
  if (!stk_symbol_is_valid(symbol))
    return FALSE;
  gsize i = 0;
  for (; symbol[i] != '\0'; i++)
    out[i] = g_ascii_toupper(symbol[i]);
  out[i] = '\0';
  return TRUE;
}

static gboolean
market_open(gint64 wall_ms)
{
  GDate date;
  g_date_clear(&date, 1);
  /* stk_market_is_open() logs a critical outside its calendar. */
  if (!stk_market_local_date(wall_ms, &date))
    return FALSE;
  return stk_market_is_open(wall_ms);
}

static gboolean
is_active(const StkScheduler *self)
{
  return self->visible && self->online;
}

static Entry *
find_entry(const StkScheduler *self, const char *key, gboolean include_removed)
{
  for (guint i = 0; i < self->entries->len; i++) {
    Entry *e = g_ptr_array_index(self->entries, i);
    if ((include_removed || !e->removed) && strcmp(e->symbol, key) == 0)
      return e;
  }
  return NULL;
}

static gboolean
is_selected(const StkScheduler *self, const Entry *e)
{
  return self->has_sel && !e->removed && strcmp(e->symbol, self->sel_symbol) == 0;
}

static gboolean
detail_valid(const StkScheduler *self, const Entry *e)
{
  return is_selected(self, e) && !(e->kind == STK_KIND_FUND && self->sel_range == STK_RANGE_1D);
}

/* Is a (not cancelled) request in flight that already delivers job @bit? */
static gboolean
job_in_flight(const StkScheduler *self, const Entry *e, guint bit)
{
  if (e->req_id == 0 || e->req_cancelling)
    return FALSE;
  switch (bit) {
  case WANT_QUOTE:
    return e->req_is_quote;
  case WANT_SPARK:
    return !e->req_is_quote && e->req_range == STK_SCHED_SPARK_RANGE;
  case WANT_DETAIL:
    return !e->req_is_quote && detail_valid(self, e) && e->req_range == self->sel_range;
  default:
    return FALSE;
  }
}

/* Marks job @bit as waiting unless it is already in flight. */
static void
want(const StkScheduler *self, Entry *e, guint bit)
{
  if (!job_in_flight(self, e, bit))
    e->wants |= bit;
}

static void
emit(StkScheduler *self, const StkSchedAction *action)
{
  if (self->actions == NULL)
    self->actions = g_array_new(FALSE, FALSE, sizeof(StkSchedAction));
  g_array_append_val(self->actions, *action);
}

static void
cancel_request(StkScheduler *self, Entry *e)
{
  if (e->req_id == 0 || e->req_cancelling)
    return;
  e->req_cancelling = TRUE;
  StkSchedAction a;
  memset(&a, 0, sizeof a);
  a.type = STK_SCHED_CANCEL;
  a.id = e->req_id;
  emit(self, &a);
}

/* ------------------------------------------------------------------------ */
/* Starting requests                                                        */
/* ------------------------------------------------------------------------ */

static void
start_request(StkScheduler *self, Entry *e, gboolean is_quote, StkRange range, gboolean for_detail)
{
  StkSchedAction a;
  memset(&a, 0, sizeof a);
  a.type = is_quote ? STK_SCHED_FETCH_QUOTE : STK_SCHED_FETCH_SERIES;
  a.id = self->next_id++;
  g_strlcpy(a.symbol, e->symbol, sizeof a.symbol);
  a.kind = e->kind;
  a.range = is_quote ? STK_RANGE_1D : range;

  e->req_id = a.id;
  e->req_is_quote = is_quote;
  e->req_range = range;
  e->req_for_detail = for_detail;
  e->req_cancelling = FALSE;
  if (is_quote) {
    e->wants &= ~(guint)WANT_QUOTE;
  } else {
    if (range == STK_SCHED_SPARK_RANGE)
      e->wants &= ~(guint)WANT_SPARK;
    if (for_detail)
      e->wants &= ~(guint)WANT_DETAIL;
  }
  self->n_inflight++;
  emit(self, &a);
}

/* Starts the highest-priority waiting job that has a free symbol slot.
 * Priority: the selection's QUOTE, the selection's DETAIL, every other QUOTE
 * (list order), every SPARK (list order). Returns FALSE if nothing started. */
static gboolean
start_next(StkScheduler *self)
{
  for (guint pass = 0; pass < 4; pass++) {
    for (guint i = 0; i < self->entries->len; i++) {
      Entry *e = g_ptr_array_index(self->entries, i);
      if (e->removed || e->req_id != 0)
        continue;
      gboolean sel = is_selected(self, e);
      switch (pass) {
      case 0:
        if (sel && (e->wants & WANT_QUOTE)) {
          start_request(self, e, TRUE, STK_RANGE_1D, FALSE);
          return TRUE;
        }
        break;
      case 1:
        if (sel && (e->wants & WANT_DETAIL)) {
          if (detail_valid(self, e)) {
            start_request(self, e, FALSE, self->sel_range, TRUE);
            return TRUE;
          }
          e->wants &= ~(guint)WANT_DETAIL;
        }
        break;
      case 2:
        if (e->wants & WANT_QUOTE) {
          start_request(self, e, TRUE, STK_RANGE_1D, FALSE);
          return TRUE;
        }
        break;
      default:
        if (e->wants & WANT_SPARK) {
          /* The selection's DETAIL job for the same range is this request. */
          gboolean detail = (e->wants & WANT_DETAIL) && detail_valid(self, e) &&
                            self->sel_range == STK_SCHED_SPARK_RANGE;
          start_request(self, e, FALSE, STK_SCHED_SPARK_RANGE, detail);
          return TRUE;
        }
        break;
      }
    }
  }
  return FALSE;
}

static void
pump(StkScheduler *self)
{
  if (!is_active(self))
    return;
  while (self->n_inflight < STK_SCHED_MAX_INFLIGHT && start_next(self))
    ;
}

/* ------------------------------------------------------------------------ */
/* Rounds, time, deadline                                                   */
/* ------------------------------------------------------------------------ */

/* Has the latest request of job @bit of @e succeeded less than
 * STK_SCHED_MIN_EVENT_GAP_US before @now_us? */
static gboolean
job_is_fresh(const Entry *e, guint bit, gint64 now_us)
{
  gint job = g_bit_nth_lsf(bit, -1);
  return (e->fresh & bit) != 0 && sat_sub(now_us, e->ok_us[job]) < STK_SCHED_MIN_EVENT_GAP_US;
}

/* Records how a request ended for the jobs @jobs it delivered. A success
 * makes a job fresh; a failure makes it stale and not fresh; a cancellation
 * changes nothing (the job simply never arrived). */
static void
record_result(Entry *e, guint jobs, StkSchedResult result, gint64 now_us)
{
  for (guint job = 0; job < N_JOBS; job++) {
    guint bit = 1u << job;
    if ((jobs & bit) == 0)
      continue;
    if (result == STK_SCHED_RESULT_OK) {
      e->failed &= ~bit;
      e->fresh |= bit;
      e->ok_us[job] = now_us;
    } else if (result == STK_SCHED_RESULT_FAILED) {
      e->failed |= bit;
      e->fresh &= ~bit;
    }
  }
}

/* Event round (@event): every job. Periodic round: QUOTE and DETAIL of
 * non-FUND symbols, and every job that failed (any symbol, funds included).
 * Each job is judged on its own: one whose latest request succeeded very
 * recently is skipped, any other (never fetched, cancelled, dropped by hide /
 * offline, failed) is wanted, whatever the state of the symbol's other jobs. */
static void
add_round(StkScheduler *self, StkSchedTime now, gboolean event)
{
  static const guint jobs[] = {WANT_QUOTE, WANT_SPARK, WANT_DETAIL};
  for (guint i = 0; i < self->entries->len; i++) {
    Entry *e = g_ptr_array_index(self->entries, i);
    if (e->removed)
      continue;
    for (guint j = 0; j < G_N_ELEMENTS(jobs); j++) {
      guint bit = jobs[j];
      if (bit == WANT_DETAIL && !detail_valid(self, e))
        continue;
      gboolean periodic = e->kind != STK_KIND_FUND && bit != WANT_SPARK;
      if (!event && !periodic && (e->failed & bit) == 0)
        continue;
      if (!job_is_fresh(e, bit, now.mono_us))
        want(self, e, bit);
    }
  }
}

/* Cancels everything in flight and forgets everything waiting. */
static void
drop_all(StkScheduler *self)
{
  for (guint i = 0; i < self->entries->len; i++) {
    Entry *e = g_ptr_array_index(self->entries, i);
    e->wants = 0;
    cancel_request(self, e);
  }
}

/* Is there anything a periodic round would do: a non-FUND symbol, or a job
 * that failed? */
static gboolean
has_periodic_work(const StkScheduler *self)
{
  for (guint i = 0; i < self->entries->len; i++) {
    const Entry *e = g_ptr_array_index(self->entries, i);
    if (!e->removed && (e->kind != STK_KIND_FUND || e->failed != 0))
      return TRUE;
  }
  return FALSE;
}

/* Re-evaluates whether the periodic timer may exist. */
static void
rearm(StkScheduler *self, StkSchedTime now)
{
  if (is_active(self) && has_periodic_work(self) && market_open(now.wall_ms)) {
    if (self->deadline_us < 0)
      self->deadline_us = sat_add(now.mono_us, STK_SCHED_PERIOD_US);
  } else {
    self->deadline_us = -1;
  }
}

/* First step of every input: clock-jump detection. */
static void
advance(StkScheduler *self, StkSchedTime now)
{
  if (self->has_last && self->deadline_us >= 0) {
    gint64 drift_ms = sat_sub(sat_sub(now.wall_ms, self->last.wall_ms),
                              sat_sub(now.mono_us, self->last.mono_us) / 1000);
    if (drift_ms > STK_SCHED_JUMP_TOLERANCE_US / 1000)
      self->deadline_us = now.mono_us;
  }
  self->last = now;
  self->has_last = TRUE;
}

static void
finish(StkScheduler *self, StkSchedTime now)
{
  pump(self);
  rearm(self, now);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

StkScheduler *
stk_scheduler_new(void)
{
  StkScheduler *self = g_new0(StkScheduler, 1);
  self->entries = g_ptr_array_new_with_free_func(g_free);
  self->sel_range = STK_RANGE_1D;
  self->next_id = 1;
  self->deadline_us = -1;
  return self;
}

void
stk_scheduler_free(StkScheduler *self)
{
  if (self == NULL)
    return;
  g_ptr_array_unref(self->entries);
  if (self->actions != NULL)
    g_array_unref(self->actions);
  g_free(self);
}

void
stk_scheduler_set_symbols(StkScheduler *self, StkSchedTime now, const StkSchedSymbol *symbols,
                          gsize n)
{
  g_return_if_fail(self != NULL);
  g_return_if_fail(symbols != NULL || n == 0);
  advance(self, now);

  /* Entries move from the old array to @next (steal), so each is owned by
   * exactly one array at any time. */
  GPtrArray *next = g_ptr_array_new_with_free_func(g_free);
  for (gsize i = 0; i < n; i++) {
    char key[STK_SCHED_SYMBOL_BYTES];
    if (!normalise(symbols[i].symbol, key))
      continue;
    gboolean dup = FALSE;
    for (guint j = 0; j < next->len && !dup; j++)
      dup = strcmp(((const Entry *)g_ptr_array_index(next, j))->symbol, key) == 0;
    if (dup)
      continue;

    Entry *e = NULL;
    for (guint j = 0; j < self->entries->len && e == NULL; j++) {
      Entry *c = g_ptr_array_index(self->entries, j);
      if (strcmp(c->symbol, key) == 0)
        e = g_ptr_array_steal_index(self->entries, j);
    }
    gboolean fresh = e == NULL || e->removed; /* new, or revived while its request winds down */
    if (e == NULL) {
      e = g_new0(Entry, 1);
      memcpy(e->symbol, key, sizeof e->symbol);
    }
    e->kind = symbols[i].kind;
    e->removed = FALSE;
    if (fresh) {
      e->wants = 0;
      e->failed = 0;
      e->fresh = 0;
      if (is_active(self)) {
        e->wants = WANT_QUOTE | WANT_SPARK;
        if (detail_valid(self, e))
          e->wants |= WANT_DETAIL;
      }
    }
    g_ptr_array_add(next, e);
  }

  /* What is left in the old array left the tracked set. */
  while (self->entries->len > 0) {
    Entry *e = g_ptr_array_steal_index(self->entries, self->entries->len - 1);
    e->wants = 0;
    cancel_request(self, e);
    if (e->req_id != 0) {
      e->removed = TRUE; /* keeps its slot until the request is reported done */
      g_ptr_array_add(next, e);
    } else {
      g_free(e);
    }
  }
  g_ptr_array_unref(self->entries);
  self->entries = next;

  finish(self, now);
}

void
stk_scheduler_set_selection(StkScheduler *self, StkSchedTime now, const char *symbol,
                            StkRange range)
{
  g_return_if_fail(self != NULL);
  g_return_if_fail(range >= STK_RANGE_1D && range <= STK_RANGE_1Y);
  advance(self, now);

  char key[STK_SCHED_SYMBOL_BYTES];
  gboolean has_sel = normalise(symbol, key);
  if (has_sel == self->has_sel && range == self->sel_range &&
      (!has_sel || strcmp(key, self->sel_symbol) == 0)) {
    finish(self, now);
    return;
  }

  Entry *old_sel = NULL;
  if (self->has_sel)
    old_sel = find_entry(self, self->sel_symbol, FALSE);
  if (old_sel != NULL) {
    if (old_sel->req_id != 0 && !old_sel->req_is_quote && old_sel->req_for_detail &&
        !old_sel->req_cancelling) {
      cancel_request(self, old_sel);
      if (old_sel->req_range == STK_SCHED_SPARK_RANGE)
        old_sel->wants |= WANT_SPARK; /* the sparkline still needs it */
    }
    old_sel->wants &= ~(guint)WANT_DETAIL;
    old_sel->failed &= ~(guint)WANT_DETAIL;
    old_sel->fresh &= ~(guint)WANT_DETAIL; /* the DETAIL job is gone */
  }

  self->has_sel = has_sel;
  if (has_sel)
    memcpy(self->sel_symbol, key, sizeof self->sel_symbol);
  self->sel_range = range;

  if (has_sel) {
    Entry *e = find_entry(self, key, FALSE);
    if (e != NULL) {
      e->failed &= ~(guint)WANT_DETAIL;
      e->fresh &= ~(guint)WANT_DETAIL; /* a new DETAIL job: nothing is fresh yet */
      if (is_active(self) && detail_valid(self, e))
        want(self, e, WANT_DETAIL);
    }
  }
  finish(self, now);
}

void
stk_scheduler_set_visible(StkScheduler *self, StkSchedTime now, gboolean visible)
{
  g_return_if_fail(self != NULL);
  advance(self, now);
  visible = !!visible;
  if (visible != self->visible) {
    self->visible = visible;
    if (!visible)
      drop_all(self);
    else if (is_active(self))
      add_round(self, now, TRUE);
  }
  finish(self, now);
}

void
stk_scheduler_set_online(StkScheduler *self, StkSchedTime now, gboolean online)
{
  g_return_if_fail(self != NULL);
  advance(self, now);
  online = !!online;
  if (online != self->online) {
    self->online = online;
    if (!online)
      drop_all(self);
    else if (is_active(self))
      add_round(self, now, TRUE);
  }
  finish(self, now);
}

void
stk_scheduler_focus_regained(StkScheduler *self, StkSchedTime now)
{
  g_return_if_fail(self != NULL);
  advance(self, now);
  if (is_active(self))
    add_round(self, now, TRUE);
  finish(self, now);
}

void
stk_scheduler_tick(StkScheduler *self, StkSchedTime now)
{
  g_return_if_fail(self != NULL);
  advance(self, now);
  if (self->deadline_us >= 0 && now.mono_us >= self->deadline_us) {
    gint64 next = sat_add(self->deadline_us, STK_SCHED_PERIOD_US);
    if (next <= now.mono_us)
      next = sat_add(now.mono_us, STK_SCHED_PERIOD_US); /* late: no catch-up rounds */
    if (is_active(self) && market_open(now.wall_ms))
      add_round(self, now, FALSE);
    self->deadline_us = next; /* rearm() clears it if the conditions are gone */
  }
  finish(self, now);
}

void
stk_scheduler_request_done(StkScheduler *self, StkSchedTime now, guint64 id, StkSchedResult result)
{
  g_return_if_fail(self != NULL);
  advance(self, now);

  Entry *e = NULL;
  if (id != 0) {
    for (guint i = 0; i < self->entries->len && e == NULL; i++) {
      Entry *c = g_ptr_array_index(self->entries, i);
      if (c->req_id == id)
        e = c;
    }
  }
  if (e == NULL) {
    finish(self, now);
    return;
  }

  /* The jobs this request delivers: the owner applies a series to the
   * sparkline when it has the SPARK range and to the detail pane when it has
   * the selected range, whichever job started it. */
  guint jobs = 0;
  if (e->req_is_quote) {
    jobs = WANT_QUOTE;
  } else {
    if (e->req_range == STK_SCHED_SPARK_RANGE)
      jobs |= WANT_SPARK;
    if (detail_valid(self, e) && e->req_range == self->sel_range)
      jobs |= WANT_DETAIL;
  }
  record_result(e, jobs, result, now.mono_us);

  e->req_id = 0;
  e->req_cancelling = FALSE;
  self->n_inflight--;
  if (e->removed)
    g_ptr_array_remove(self->entries, e);
  finish(self, now);
}

GArray *
stk_scheduler_take_actions(StkScheduler *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  GArray *out = self->actions;
  self->actions = NULL;
  return out;
}

gint64
stk_scheduler_next_wakeup_us(StkScheduler *self)
{
  g_return_val_if_fail(self != NULL, -1);
  return self->deadline_us;
}

gboolean
stk_scheduler_is_stale(StkScheduler *self, const char *symbol)
{
  g_return_val_if_fail(self != NULL, FALSE);
  char key[STK_SCHED_SYMBOL_BYTES];
  if (!normalise(symbol, key))
    return FALSE;
  const Entry *e = find_entry(self, key, FALSE);
  return e != NULL && e->failed != 0;
}

guint
stk_scheduler_n_inflight(StkScheduler *self)
{
  g_return_val_if_fail(self != NULL, 0);
  return self->n_inflight;
}
