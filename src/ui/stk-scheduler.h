/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-scheduler.h - StkScheduler: pure refresh-policy logic of the stocks app.
 *
 * The scheduler decides WHEN data is fetched; it never fetches, never touches
 * GLib's main loop, never reads a clock and has no timers. The owner feeds it
 * inputs (every call carries the current time as a StkSchedTime), then drains
 * the resulting actions with stk_scheduler_take_actions() and arms at most one
 * timer for stk_scheduler_next_wakeup_us(). Because time is an argument, every
 * rule is deterministic and unit-tested (test-scheduler.c).
 *
 * Threading: not thread-safe; use it from one thread (the main thread).
 * Everything is main-loop-free pure logic over memory: no I/O of any kind.
 * Only stk_sched_series_is_hollow() takes no scheduler and may be called from
 * any thread (it just reads its two arguments).
 *
 * Data wanted per symbol ("jobs")
 * -------------------------------
 *   QUOTE   the quote (row price/change, detail header).
 *   SPARK   the STK_SCHED_SPARK_RANGE (1M) series (the row sparkline).
 *   DETAIL  the series of the SELECTED symbol for the selected range.
 * A DETAIL job whose range equals STK_SCHED_SPARK_RANGE is the same request as
 * the SPARK job: it is issued once and satisfies both. A selected FUND with
 * STK_RANGE_1D has no DETAIL job (funds have no intraday data).
 *
 * Rules
 * -----
 *  - A request is issued only while the window is visible AND the network is
 *    available ("active"). While not active the scheduler emits no FETCH
 *    action and has no wakeup.
 *  - Refresh rounds. An "event round" (all jobs: QUOTE and SPARK of every
 *    symbol, DETAIL of the selection) happens exactly once for each of: the
 *    window becoming visible (also the app start), the window regaining focus
 *    while visible and online, and the network coming back while visible. A
 *    "periodic round" (QUOTE of every non-FUND symbol and DETAIL of a
 *    non-FUND selection, plus every job that has failed, funds included;
 *    funds publish one NAV a day and the 1M series changes once a day, so a
 *    healthy fund is left alone) happens every STK_SCHED_PERIOD_US, but ONLY
 *    while the market is open (stk_market_is_open() of the wall clock), the
 *    window is visible and the network is available. One more periodic
 *    round, the "final round", follows the close (see Timer).
 *  - Freshness is per job. Of the jobs a round includes, one whose latest
 *    request succeeded less than STK_SCHED_MIN_EVENT_GAP_US ago is skipped
 *    (alt-tab bursts do not cause repeated requests) and every other one is
 *    wanted. In particular a job that has not arrived since its request was
 *    cancelled (window hidden, network lost, selection changed) or its
 *    waiting was dropped, and a job whose latest request failed, is wanted
 *    even if the quote of the same symbol is fresh. The DETAIL job starts
 *    over (nothing fresh, no failure) whenever the selection changes.
 *  - Timer: stk_scheduler_next_wakeup_us() is the monotonic deadline of the
 *    next periodic round while "active AND market open AND (at least one
 *    non-FUND symbol OR at least one failed job)". A deadline is armed only
 *    while the market is open, and one that is armed survives the close
 *    (while the conditions other than "market open" still hold): when it
 *    fires with the market closed, exactly ONE final round is issued (the
 *    periodic round's jobs: the official close of a window left visible
 *    across 16:00 ET), after which there is no deadline. Nothing is armed
 *    while the market is closed, so there is no polling after the close, and
 *    an event round that finds the market closed (it is itself a refresh
 *    after the close) cancels the final round still owed. Hiding the window
 *    or losing the network clears the deadline, so no final round happens
 *    then either (showing / reconnecting is an event round). The scheduler
 *    does not wake itself at the next market open: while the market is
 *    closed there is no timer, so a window left alone across the open starts
 *    refreshing only after the next call of a mutator (event round,
 *    selection change, ...) that finds the market open; the first periodic
 *    round then follows one period later.
 *  - In flight: at most one request per symbol, at most
 *    STK_SCHED_MAX_INFLIGHT in total. A job that is already in flight (or
 *    already waiting) is merged: asking again does not start a second
 *    request. A cancelled request keeps its slot until its completion is
 *    reported with stk_scheduler_request_done(), so a replacement request of
 *    the same symbol never overlaps it.
 *  - Index requests: at most STK_SCHED_MAX_INFLIGHT_INDEX request issued
 *    with kind STK_KIND_INDEX is outstanding at any moment, and it counts
 *    toward STK_SCHED_MAX_INFLIGHT. The kind is the one the request was
 *    issued with, so changing a symbol's kind (stk_scheduler_set_symbols())
 *    while it is in flight does not allow a second one. Outstanding means
 *    not yet reported finished by stk_scheduler_request_done(): a cancelled
 *    request and the request of a removed symbol still count. While one is
 *    outstanding, the waiting jobs of INDEX symbols are skipped (they stay
 *    wanted, are not failed or dropped) and the waiting jobs of every other
 *    symbol start as usual, so a slow index source can hold only one slot;
 *    the skipped jobs start, in the usual priority order, when it is
 *    reported finished. The rule looks only at the kind: no clock, timer or
 *    pacing.
 *  - Failure: a FAILED completion marks the symbol stale
 *    (stk_scheduler_is_stale()) until a request of the same job succeeds or,
 *    for the DETAIL job, the selection changes; the owner keeps showing its
 *    cached data. Nothing retries a failure by itself: the next round that
 *    includes the job does, i.e. every event round and, while its conditions
 *    hold (market open, visible, online), every periodic round; with the
 *    market closed only an event round does. CANCELLED completions never
 *    mark stale.
 *  - Stale marks. The mark of a symbol can change only in these calls, so
 *    the owner has to re-read stk_scheduler_is_stale() of exactly these
 *    symbols after them: stk_scheduler_request_done() (the symbol of that
 *    request); stk_scheduler_set_selection() (the previously selected symbol
 *    AND the newly selected one: the failure of the DETAIL job of each is
 *    forgotten); stk_scheduler_set_symbols() (an added symbol starts without
 *    a mark, a removed one has none). No other call changes a mark.
 *  - Cancellation: hiding the window or losing the network cancels every
 *    request in flight and drops everything waiting; the jobs concerned have
 *    not arrived, so the next event round wants them again (see Freshness).
 *    Changing the selection (symbol or range) cancels the in-flight request
 *    that was started for the DETAIL job of the previous selection (a request
 *    that was started as the SPARK job keeps running); removing a symbol
 *    cancels its request.
 *  - Clock jumps: deadlines use the monotonic clock only. If the wall clock
 *    advanced more than STK_SCHED_JUMP_TOLERANCE_US more than the monotonic
 *    clock since the previous call (suspend/resume, clock set forward), a
 *    pending deadline becomes due at once (one round, never a burst; if the
 *    jump left the market closed, that round is the final round). A wall
 *    clock that moves backwards changes nothing but the market-open
 *    evaluation (moved into closed hours, the pending deadline fires as the
 *    final round). A wall clock outside the supported calendar
 *    (STK_MARKET_MIN_YEAR..STK_MARKET_MAX_YEAR) counts as "market closed".
 *    A periodic round that fires late is followed by the next one a full
 *    period later, never by catch-up rounds.
 */
