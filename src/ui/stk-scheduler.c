/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
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
  StkKind req_kind;        /* kind the request was issued with (see n_inflight_index) */
} Entry;

struct _StkScheduler {
  GPtrArray *entries; /* Entry*, display order, removed ones last */
  gboolean has_sel;
  char sel_symbol[STK_SCHED_SYMBOL_BYTES];
  StkRange sel_range;
  gboolean visible;
  gboolean online;
  guint n_inflight;
  guint n_inflight_index; /* outstanding requests issued with kind STK_KIND_INDEX */
  guint64 next_id;
  gint64 deadline_us; /* -1: none */
  gboolean has_last;
  StkSchedTime last;
  GArray *actions; /* StkSchedAction, NULL when empty */
};

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

/**
 * sat_add:
 * @a: first addend.
 * @b: second addend.
 *
 * Adds two microsecond values without signed overflow.
 *
 * Preconditions:
 *  - none; every gint64 pair is accepted.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: @a + @b, or G_MAXINT64 / G_MININT64 when the exact sum would be
 *   above / below the gint64 range.
 */
static gint64
sat_add(gint64 a, gint64 b)
{
  if (b > 0 && a > G_MAXINT64 - b)
    return G_MAXINT64;
  if (b < 0 && a < G_MININT64 - b)
    return G_MININT64;
  return a + b;
}

/**
 * sat_sub:
 * @a: minuend.
 * @b: subtrahend.
 *
 * Subtracts two microsecond/millisecond values without signed overflow.
 *
 * Preconditions:
 *  - none; every gint64 pair is accepted.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: @a - @b, or G_MAXINT64 / G_MININT64 when the exact difference would
 *   be above / below the gint64 range.
 */
static gint64
sat_sub(gint64 a, gint64 b)
{
  if (b < 0 && a > G_MAXINT64 + b)
    return G_MAXINT64;
  if (b > 0 && a < G_MININT64 + b)
    return G_MININT64;
  return a - b;
}

/**
 * normalise:
 * @symbol: (nullable): candidate ticker (any case).
 * @out: (out caller-allocates) (array fixed-size=16): receives the upper-cased
 *   symbol; STK_SCHED_SYMBOL_BYTES bytes.
 *
 * Copies the upper-cased symbol into @out; FALSE if it is not valid. Every
 * symbol buffer of the scheduler (Entry.symbol, sel_symbol and the keys that
 * are compared with them) is produced here, which is what makes the fixed-size
 * comparisons and copies of this file well defined.
 *
 * Preconditions:
 *  - @symbol is NULL, NUL-terminated, or has at least 16 readable bytes (the
 *    contract of stk_symbol_is_valid(), which is the only code that reads it
 *    before the validity decision).
 *  - @out points to STK_SCHED_SYMBOL_BYTES writable bytes.
 * Postconditions:
 *  - on TRUE: @out holds the symbol upper-cased, NUL-terminated at an index of
 *    at most STK_SCHED_SYMBOL_BYTES - 1, and ALL STK_SCHED_SYMBOL_BYTES bytes
 *    of @out are initialised (every byte after the NUL is 0), so the buffer can
 *    be copied and compared as a whole (memcpy / memcmp).
 *  - on FALSE: @out is untouched.
 * Thread affinity: any thread (pure).
 * Ownership: @symbol is borrowed; @out is filled for the caller.
 *
 * Returns: TRUE iff @symbol satisfies stk_symbol_is_valid().
 */
static gboolean
normalise(const char *symbol, char out[STK_SCHED_SYMBOL_BYTES])
{
  if (!stk_symbol_is_valid(symbol))
    return FALSE;
  memset(out, 0, STK_SCHED_SYMBOL_BYTES);
  gsize i = 0;
  for (; i + 1 < STK_SCHED_SYMBOL_BYTES && symbol[i] != '\0'; i++)
    out[i] = g_ascii_toupper(symbol[i]);
  out[i] = '\0';
  return TRUE;
}

/**
 * market_open:
 * @wall_ms: wall-clock time, milliseconds since the Unix epoch UTC.
 *
 * stk_market_is_open() for any wall clock: a time outside the supported
 * calendar counts as "market closed" (stk_market_is_open() would log a
 * critical there).
 *
 * Preconditions:
 *  - none; every gint64 is accepted.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: TRUE iff @wall_ms is inside the supported calendar and the regular
 *   session of that instant is open.
 */
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

/**
 * is_active:
 * @self: (not nullable): the scheduler.
 *
 * Preconditions:
 *  - @self is a valid scheduler (every caller dereferences it first).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none; @self is only read.
 *
 * Returns: TRUE iff the window is visible AND the network is available.
 */
static gboolean
is_active(const StkScheduler *self)
{
  return self->visible && self->online;
}

/**
 * find_entry:
 * @self: (not nullable): the scheduler.
 * @key: (not nullable) (array fixed-size=16): a symbol buffer of
 *   STK_SCHED_SYMBOL_BYTES initialised bytes, as produced by normalise() (or
 *   the scheduler's own sel_symbol).
 * @include_removed: whether entries kept only to hold a request slot count.
 *
 * Looks up the entry of a symbol. Symbols are compared as whole
 * STK_SCHED_SYMBOL_BYTES buffers with memcmp(): that equals comparing the
 * NUL-terminated strings because every Entry.symbol and every key is zero
 * padded after its NUL (Entry.symbol is g_new0()-allocated and filled only by
 * memcpy() of a normalise()d key; keys come from normalise()).
 *
 * Preconditions:
 *  - @self is a valid scheduler; @key points to STK_SCHED_SYMBOL_BYTES readable,
 *    fully initialised bytes (zero after the NUL), as every caller passes it.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: the entry is owned by @self; @key is borrowed.
 *
 * Returns: (transfer none) (nullable): the first matching entry in display
 *   order (removed ones only when @include_removed), or NULL.
 */
static Entry *
find_entry(const StkScheduler *self, const char *key, gboolean include_removed)
{
  for (guint i = 0; i < self->entries->len; i++) {
    Entry *e = g_ptr_array_index(self->entries, i);
    if ((include_removed || !e->removed) &&
        memcmp(e->symbol, key, STK_SCHED_SYMBOL_BYTES) == 0)
      return e;
  }
  return NULL;
}

/**
 * is_selected:
 * @self: (not nullable): the scheduler.
 * @e: (not nullable): an entry of @self.
 *
 * Preconditions:
 *  - @self and @e are valid; @e->symbol and @self->sel_symbol are
 *    zero-padded fixed buffers (see find_entry()): sel_symbol is g_new0()-zero
 *    until stk_scheduler_set_selection() copies a normalise()d key into it.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none; both pointers are only read.
 *
 * Returns: TRUE iff a symbol is selected, @e is not a removed entry and its
 *   symbol equals the selected one (compared as whole buffers with memcmp()).
 */
static gboolean
is_selected(const StkScheduler *self, const Entry *e)
{
  return self->has_sel && !e->removed &&
         memcmp(e->symbol, self->sel_symbol, STK_SCHED_SYMBOL_BYTES) == 0;
}

/**
 * detail_valid:
 * @self: (not nullable): the scheduler.
 * @e: (not nullable): an entry of @self.
 *
 * Preconditions:
 *  - @self and @e are valid (see is_selected()).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none; both pointers are only read.
 *
 * Returns: TRUE iff @e is the selected entry and has a DETAIL job (a FUND has
 *   none for STK_RANGE_1D).
 */
static gboolean
detail_valid(const StkScheduler *self, const Entry *e)
{
  return is_selected(self, e) && !(e->kind == STK_KIND_FUND && self->sel_range == STK_RANGE_1D);
}