#ifndef STK_SCHEDULER_H
#define STK_SCHEDULER_H

#include <glib.h>

#include "stk-types.h"

G_BEGIN_DECLS

/** Period of the periodic round, microseconds. */
#define STK_SCHED_PERIOD_US (60 * G_USEC_PER_SEC)
/** Maximum number of requests outstanding at once (all symbols). */
#define STK_SCHED_MAX_INFLIGHT 4
/** Maximum number of outstanding requests issued with kind STK_KIND_INDEX (they count toward
 *  STK_SCHED_MAX_INFLIGHT). */
#define STK_SCHED_MAX_INFLIGHT_INDEX 1
/** A job whose latest request succeeded less than this ago is skipped by rounds. */
#define STK_SCHED_MIN_EVENT_GAP_US (5 * G_USEC_PER_SEC)
/** Wall-minus-monotonic drift above which a forward clock jump is assumed. */
#define STK_SCHED_JUMP_TOLERANCE_US (10 * G_USEC_PER_SEC)
/** Range of the series that feeds the row sparklines. */
#define STK_SCHED_SPARK_RANGE STK_RANGE_1M
/** Size of StkSchedAction.symbol: longest valid symbol (15 bytes) plus NUL. */
#define STK_SCHED_SYMBOL_BYTES 16

typedef struct _StkScheduler StkScheduler;

/** The instant an input happens at; both clocks are read by the caller. */
typedef struct {
  gint64 mono_us; /**< g_get_monotonic_time()                          */
  gint64 wall_ms; /**< real time, milliseconds since the Unix epoch UTC */
} StkSchedTime;

/** One tracked instrument (input of stk_scheduler_set_symbols()). */
typedef struct {
  const char *symbol; /**< must satisfy stk_symbol_is_valid(); case-insensitive */
  StkKind kind;
} StkSchedSymbol;

typedef enum {
  STK_SCHED_FETCH_QUOTE,  /**< start stk_provider_fetch_quote_async()  */
  STK_SCHED_FETCH_SERIES, /**< start stk_provider_fetch_series_async() */
  STK_SCHED_CANCEL        /**< cancel the request with this id         */
} StkSchedActionType;

/** An instruction for the owner; plain data, no pointers. */
typedef struct {
  StkSchedActionType type;
  /** FETCH_*: id of the new request (never 0, never reused); CANCEL: id of
   *  the request to cancel. */
  guint64 id;
  /** FETCH_*: upper-case symbol, NUL-terminated; CANCEL: "". */
  char symbol[STK_SCHED_SYMBOL_BYTES];
  StkKind kind;   /**< FETCH_*: the symbol's kind                  */
  StkRange range; /**< FETCH_SERIES: the range to fetch            */
} StkSchedAction;

/** Outcome of a request (input of stk_scheduler_request_done()). */
typedef enum {
  STK_SCHED_RESULT_OK,
  STK_SCHED_RESULT_FAILED,   /**< any error except cancellation */
  STK_SCHED_RESULT_CANCELLED /**< ended by cancellation          */
} StkSchedResult;

/**
 * stk_scheduler_new:
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - the scheduler is hidden, offline, has no symbols, no selection (range
 *    STK_RANGE_1D), no pending action and no deadline; the first request id
 *    it will issue is 1.
 * Thread affinity: the thread that will use it (main thread); the scheduler is
 *   not thread-safe.
 * Ownership: the caller owns the result; release it with
 *   stk_scheduler_free().
 *
 * Returns: (transfer full): a scheduler that is hidden, offline, has no
 *   symbols and no selection (so it emits nothing until told otherwise).
 */
StkScheduler *stk_scheduler_new(void);

/**
 * stk_scheduler_free:
 * @self: (nullable) (transfer full): the scheduler to free, or NULL.
 *
 * Frees @self; NULL is allowed. Requests still in flight are forgotten.
 *
 * Preconditions:
 *  - @self is NULL or a scheduler from stk_scheduler_new() that is not used
 *    again; no other thread uses it.
 * Postconditions:
 *  - everything the scheduler owns is freed, including actions that were never
 *    taken; an array already returned by stk_scheduler_take_actions() is not
 *    affected. Nothing is cancelled: the owner cancels its own requests.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: consumes @self.
 */
void stk_scheduler_free(StkScheduler *self);

/**
 * stk_scheduler_set_symbols:
 * @self: (not nullable): the scheduler.
 * @now: the current time.
 * @symbols: (nullable) (array length=n): the tracked instruments in display
 *   order; elements with an invalid symbol and repeats (ASCII
 *   case-insensitive) are ignored. May be NULL when @n is 0.
 * @n: number of elements.
 *
 * Replaces the tracked set. Kept symbols keep their state; a removed symbol's
 * request in flight is cancelled; while active, new symbols get an
 * immediate QUOTE, SPARK (and DETAIL when selected) job. The list order is
 * the order in which waiting jobs of equal priority are started.
 *
 * Preconditions:
 *  - @self is a scheduler (else: critical, nothing changes); @symbols is
 *    non-NULL or @n is 0 (else: critical, nothing changes).
 *  - @symbols points to @n readable StkSchedSymbol; each .symbol is NULL,
 *    NUL-terminated or has at least 16 readable bytes (anything else but a
 *    valid ticker is ignored, see above).
 *  - each .kind is an StkKind: the scheduler only compares it with
 *    STK_KIND_FUND and STK_KIND_INDEX and copies it into the actions, it never
 *    uses it as an index, so another value is stored as is and treated like a
 *    stock; the provider rejects it with STK_ERROR_INVALID_ARG when it is
 *    fetched.
 * Postconditions:
 *  - the tracked set is the valid, de-duplicated elements (symbols stored
 *    upper-cased in the scheduler's own copies); @symbols and the strings it
 *    points to are not retained. Pending actions may have been queued (CANCEL
 *    for removed symbols, FETCH_* for new ones while active); the deadline is
 *    re-evaluated.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: all pointers are borrowed for the call only.
 */
void stk_scheduler_set_symbols(StkScheduler *self, StkSchedTime now,
                               const StkSchedSymbol *symbols, gsize n);