/**
 * job_in_flight:
 * @self: (not nullable): the scheduler.
 * @e: (not nullable): an entry of @self.
 * @bit: one WANT_QUOTE / WANT_SPARK / WANT_DETAIL constant.
 *
 * Is a (not cancelled) request in flight that already delivers job @bit?
 *
 * Preconditions:
 *  - @self and @e are valid; @bit is one of the WANT_* constants (any other
 *    value is answered FALSE by the switch).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none; both pointers are only read.
 *
 * Returns: TRUE iff @e has a request in flight that is not being cancelled and
 *   delivers @bit.
 */
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

/**
 * want:
 * @self: (not nullable): the scheduler.
 * @e: (not nullable): an entry of @self.
 * @bit: one WANT_* constant.
 *
 * Marks job @bit as waiting unless it is already in flight.
 *
 * Preconditions:
 *  - @self and @e are valid; @bit is one WANT_* constant.
 * Postconditions:
 *  - @e->wants has @bit set unless job_in_flight() was TRUE for it (then it is
 *    unchanged). Nothing else changes.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none; @self is only read, @e is modified in place.
 */
static void
want(const StkScheduler *self, Entry *e, guint bit)
{
  if (!job_in_flight(self, e, bit))
    e->wants |= bit;
}

/**
 * emit:
 * @self: (not nullable): the scheduler.
 * @action: (not nullable): the action to queue.
 *
 * Appends a copy of @action to the pending actions, creating the array on
 * first use.
 *
 * Preconditions:
 *  - @self is a valid scheduler; @action points to a fully initialised
 *    StkSchedAction (every caller memset()s it to 0 first).
 * Postconditions:
 *  - @self->actions is non-NULL and ends with a copy of @action.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: @action is borrowed and copied by value; the array is owned by
 *   @self until stk_scheduler_take_actions() hands it over.
 */
static void
emit(StkScheduler *self, const StkSchedAction *action)
{
  if (self->actions == NULL)
    self->actions = g_array_new(FALSE, FALSE, sizeof(StkSchedAction));
  g_array_append_val(self->actions, *action);
}

/**
 * cancel_request:
 * @self: (not nullable): the scheduler.
 * @e: (not nullable): an entry of @self.
 *
 * Queues a CANCEL action for the request of @e, once. The request keeps its
 * slot until stk_scheduler_request_done() reports it.
 *
 * Preconditions:
 *  - @self and @e are valid.
 * Postconditions:
 *  - if @e has a request in flight that is not already cancelling: it is marked
 *    cancelling and one CANCEL action carrying its id (symbol "") is queued;
 *    otherwise nothing changes.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none; @e is modified in place, the action is copied into @self.
 */
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

/**
 * start_request:
 * @self: (not nullable): the scheduler.
 * @e: (not nullable): an entry of @self that is not removed and has no request.
 * @is_quote: TRUE for a quote request, FALSE for a series request.
 * @range: the series range (ignored for a quote request).
 * @for_detail: whether the series request is also the DETAIL job.
 *
 * Issues a request for @e: queues the FETCH action with a fresh id, records the
 * request in @e and takes a slot.
 *
 * Preconditions:
 *  - @self and @e are valid; @e->req_id == 0 and @e is not removed; the caller
 *    (start_next(), reached only from pump()) has checked that
 *    @self->n_inflight < STK_SCHED_MAX_INFLIGHT and, for an INDEX entry, that
 *    @self->n_inflight_index < STK_SCHED_MAX_INFLIGHT_INDEX.
 *  - @e->symbol is NUL-terminated within its buffer (it comes from normalise()),
 *    so the g_strlcpy() into the equally sized action buffer copies it whole.
 * Postconditions:
 *  - one FETCH_QUOTE / FETCH_SERIES action is queued; its id is
 *    @self->next_id before the call, taken from a counter that starts at 1 and
 *    is only incremented here, so ids are never 0 and are never reused (a wrap
 *    would need 2^64 requests). The matching WANT_* bits of @e are cleared,
 *    @self->n_inflight (and n_inflight_index when @e is an INDEX entry) is
 *    incremented and @e->req_kind is the kind the request was issued with.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none; the action is copied into @self.
 */
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
  e->req_kind = a.kind;
  if (a.kind == STK_KIND_INDEX)
    self->n_inflight_index++;
  emit(self, &a);
}

/**
 * start_next:
 * @self: (not nullable): the scheduler.
 *
 * Starts the highest-priority waiting job that has a free symbol slot.
 * Priority: the selection's QUOTE, the selection's DETAIL, every other QUOTE
 * (list order), every SPARK (list order).
 *
 * Preconditions:
 *  - @self is a valid scheduler, active, with n_inflight below
 *    STK_SCHED_MAX_INFLIGHT (the only caller, pump(), checks both).
 * Postconditions:
 *  - at most one request was started through start_request(); a waiting DETAIL
 *    job of the selection that has no DETAIL job any more is dropped.
 *    Waiting jobs of an INDEX symbol are left wanted while the index slot is
 *    taken.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none; the entries stay owned by @self.
 *
 * Returns: TRUE iff a request was started; FALSE if nothing started.
 */
static gboolean
start_next(StkScheduler *self)
{
  for (guint pass = 0; pass < 4; pass++) {
    for (guint i = 0; i < self->entries->len; i++) {
      Entry *e = g_ptr_array_index(self->entries, i);
      if (e->removed || e->req_id != 0)
        continue;
      /* The index slot is taken: this job stays wanted, the others go on. */
      if (e->kind == STK_KIND_INDEX && self->n_inflight_index >= STK_SCHED_MAX_INFLIGHT_INDEX)
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

/**
 * pump:
 * @self: (not nullable): the scheduler.
 *
 * Starts waiting jobs while the scheduler is active and a slot is free.
 *
 * Preconditions:
 *  - @self is a valid scheduler.
 * Postconditions:
 *  - when not active nothing happens; otherwise start_next() ran until it
 *    started nothing or n_inflight reached STK_SCHED_MAX_INFLIGHT.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none.
 */
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

/**
 * job_is_fresh:
 * @e: (not nullable): an entry.
 * @bit: exactly one WANT_* constant.
 * @now_us: the current monotonic time, microseconds.
 *
 * Has the latest request of job @bit of @e succeeded less than
 * STK_SCHED_MIN_EVENT_GAP_US before @now_us?
 *
 * Preconditions:
 *  - @e is valid; @bit is exactly one of WANT_QUOTE, WANT_SPARK, WANT_DETAIL,
 *    so g_bit_nth_lsf() yields the index 0..N_JOBS-1 into @e->ok_us. Its only
 *    caller, add_round(), passes the constants of its static jobs[] table.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none; @e is only read.
 *
 * Returns: TRUE iff the job is marked fresh and its age is below the gap.
 */
static gboolean
job_is_fresh(const Entry *e, guint bit, gint64 now_us)
{
  gint job = g_bit_nth_lsf(bit, -1);
  return (e->fresh & bit) != 0 && sat_sub(now_us, e->ok_us[job]) < STK_SCHED_MIN_EVENT_GAP_US;
}

/**
 * record_result:
 * @e: (not nullable): the entry whose request ended.
 * @jobs: the WANT_* bits the request delivered.
 * @result: how the request ended.
 * @now_us: the current monotonic time, microseconds.
 *
 * Records how a request ended for the jobs @jobs it delivered. A success
 * makes a job fresh; a failure makes it stale and not fresh; a cancellation
 * changes nothing (the job simply never arrived).
 *
 * Preconditions:
 *  - @e is valid; @jobs only contains WANT_* bits (bits outside the N_JOBS
 *    jobs are ignored).
 * Postconditions:
 *  - OK: the jobs are not failed, are fresh and have ok_us == @now_us;
 *    FAILED: they are failed and not fresh; CANCELLED: nothing changes.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none; @e is modified in place.
 */
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

/**
 * add_round:
 * @self: (not nullable): the scheduler.
 * @now: the current time.
 * @event: TRUE for an event round, FALSE for a periodic round.
 *
 * Event round (@event): every job. Periodic round: QUOTE and DETAIL of
 * non-FUND symbols, and every job that failed (any symbol, funds included).
 * Each job is judged on its own: one whose latest request succeeded very
 * recently is skipped, any other (never fetched, cancelled, dropped by hide /
 * offline, failed) is wanted, whatever the state of the symbol's other jobs.
 * An event round that finds the market closed is itself the refresh after
 * the close, so the final round still owed to a pending deadline (see
 * stk_scheduler_tick()) is cancelled.
 *
 * Preconditions:
 *  - @self is a valid scheduler.
 * Postconditions:
 *  - the selected jobs of every tracked (not removed) symbol are marked wanted
 *    unless fresh or already in flight; for an event round with the market
 *    closed the deadline is cleared. Nothing is started (pump() does that).
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none; the entries stay owned by @self.
 */
static void
add_round(StkScheduler *self, StkSchedTime now, gboolean event)
{
  static const guint jobs[] = {WANT_QUOTE, WANT_SPARK, WANT_DETAIL};
  if (event && !market_open(now.wall_ms))
    self->deadline_us = -1;
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

/**
 * drop_all:
 * @self: (not nullable): the scheduler.
 *
 * Cancels everything in flight and forgets everything waiting.
 *
 * Preconditions:
 *  - @self is a valid scheduler.
 * Postconditions:
 *  - every entry has no waiting job; every request in flight that was not
 *    already cancelling is marked cancelling with one CANCEL action queued.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none; the entries stay owned by @self.
 */
static void
drop_all(StkScheduler *self)
{
  for (guint i = 0; i < self->entries->len; i++) {
    Entry *e = g_ptr_array_index(self->entries, i);
    e->wants = 0;
    cancel_request(self, e);
  }
}

/**
 * has_periodic_work:
 * @self: (not nullable): the scheduler.
 *
 * Is there anything a periodic round would do: a non-FUND symbol, or a job
 * that failed?
 *
 * Preconditions:
 *  - @self is a valid scheduler.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none; @self is only read.
 *
 * Returns: TRUE iff a tracked (not removed) entry is not a FUND or has a failed
 *   job.
 */
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

/**
 * rearm:
 * @self: (not nullable): the scheduler.
 * @now: the current time.
 *
 * Re-evaluates whether the periodic timer may exist. A deadline is armed only
 * while the market is open; once armed it survives the close, because when it
 * fires it issues the one final round (see stk_scheduler_tick()). It is
 * never armed (or re-armed) while the market is closed.
 *
 * Preconditions:
 *  - @self is a valid scheduler.
 * Postconditions:
 *  - the deadline is -1 unless the scheduler is active and has periodic work;
 *    when it was unset and the market is open it is @now.mono_us plus
 *    STK_SCHED_PERIOD_US (saturating).
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none.
 */
static void
rearm(StkScheduler *self, StkSchedTime now)
{
  if (!is_active(self) || !has_periodic_work(self))
    self->deadline_us = -1;
  else if (self->deadline_us < 0 && market_open(now.wall_ms))
    self->deadline_us = sat_add(now.mono_us, STK_SCHED_PERIOD_US);
}

/**
 * advance:
 * @self: (not nullable): the scheduler.
 * @now: the current time.
 *
 * First step of every input: clock-jump detection. If a deadline is pending
 * and the wall clock advanced more than STK_SCHED_JUMP_TOLERANCE_US more than
 * the monotonic clock since the previous input (differences computed with
 * saturating arithmetic), the deadline becomes due now.
 *
 * Preconditions:
 *  - @self is a valid scheduler.
 * Postconditions:
 *  - the deadline is @now.mono_us after a detected forward jump; @self->last is
 *    @now and has_last is TRUE.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none.
 */
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

/**
 * finish:
 * @self: (not nullable): the scheduler.
 * @now: the current time.
 *
 * Last step of every input: starts waiting jobs and re-evaluates the timer.
 *
 * Preconditions:
 *  - @self is a valid scheduler.
 * Postconditions:
 *  - pump() and rearm() have run.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none.
 */
static void
finish(StkScheduler *self, StkSchedTime now)
{
  pump(self);
  rearm(self, now);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

/* Documented in stk-scheduler.h. */
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

/* Documented in stk-scheduler.h. */
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

/* Documented in stk-scheduler.h. */
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
      dup = memcmp(((const Entry *)g_ptr_array_index(next, j))->symbol, key,
                   STK_SCHED_SYMBOL_BYTES) == 0;
    if (dup)
      continue;

    Entry *e = NULL;
    for (guint j = 0; j < self->entries->len && e == NULL; j++) {
      Entry *c = g_ptr_array_index(self->entries, j);
      if (memcmp(c->symbol, key, STK_SCHED_SYMBOL_BYTES) == 0)
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

/* Documented in stk-scheduler.h. */
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
      (!has_sel || memcmp(key, self->sel_symbol, STK_SCHED_SYMBOL_BYTES) == 0)) {
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

/* Documented in stk-scheduler.h. */
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

/* Documented in stk-scheduler.h. */
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

/* Documented in stk-scheduler.h. */
void
stk_scheduler_focus_regained(StkScheduler *self, StkSchedTime now)
{
  g_return_if_fail(self != NULL);
  advance(self, now);
  if (is_active(self))
    add_round(self, now, TRUE);
  finish(self, now);
}

/* Documented in stk-scheduler.h. */
void
stk_scheduler_tick(StkScheduler *self, StkSchedTime now)
{
  g_return_if_fail(self != NULL);
  advance(self, now);
  if (self->deadline_us >= 0 && now.mono_us >= self->deadline_us) {
    gint64 next = sat_add(self->deadline_us, STK_SCHED_PERIOD_US);
    if (next <= now.mono_us)
      next = sat_add(now.mono_us, STK_SCHED_PERIOD_US); /* late: no catch-up rounds */
    /* Open: the periodic round. Closed: the market closed since the round
     * that armed this deadline (an event round after the close would have
     * cleared it), so this is the final round and nothing is armed after it. */
    gboolean open = market_open(now.wall_ms);
    if (is_active(self))
      add_round(self, now, FALSE);
    self->deadline_us = open ? next : -1; /* rearm() clears it if the conditions are gone */
  }
  finish(self, now);
}

/* Documented in stk-scheduler.h. */
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
  if (e->req_kind == STK_KIND_INDEX)
    self->n_inflight_index--;
  if (e->removed)
    g_ptr_array_remove(self->entries, e);
  finish(self, now);
}

/* Documented in stk-scheduler.h. */
GArray *
stk_scheduler_take_actions(StkScheduler *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  GArray *out = self->actions;
  self->actions = NULL;
  return out;
}

/* Documented in stk-scheduler.h. */
gint64
stk_scheduler_next_wakeup_us(StkScheduler *self)
{
  g_return_val_if_fail(self != NULL, -1);
  return self->deadline_us;
}

/* Documented in stk-scheduler.h. */
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

/* Documented in stk-scheduler.h. */
guint
stk_scheduler_n_inflight(StkScheduler *self)
{
  g_return_val_if_fail(self != NULL, 0);
  return self->n_inflight;
}

/* Documented in stk-scheduler.h. */
gboolean
stk_sched_series_is_hollow(const StkSeries *fresh, const StkSeries *held)
{
  g_return_val_if_fail(fresh != NULL, FALSE);
  return fresh->range == STK_RANGE_1D && fresh->n_points == 0 && held != NULL &&
         held->range == STK_RANGE_1D && held->n_points > 0;
}