/**
 * stk_scheduler_set_selection:
 * @self: (not nullable): the scheduler.
 * @now: the current time.
 * @symbol: (nullable): the selected symbol, or NULL / an invalid symbol for
 *   none. It need not be tracked (yet).
 * @range: the selected chart range (a valid StkRange).
 *
 * No-op when neither symbol nor range changed. Otherwise cancels the previous
 * selection's DETAIL request in flight and, while active, starts the DETAIL
 * job of the new selection (not for a FUND with STK_RANGE_1D). The stale
 * marks of the DETAIL job of the previous AND of the new selected symbol are
 * cleared: re-read stk_scheduler_is_stale() of both.
 *
 * Preconditions:
 *  - @self is a scheduler and @range is STK_RANGE_1D..STK_RANGE_1Y (else:
 *    critical, nothing changes); @symbol is NULL, NUL-terminated or has at
 *    least 16 readable bytes.
 * Postconditions:
 *  - the selection is the upper-cased valid @symbol (none if @symbol is NULL or
 *    invalid) with @range; @symbol is not retained. Actions may have been
 *    queued and the deadline re-evaluated.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: @symbol is borrowed for the call only.
 */
void stk_scheduler_set_selection(StkScheduler *self, StkSchedTime now,
                                 const char *symbol, StkRange range);

/**
 * stk_scheduler_set_visible:
 * @self: (not nullable): the scheduler.
 * @now: the current time.
 * @visible: whether the window is mapped and not minimised/suspended.
 *
 * No-op when unchanged. TRUE starts an event round (when online); FALSE
 * cancels everything in flight and drops everything waiting.
 *
 * Preconditions:
 *  - @self is a scheduler (else: critical, nothing changes).
 * Postconditions:
 *  - the visible flag is !!@visible; actions may have been queued and the
 *    deadline re-evaluated.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none (no pointers besides @self, which is only used for the call).
 */
void stk_scheduler_set_visible(StkScheduler *self, StkSchedTime now, gboolean visible);

/**
 * stk_scheduler_set_online:
 * @self: (not nullable): the scheduler.
 * @now: the current time.
 * @online: whether the network is available.
 *
 * No-op when unchanged. TRUE starts an event round (when visible); FALSE
 * cancels everything in flight and drops everything waiting.
 *
 * Preconditions:
 *  - @self is a scheduler (else: critical, nothing changes).
 * Postconditions:
 *  - the online flag is !!@online; actions may have been queued and the
 *    deadline re-evaluated.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none (no pointers besides @self, which is only used for the call).
 */
void stk_scheduler_set_online(StkScheduler *self, StkSchedTime now, gboolean online);

/**
 * stk_scheduler_focus_regained:
 * @self: (not nullable): the scheduler.
 * @now: the current time.
 *
 * The window became the active window. Starts an event round, but only while
 * visible and online (otherwise nothing: showing or reconnecting does it).
 *
 * Preconditions:
 *  - @self is a scheduler (else: critical, nothing changes).
 * Postconditions:
 *  - actions may have been queued and the deadline re-evaluated.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none (no pointers besides @self, which is only used for the call).
 */
void stk_scheduler_focus_regained(StkScheduler *self, StkSchedTime now);

/**
 * stk_scheduler_tick:
 * @self: (not nullable): the scheduler.
 * @now: the current time.
 *
 * To be called when the timer armed for stk_scheduler_next_wakeup_us() fires.
 * If the deadline has been reached and the window is visible and the network
 * available: starts a periodic round and schedules the next deadline while
 * the market is open; with the market closed it starts the one final round
 * instead and leaves no deadline (see "Timer"). Calling it early, or with no
 * deadline, only re-evaluates the conditions.
 *
 * Preconditions:
 *  - @self is a scheduler (else: critical, nothing changes).
 * Postconditions:
 *  - actions may have been queued; the deadline is re-evaluated (see
 *    stk_scheduler_next_wakeup_us()).
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none (no pointers besides @self, which is only used for the call).
 */
void stk_scheduler_tick(StkScheduler *self, StkSchedTime now);

/**
 * stk_scheduler_request_done:
 * @self: (not nullable): the scheduler.
 * @now: the current time.
 * @id: the id of a request that FETCH_* returned (also one that was
 *   cancelled; the provider reports every operation exactly once).
 * @result: how it ended.
 *
 * Frees the request's slot, updates the stale mark and the freshness of the
 * jobs the request delivered (QUOTE; a series: SPARK if it has the range
 * STK_SCHED_SPARK_RANGE, DETAIL if it has the selected range of the selected
 * symbol) and starts waiting jobs. An unknown or already finished @id is
 * ignored.
 *
 * Preconditions:
 *  - @self is a scheduler (else: critical, nothing changes); @result is an
 *    StkSchedResult (a value other than OK or FAILED is handled like
 *    CANCELLED: no mark changes).
 * Postconditions:
 *  - if @id is the id of an outstanding request: that request no longer holds
 *    a slot (the in-flight counters drop by one, the entry of a removed symbol
 *    is freed); otherwise only the conditions are re-evaluated. Actions may
 *    have been queued.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none (no pointers besides @self, which is only used for the call).
 */
void stk_scheduler_request_done(StkScheduler *self, StkSchedTime now, guint64 id,
                                StkSchedResult result);

/**
 * stk_scheduler_take_actions:
 * @self: (not nullable): the scheduler.
 *
 * Preconditions:
 *  - @self is a scheduler (else: critical, NULL returned).
 * Postconditions:
 *  - the scheduler holds no pending action afterwards.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: the caller owns the returned array.
 *
 * Returns: (transfer full) (nullable) (element-type StkSchedAction): the
 *   actions produced since the last call, in the order to execute them, or
 *   NULL if there are none. Free with g_array_unref().
 */
GArray *stk_scheduler_take_actions(StkScheduler *self);

/**
 * stk_scheduler_next_wakeup_us:
 * @self: (not nullable): the scheduler.
 *
 * Preconditions:
 *  - @self is a scheduler (else: critical, -1 returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none (no pointers returned).
 *
 * Returns: the monotonic time (microseconds, g_get_monotonic_time() scale)
 *   at which to call stk_scheduler_tick(), or -1: no timer must exist. It can
 *   change after every call that takes a StkSchedTime; re-read it after each.
 */
gint64 stk_scheduler_next_wakeup_us(StkScheduler *self);

/**
 * stk_scheduler_is_stale:
 * @self: (not nullable): the scheduler.
 * @symbol: (nullable): a tracked symbol (ASCII case-insensitive).
 *
 * Preconditions:
 *  - @self is a scheduler (else: critical, FALSE returned); @symbol is NULL,
 *    NUL-terminated or has at least 16 readable bytes.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: @symbol is borrowed for the call only.
 *
 * Returns: TRUE iff at least one job of @symbol is marked failed (see
 *   "Failure" and "Stale marks" above: the latest request of the job failed
 *   and neither a success nor, for DETAIL, a selection change has cleared it).
 *   FALSE for unknown (and NULL or invalid) symbols.
 */
gboolean stk_scheduler_is_stale(StkScheduler *self, const char *symbol);

/**
 * stk_scheduler_n_inflight:
 * @self: (not nullable): the scheduler.
 *
 * Preconditions:
 *  - @self is a scheduler (else: critical, 0 returned).
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: the thread that owns the scheduler (main thread).
 * Ownership: none (no pointers returned).
 *
 * Returns: the number of requests outstanding, cancelled ones included.
 */
guint stk_scheduler_n_inflight(StkScheduler *self);

/**
 * stk_sched_series_is_hollow:
 * @fresh: (not nullable): a series that was just fetched.
 * @held: (nullable): a series the owner holds for the same instrument (shown,
 *   cached), or NULL.
 *
 * The result policy for a refreshed intraday series. Before the open the
 * provider has no regular-session point for the new day, so a successful 1D
 * refresh can legitimately arrive with zero points; it carries no information
 * about the last session, which @held still describes. The owner must then
 * keep @held (neither show nor store @fresh) and must not mark the symbol
 * stale: the request did not fail.
 *
 * Preconditions:
 *  - @fresh is non-NULL (else: critical, FALSE returned); @fresh and a
 *    non-NULL @held are live series that nobody modifies during the call.
 * Postconditions:
 *  - no state is changed; no reference is taken.
 * Thread affinity: any thread (pure: reads only the two series).
 * Ownership: both series are borrowed for the call only.
 *
 * Returns: TRUE iff @fresh has the range STK_RANGE_1D and no points while
 *   @held has that range and at least one point (so @fresh must not replace
 *   @held). FALSE in every other case: a series with points replaces
 *   normally, other ranges are never kept back, and an empty series replaces
 *   only when nothing non-empty is held (the owner then shows "No data").
 */
gboolean stk_sched_series_is_hollow(const StkSeries *fresh, const StkSeries *held);

G_END_DECLS

#endif /* STK_SCHEDULER_H */
