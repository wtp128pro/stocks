/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-chart.c - StkChart: price chart widget drawn with GtkSnapshot.
 * See stk-chart.h for the public contract.
 *
 * Structure: set_series() copies the data and computes everything that
 * depends only on the data (domain, visible window, y range, direction). The
 * size-dependent geometry (plot rectangle, ticks, label layouts, decimated
 * line/fill/dash paths) is rebuilt lazily by rebuild() when the series, the
 * allocation or the style changed, never per frame. snapshot() only replays
 * the cached paths and layouts.
 */
#include "stk-chart.h"

#include <math.h>
#include <string.h>

#include "stk-chart-geom.h"

#define MIN_WIDTH 120
#define MIN_HEIGHT 80
#define NATURAL_WIDTH 320
#define NATURAL_HEIGHT 200

#define MAX_Y_LABELS 5
#define MAX_X_LABELS 6
#define PLOT_PAD_LEFT 4.0f
#define Y_PAD_FRAC 0.08
#define LABEL_SCALE 0.85
#define HOVER_EDGE_TOLERANCE 4.0
#define DOT_RADIUS 4.0f

#define NO_HOVER ((gssize)-1)

/* The label arrays are filled by the tick functions, which write at most the
 * (clamped) count asked for and never more than STK_CHART_GEOM_MAX_TICKS. */
G_STATIC_ASSERT(MAX_Y_LABELS <= STK_CHART_GEOM_MAX_TICKS);
G_STATIC_ASSERT(MAX_X_LABELS <= STK_CHART_GEOM_MAX_TICKS);

/* Short-lived graphene values for snapshot calls. */
#define POINT(x, y) (&(graphene_point_t){ (x), (y) })
#define RECT(x, y, w, h) (&(graphene_rect_t){ { (x), (y) }, { (w), (h) } })

struct _StkChart {
  GtkWidget parent_instance;

  /* Data cache (set_series): at most STK_CHART_GEOM_MAX_SAMPLES samples. */
  double *t, *v; /* win_n valid entries each, n_alloc allocated */
  gsize n_alloc;
  StkRange range;
  double prev_close; /* finite only when it is drawn (1D) */
  double t0, t1;     /* x domain */
  double lo, hi;     /* y range incl. padding */
  gsize win_n;       /* samples kept (all inside the domain) */
  gboolean has_data;
  gboolean stale;

  /* Size/style dependent cache (rebuild). */
  int width, height;
  gboolean layout_dirty;
  gboolean has_plot;
  float pl, pr, pt, pb; /* plot rectangle edges */
  int text_h;
  gsize n_yt, n_xt;
  float yt_y[MAX_Y_LABELS];
  float xl_x[MAX_X_LABELS];
  GskPath *line, *fill, *prev;
  gboolean has_dot;
  float dot_x, dot_y;
  gsize *idx_buf;
  gsize idx_cap;

  /* Immutable drawing resources, built once. */
  GskStroke *line_stroke, *dash_stroke;
  PangoAttrList *label_attrs;
  PangoLayout *ylab[MAX_Y_LABELS];
  PangoLayout *xlab[MAX_X_LABELS];
  PangoLayout *tip, *stale_lbl, *nodata, *sample;

  /* Hover / scrub state. */
  gssize hover; /* index into the visible window, or NO_HOVER */
  float hover_x, hover_y, tip_x;
  gboolean scrubbing;
  gboolean disposed; /* dispose() ran: layouts and strokes are gone, every method is a no-op */
};

/* Type boilerplate: G_DEFINE_FINAL_TYPE() defines stk_chart_get_type(),
 * stk_chart_parent_class and the instance-private plumbing; all are used on
 * the main thread only. */
G_DEFINE_FINAL_TYPE(StkChart, stk_chart, GTK_TYPE_WIDGET)

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

/**
 * with_alpha:
 * @c: colour to copy.
 * @a: factor applied to the alpha channel.
 *
 * Scales the alpha of a copy of @c.
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - no state is changed; @c (a by-value copy) is the only thing modified.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: @c with alpha multiplied by @a.
 */
static GdkRGBA
with_alpha(GdkRGBA c, float a)
{
  c.alpha *= a;
  return c;
}

/**
 * free_paths:
 * @self: (not nullable): the chart.
 *
 * Releases the cached line, fill and previous-close paths and the dot flag.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_CHART().
 * Postconditions:
 *  - line, fill and prev are NULL and has_dot is FALSE; repeated calls are
 *    safe (g_clear_pointer() ignores NULL).
 * Thread affinity: main thread only.
 * Ownership: the paths were owned by @self and are released here.
 *
 * Returns: void.
 */
static void
free_paths(StkChart *self)
{
  g_clear_pointer(&self->line, gsk_path_unref);
  g_clear_pointer(&self->fill, gsk_path_unref);
  g_clear_pointer(&self->prev, gsk_path_unref);
  self->has_dot = FALSE;
}

/**
 * clear_hover:
 * @self: (not nullable): the chart.
 *
 * Forgets the hovered sample and redraws if there was one.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_CHART().
 * Postconditions:
 *  - hover == NO_HOVER; a redraw is queued only if it changed.
 * Thread affinity: main thread only.
 * Ownership: @self is borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
clear_hover(StkChart *self)
{
  if (self->hover != NO_HOVER) {
    self->hover = NO_HOVER;
    gtk_widget_queue_draw(GTK_WIDGET(self));
  }
}

/**
 * make_label:
 * @self: (not nullable): the chart whose Pango context is used.
 *
 * Creates a label layout with the shared small-text attributes.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_CHART(); label_attrs was created (init order).
 * Postconditions:
 *  - the layout holds its own reference to the attribute list.
 * Thread affinity: main thread only.
 * Ownership: @self is borrowed.
 *
 * Returns: (transfer full): a new layout; the caller owns it.
 */
static PangoLayout *
make_label(StkChart *self)
{
  PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), NULL);
  pango_layout_set_attributes(layout, self->label_attrs);
  return layout;
}

/**
 * set_text:
 * @layout: (not nullable): the layout to update.
 * @text: (not nullable): NUL-terminated UTF-8 text.
 *
 * Sets the text of @layout.
 *
 * Preconditions:
 *  - @text is NUL-terminated: every caller passes a string literal or a
 *    buffer filled by g_snprintf() / the stk_chart_geom_format_*() functions,
 *    which terminate within their sizeof-bounded buffer (the length -1 makes
 *    Pango measure it).
 * Postconditions:
 *  - the layout holds a copy of @text.
 * Thread affinity: main thread only.
 * Ownership: @layout and @text are borrowed; Pango copies the text.
 *
 * Returns: void.
 */
static void
set_text(PangoLayout *layout, const char *text)
{
  pango_layout_set_text(layout, text, -1);
}

/* ------------------------------------------------------------------------ */
/* Layout rebuild (size / data / style change only)                         */
/* ------------------------------------------------------------------------ */

/**
 * build_paths:
 * @self: (not nullable): the chart.
 *
 * Decimates the visible samples onto pixel columns and builds the line, fill
 * and previous-close paths (or the single-sample dot).
 *
 * Preconditions:
 *  - called only from rebuild() with has_data set, win_n >= 1, no paths
 *    present, and pr - pl >= 16 (finite, bounded by the int widget width), so
 *    the (gsize)ceil() of it is positive and far below 2^63.
 * Postconditions:
 *  - the index buffer holds at least min(2 * columns, win_n) entries, with
 *    columns <= STK_CHART_GEOM_MAX_COLUMNS and win_n <=
 *    STK_CHART_GEOM_MAX_SAMPLES, and decimate() never writes beyond that
 *    capacity; every index it returns is below win_n.
 *  - for two or more kept samples line and fill are set; for one, has_dot and
 *    dot_x/dot_y; if none is kept, nothing is set. prev is set when
 *    prev_close is finite. The float conversions take values mapped into the
 *    plot rectangle from finite samples, so they are in float range.
 * Thread affinity: main thread only.
 * Ownership: @self is borrowed; the created paths are owned by @self.
 *
 * Returns: void.
 */
static void
build_paths(StkChart *self)
{
  const double *wt = self->t;
  const double *wv = self->v;
  /* Columns are bounded by STK_CHART_GEOM_MAX_COLUMNS however wide the widget
   * is, and the output cannot exceed the sample count, so the index buffer
   * never holds more than STK_CHART_GEOM_MAX_SAMPLES entries. */
  gsize columns = (gsize)ceil((double)(self->pr - self->pl));
  columns = CLAMP(columns, 1, (gsize)STK_CHART_GEOM_MAX_COLUMNS);

  gsize cap = MIN(2 * columns, self->win_n);
  if (cap > self->idx_cap) {
    self->idx_buf = g_renew(gsize, self->idx_buf, cap);
    self->idx_cap = cap;
  }
  gsize kept = stk_chart_geom_decimate(wt, wv, self->win_n, self->t0, self->t1, columns,
                                       self->idx_buf, cap);
  if (kept == 0)
    return;

  float bottom = self->pb;
  float x = 0.0f, y = 0.0f, first_x = 0.0f;
  GskPathBuilder *line = gsk_path_builder_new();
  GskPathBuilder *fill = gsk_path_builder_new();
  for (gsize k = 0; k < kept; k++) {
    gsize i = self->idx_buf[k];
    x = (float)stk_chart_geom_map(wt[i], self->t0, self->t1, self->pl, self->pr);
    y = (float)stk_chart_geom_map(wv[i], self->lo, self->hi, self->pb, self->pt);
    if (k == 0) {
      first_x = x;
      gsk_path_builder_move_to(line, x, y);
      gsk_path_builder_move_to(fill, x, bottom);
      gsk_path_builder_line_to(fill, x, y);
    } else {
      gsk_path_builder_line_to(line, x, y);
      gsk_path_builder_line_to(fill, x, y);
    }
  }
  gsk_path_builder_line_to(fill, x, bottom);
  gsk_path_builder_line_to(fill, first_x, bottom);
  gsk_path_builder_close(fill);

  if (kept == 1) {
    self->has_dot = TRUE;
    self->dot_x = x;
    self->dot_y = y;
    gsk_path_builder_unref(line);
    gsk_path_builder_unref(fill);
  } else {
    self->line = gsk_path_builder_free_to_path(line);
    self->fill = gsk_path_builder_free_to_path(fill);
  }

  if (isfinite(self->prev_close)) {
    float py = (float)stk_chart_geom_map(self->prev_close, self->lo, self->hi, self->pb, self->pt);
    GskPathBuilder *dash = gsk_path_builder_new();
    gsk_path_builder_move_to(dash, self->pl, py);
    gsk_path_builder_line_to(dash, self->pr, py);
    self->prev = gsk_path_builder_free_to_path(dash);
  }
}

/**
 * rebuild:
 * @self: (not nullable): the chart.
 *
 * Recomputes the size-dependent geometry (plot rectangle, ticks, label texts
 * and positions, decimated paths) when the series, size or style changed.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_CHART(). The label counts are bounded because
 *    nice_ticks() / time_ticks() write at most their (clamped) max_ticks,
 *    which is <= MAX_Y_LABELS / MAX_X_LABELS (see the static asserts).
 * Postconditions:
 *  - nothing happens when disposed, not dirty, or the size is not positive.
 *    Otherwise layout_dirty and the hover are cleared and the old paths are
 *    freed. has_plot is set only when data exist and the plot is large
 *    enough (>= 8 px high, >= 16 px wide); then pl/pr/pt/pb, n_yt, n_xt,
 *    yt_y, xl_x and the paths are valid. n_yt and n_xt are 0 otherwise.
 *  - the integer conversions here are guarded: the y-label count against a
 *    zero text height, and the (gint64) casts of the domain against values of
 *    magnitude >= 2^63. The (gsize) cast in max_x converts a quotient of the
 *    plot width (>= 16, finite) by (label width + 18) >= 18, which is finite
 *    and positive.
 * Thread affinity: main thread only.
 * Ownership: @self is borrowed; the paths created are owned by @self and
 *   released by free_paths().
 *
 * Returns: void.
 */
static void
rebuild(StkChart *self)
{
  if (self->disposed || !self->layout_dirty || self->width <= 0 || self->height <= 0)
    return;
  self->layout_dirty = FALSE;
  self->hover = NO_HOVER; /* its cached coordinates belong to the old layout */
  self->has_plot = FALSE;
  self->n_yt = self->n_xt = 0;
  free_paths(self);

  int w = self->width, h = self->height;
  int sample_w;
  set_text(self->sample, "0");
  pango_layout_get_pixel_size(self->sample, &sample_w, &self->text_h);

  if (!self->has_data)
    return;

  self->pt = (float)(self->text_h + 8);
  self->pb = (float)(h - self->text_h - 8);
  float plot_h = self->pb - self->pt;
  if (plot_h < 8.0f)
    return;

  /* Y ticks and labels; their width fixes the right margin. */
  /* text_h is 0 for a font without height: the quotient is then +inf, and a
   * float-to-integer cast of inf (or of anything >= 2^64) is undefined, so
   * compare before casting. Results equal the old CLAMP(trunc(q), 1, MAX_Y_LABELS)
   * for every finite quotient. */
  float rows = plot_h / (3.0f * (float)self->text_h);
  gsize max_y =
      rows >= (float)MAX_Y_LABELS ? (gsize)MAX_Y_LABELS : (rows >= 1.0f ? (gsize)rows : 1);
  max_y = CLAMP(max_y, 1, MAX_Y_LABELS);
  double yt[STK_CHART_GEOM_MAX_TICKS];
  self->n_yt = stk_chart_geom_nice_ticks(self->lo, self->hi, max_y, 0.01, yt);
  int max_label_w = 0;
  for (gsize i = 0; i < self->n_yt; i++) {
    char buf[STK_CHART_GEOM_LABEL_LEN];
    int lw, lh;
    stk_chart_geom_format_price(yt[i], buf, sizeof buf);
    set_text(self->ylab[i], buf);
    pango_layout_get_pixel_size(self->ylab[i], &lw, &lh);
    max_label_w = MAX(max_label_w, lw);
    self->yt_y[i] =
        (float)stk_chart_geom_map(yt[i], self->lo, self->hi, self->pb, self->pt);
  }
  self->pl = PLOT_PAD_LEFT;
  self->pr = (float)w - (self->n_yt > 0 ? (float)max_label_w + 12.0f : 8.0f);
  if (self->pr - self->pl < 16.0f) {
    self->n_yt = 0;
    return;
  }
  self->has_plot = TRUE;

  /* X ticks and labels. */
  int xw, xh;
  set_text(self->sample, "00:00 AM");
  pango_layout_get_pixel_size(self->sample, &xw, &xh);
  gsize max_x = (gsize)((self->pr - self->pl) / (float)(xw + 18));
  max_x = CLAMP(max_x, 1, MAX_X_LABELS);
  gint64 xt[STK_CHART_GEOM_MAX_TICKS];
  /* (gint64) of a value of magnitude >= 2^63 (or NaN) is undefined; (double)
   * G_MAXINT64 is itself 2^63. A domain that does not fit gint64 gets no time
   * labels, as does any domain outside the calendar. */
  if (fabs(self->t0) < 9223372036854775808.0 && fabs(self->t1) < 9223372036854775808.0)
    self->n_xt = stk_chart_geom_time_ticks(self->range, (gint64)self->t0, (gint64)self->t1, max_x,
                                           xt);
  for (gsize i = 0; i < self->n_xt; i++) {
    char buf[STK_CHART_GEOM_LABEL_LEN];
    int lw, lh;
    stk_chart_geom_format_tick(self->range, xt[i], buf, sizeof buf);
    set_text(self->xlab[i], buf);
    pango_layout_get_pixel_size(self->xlab[i], &lw, &lh);
    float cx = (float)stk_chart_geom_map((double)xt[i], self->t0, self->t1, self->pl, self->pr);
    self->xl_x[i] = CLAMP(cx - (float)lw / 2.0f, 0.0f, (float)(w - lw));
  }

  build_paths(self);
}

/* ------------------------------------------------------------------------ */
/* Hover                                                                    */
/* ------------------------------------------------------------------------ */

/**
 * set_hover_from_x:
 * @self: (not nullable): the chart.
 * @x: pointer x in widget coordinates (any double; NaN clears the hover).
 *
 * Selects the visible sample nearest to @x, caches its crosshair position and
 * tooltip text and queues a redraw. No crosshair is shown beyond the first /
 * last sample (plus a small tolerance) or without a plot.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_CHART().
 * Postconditions:
 *  - after dispose() nothing happens. Otherwise rebuild() ran; without data
 *    or plot, or with @x outside the samples, the hover is cleared. Else
 *    self->hover is an index below win_n (win_n >= 1 whenever has_data is
 *    set, so wt[0] and wt[win_n - 1] are valid), the cached hover_x/hover_y/
 *    tip_x and the tooltip layout are updated and a redraw is queued (none if
 *    the same sample is already hovered).
 *  - the tooltip text is built in fixed buffers with g_snprintf(), which
 *    truncates instead of overrunning.
 * Thread affinity: main thread only.
 * Ownership: @self is borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
set_hover_from_x(StkChart *self, double x)
{
  if (self->disposed)
    return;
  rebuild(self);
  if (!self->has_data || !self->has_plot) {
    clear_hover(self);
    return;
  }

  const double *wt = self->t;
  const double *wv = self->v;
  double xa = stk_chart_geom_map(wt[0], self->t0, self->t1, self->pl, self->pr);
  double xb = stk_chart_geom_map(wt[self->win_n - 1], self->t0, self->t1, self->pl, self->pr);
  if (x < xa - HOVER_EDGE_TOLERANCE || x > xb + HOVER_EDGE_TOLERANCE) {
    clear_hover(self);
    return;
  }

  double target = stk_chart_geom_map(x, self->pl, self->pr, self->t0, self->t1);
  gsize i;
  if (!stk_chart_geom_nearest(wt, self->win_n, target, &i)) {
    clear_hover(self);
    return;
  }
  if (self->hover == (gssize)i)
    return;
  self->hover = (gssize)i;

  self->hover_x = (float)stk_chart_geom_map(wt[i], self->t0, self->t1, self->pl, self->pr);
  self->hover_y = (float)stk_chart_geom_map(wv[i], self->lo, self->hi, self->pb, self->pt);

  char price[STK_CHART_GEOM_LABEL_LEN], when[STK_CHART_GEOM_LABEL_LEN];
  char text[2 * STK_CHART_GEOM_LABEL_LEN + 8];
  stk_chart_geom_format_price(wv[i], price, sizeof price);
  /* (gint64) of a value of magnitude >= 2^63 is undefined; such an instant is
   * beyond year 9999, for which the hover text is an em dash either way. */
  gint64 when_ms = fabs(wt[i]) < 9223372036854775808.0 ? (gint64)wt[i] : G_MAXINT64;
  stk_chart_geom_format_hover(self->range, when_ms, when, sizeof when);
  g_snprintf(text, sizeof text, "%s  \xc2\xb7  %s", price, when);
  set_text(self->tip, text);
  int th, tip_w;
  pango_layout_get_pixel_size(self->tip, &tip_w, &th);

  float right_limit = self->pr;
  if (self->stale) {
    int sw, sh;
    pango_layout_get_pixel_size(self->stale_lbl, &sw, &sh);
    right_limit = self->pr - (float)sw - 8.0f;
  }
  float tx = self->hover_x - (float)tip_w / 2.0f;
  tx = MIN(tx, right_limit - (float)tip_w);
  self->tip_x = MAX(tx, self->pl);

  gtk_widget_queue_draw(GTK_WIDGET(self));
}

/**
 * on_motion:
 * @controller: the motion controller (unused).
 * @x: pointer x in widget coordinates.
 * @y: pointer y (unused).
 * @user_data: (not nullable): the StkChart that owns the controller.
 *
 * "motion" handler: selects the sample nearest to the pointer.
 *
 * Preconditions:
 *  - @user_data is the StkChart passed at connection (see on_drag_end()).
 * Postconditions:
 *  - see set_hover_from_x().
 * Thread affinity: main thread only.
 * Ownership: all arguments are borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
on_motion(GtkEventControllerMotion *controller, double x, double y, gpointer user_data)
{
  (void)controller;
  (void)y;
  set_hover_from_x(STK_CHART(user_data), x);
}

/**
 * on_leave:
 * @controller: the motion controller (unused).
 * @user_data: (not nullable): the StkChart that owns the controller.
 *
 * "leave" handler: clears the crosshair unless a finger is scrubbing.
 *
 * Preconditions:
 *  - @user_data is the StkChart passed at connection (see on_drag_end()).
 * Postconditions:
 *  - the hover is cleared when not scrubbing.
 * Thread affinity: main thread only.
 * Ownership: all arguments are borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
on_leave(GtkEventControllerMotion *controller, gpointer user_data)
{
  (void)controller;
  StkChart *self = STK_CHART(user_data);
  if (!self->scrubbing)
    clear_hover(self);
}

/**
 * on_long_press:
 * @gesture: (not nullable): the long-press gesture.
 * @x: x position of the press.
 * @y: y position of the press (unused).
 * @user_data: (not nullable): the StkChart that owns the gesture.
 *
 * "pressed" handler: starts scrubbing, claims the touch sequence and shows the
 * crosshair at the press position.
 *
 * Preconditions:
 *  - @user_data is the StkChart passed at connection (see on_drag_end()).
 * Postconditions:
 *  - scrubbing is TRUE and the gesture sequence is claimed; see
 *    set_hover_from_x() for the hover (a no-op after dispose()).
 * Thread affinity: main thread only.
 * Ownership: all arguments are borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
on_long_press(GtkGestureLongPress *gesture, double x, double y, gpointer user_data)
{
  (void)y;
  StkChart *self = STK_CHART(user_data);
  self->scrubbing = TRUE;
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  set_hover_from_x(self, x);
}

/**
 * on_drag_update:
 * @gesture: (not nullable): the drag gesture.
 * @offset_x: x offset from the start point.
 * @offset_y: y offset from the start point (unused).
 * @user_data: (not nullable): the StkChart that owns the gesture.
 *
 * "drag-update" handler: while scrubbing, moves the crosshair to the finger.
 *
 * Preconditions:
 *  - @user_data is the StkChart passed at connection (see on_drag_end()).
 * Postconditions:
 *  - without scrubbing nothing happens; otherwise see set_hover_from_x().
 * Thread affinity: main thread only.
 * Ownership: all arguments are borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data)
{
  (void)offset_y;
  StkChart *self = STK_CHART(user_data);
  if (!self->scrubbing)
    return;
  double sx, sy;
  gtk_gesture_drag_get_start_point(gesture, &sx, &sy);
  set_hover_from_x(self, sx + offset_x);
}

/**
 * on_drag_end:
 * @gesture: the drag gesture (unused).
 * @offset_x: x offset from the start point (unused).
 * @offset_y: y offset from the start point (unused).
 * @user_data: (not nullable): the StkChart that owns the gesture.
 *
 * "drag-end" handler: scrubbing stops and the crosshair is cleared.
 *
 * Preconditions:
 *  - @user_data is the StkChart passed at connection; the widget owns the
 *    gesture, so the pointer is valid while the gesture can emit.
 * Postconditions:
 *  - scrubbing is FALSE and the hover is cleared.
 * Thread affinity: main thread only.
 * Ownership: all arguments are borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data)
{
  (void)gesture;
  (void)offset_x;
  (void)offset_y;
  StkChart *self = STK_CHART(user_data);
  self->scrubbing = FALSE;
  clear_hover(self);
}

/* ------------------------------------------------------------------------ */
/* GtkWidget vfuncs                                                         */
/* ------------------------------------------------------------------------ */

/**
 * stk_chart_measure:
 * @widget: unused.
 * @orientation: which dimension is measured.
 * @for_size: unused.
 * @minimum: (out): receives the minimum size.
 * @natural: (out): receives the natural size.
 * @minimum_baseline: (out): receives -1 (no baseline).
 * @natural_baseline: (out): receives -1 (no baseline).
 *
 * Reports the minimum (MIN_WIDTH x MIN_HEIGHT) and natural (NATURAL_WIDTH x
 * NATURAL_HEIGHT) sizes.
 *
 * Preconditions:
 *  - all four output pointers are non-NULL (GTK always passes them).
 * Postconditions:
 *  - the outputs are written; no widget state is read or changed.
 * Thread affinity: main thread only.
 * Ownership: @widget is borrowed and unused; the outputs are caller-owned.
 *
 * Returns: void.
 */
static void
stk_chart_measure(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum,
                  int *natural, int *minimum_baseline, int *natural_baseline)
{
  (void)widget;
  (void)for_size;
  *minimum = orientation == GTK_ORIENTATION_HORIZONTAL ? MIN_WIDTH : MIN_HEIGHT;
  *natural = orientation == GTK_ORIENTATION_HORIZONTAL ? NATURAL_WIDTH : NATURAL_HEIGHT;
  *minimum_baseline = -1;
  *natural_baseline = -1;
}

/**
 * stk_chart_size_allocate:
 * @widget: (not nullable): the StkChart.
 * @width: new width in pixels.
 * @height: new height in pixels.
 * @baseline: unused.
 *
 * Records a changed size and marks the geometry dirty.
 *
 * Preconditions:
 *  - called by GTK.
 * Postconditions:
 *  - when the size changed, width/height are stored, layout_dirty is set and
 *    the hover is cleared; otherwise nothing changes.
 * Thread affinity: main thread only.
 * Ownership: @widget is borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
stk_chart_size_allocate(GtkWidget *widget, int width, int height, int baseline)
{
  (void)baseline;
  StkChart *self = STK_CHART(widget);
  if (width != self->width || height != self->height) {
    self->width = width;
    self->height = height;
    self->layout_dirty = TRUE;
    self->hover = NO_HOVER;
  }
}

/**
 * style_changed:
 * @self: (not nullable): the chart.
 *
 * Fonts, scale or text direction changed: every cached layout must re-read
 * the (updated) widget Pango context, and all geometry and the hover that
 * depended on the old label sizes are stale.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_CHART().
 * Postconditions:
 *  - after dispose() nothing happens (the layouts are gone). Otherwise every
 *    layout is told its context changed, layout_dirty is set and the hover is
 *    cleared (no redraw is queued here).
 * Thread affinity: main thread only.
 * Ownership: @self is borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
style_changed(StkChart *self)
{
  if (self->disposed)
    return;
  pango_layout_context_changed(self->sample);
  pango_layout_context_changed(self->tip);
  pango_layout_context_changed(self->stale_lbl);
  pango_layout_context_changed(self->nodata);
  for (gsize i = 0; i < MAX_Y_LABELS; i++)
    pango_layout_context_changed(self->ylab[i]);
  for (gsize i = 0; i < MAX_X_LABELS; i++)
    pango_layout_context_changed(self->xlab[i]);
  self->layout_dirty = TRUE;
  self->hover = NO_HOVER;
}

/**
 * stk_chart_css_changed:
 * @widget: (not nullable): the StkChart.
 * @change: the CSS style change.
 *
 * Chains up, then invalidates the cached layouts (see style_changed()).
 *
 * Preconditions:
 *  - called by GTK on a live (possibly disposed) widget.
 * Postconditions:
 *  - see style_changed().
 * Thread affinity: main thread only.
 * Ownership: @widget and @change are borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
stk_chart_css_changed(GtkWidget *widget, GtkCssStyleChange *change)
{
  GTK_WIDGET_CLASS(stk_chart_parent_class)->css_changed(widget, change);
  style_changed(STK_CHART(widget));
}

/**
 * stk_chart_system_setting_changed:
 * @widget: (not nullable): the StkChart.
 * @setting: the changed system setting.
 *
 * Chains up, invalidates the cached layouts and queues a redraw.
 *
 * Preconditions:
 *  - called by GTK on a live (possibly disposed) widget.
 * Postconditions:
 *  - see style_changed(); a redraw is queued.
 * Thread affinity: main thread only.
 * Ownership: @widget is borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
stk_chart_system_setting_changed(GtkWidget *widget, GtkSystemSetting setting)
{
  GTK_WIDGET_CLASS(stk_chart_parent_class)->system_setting_changed(widget, setting);
  style_changed(STK_CHART(widget));
  gtk_widget_queue_draw(widget);
}

/**
 * append_layout_at:
 * @snapshot: (not nullable): snapshot to append to.
 * @layout: (not nullable): the layout to draw.
 * @x: x offset of the layout's top-left corner.
 * @y: y offset of the layout's top-left corner.
 * @color: (not nullable): text colour.
 *
 * Appends @layout at (@x, @y) inside a saved / restored snapshot state.
 *
 * Preconditions:
 *  - @snapshot, @layout and @color are valid (layouts exist until dispose();
 *    the only caller is snapshot(), which returns early once disposed).
 * Postconditions:
 *  - the snapshot state is restored.
 * Thread affinity: main thread only.
 * Ownership: all arguments are borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
append_layout_at(GtkSnapshot *snapshot, PangoLayout *layout, float x, float y, const GdkRGBA *color)
{
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, POINT(x, y));
  gtk_snapshot_append_layout(snapshot, layout, color);
  gtk_snapshot_restore(snapshot);
}

/**
 * stk_chart_snapshot:
 * @widget: (not nullable): the StkChart.
 * @snapshot: (not nullable): snapshot to append to.
 *
 * Replays the cached geometry: stale marker, "No data" text, grid, labels,
 * previous-close dash, gradient fill, line or dot and the hover crosshair.
 *
 * Preconditions:
 *  - called by GTK during drawing with a valid @snapshot.
 * Postconditions:
 *  - nothing is drawn after dispose() or at a non-positive size. Otherwise
 *    rebuild() ran first, so n_yt / n_xt are within the label array sizes
 *    (<= MAX_Y_LABELS / MAX_X_LABELS); self->line is non-NULL whenever
 *    self->fill is (build_paths() creates them together). The cached
 *    geometry is not modified.
 * Thread affinity: main thread only.
 * Ownership: @widget and @snapshot are borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
stk_chart_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
  StkChart *self = STK_CHART(widget);
  if (self->disposed)
    return;
  rebuild(self);
  if (self->width <= 0 || self->height <= 0)
    return;

  GdkRGBA accent, neutral;
  GtkWidget *parent = gtk_widget_get_parent(widget);
  gtk_widget_get_color(widget, &accent);
  gtk_widget_get_color(parent != NULL ? parent : widget, &neutral);
  GdkRGBA dim = with_alpha(neutral, 0.7f);

  if (self->stale) {
    int sw, sh;
    pango_layout_get_pixel_size(self->stale_lbl, &sw, &sh);
    float sx = (self->has_plot ? self->pr : (float)self->width - 4.0f) - (float)sw;
    append_layout_at(snapshot, self->stale_lbl, sx, 4.0f, &dim);
  }

  if (!self->has_data) {
    int tw, th;
    pango_layout_get_pixel_size(self->nodata, &tw, &th);
    append_layout_at(snapshot, self->nodata, ((float)self->width - (float)tw) / 2.0f,
                     ((float)self->height - (float)th) / 2.0f, &dim);
    return;
  }
  if (!self->has_plot)
    return;

  float plot_w = self->pr - self->pl;
  float plot_h = self->pb - self->pt;

  /* Grid and labels. */
  GdkRGBA grid = with_alpha(neutral, 0.15f);
  for (gsize i = 0; i < self->n_yt; i++) {
    float y = floorf(self->yt_y[i]);
    gtk_snapshot_append_color(snapshot, &grid, RECT(self->pl, y, plot_w, 1.0f));
    append_layout_at(snapshot, self->ylab[i], self->pr + 6.0f,
                     self->yt_y[i] - (float)self->text_h / 2.0f, &dim);
  }
  for (gsize i = 0; i < self->n_xt; i++)
    append_layout_at(snapshot, self->xlab[i], self->xl_x[i], self->pb + 4.0f, &dim);

  /* Previous-close dashed line. */
  if (self->prev != NULL) {
    GdkRGBA prev_color = with_alpha(neutral, 0.5f);
    gtk_snapshot_append_stroke(snapshot, self->prev, self->dash_stroke, &prev_color);
  }

  /* Gradient fill and line. */
  if (self->fill != NULL) {
    GskColorStop stops[2] = {
      { 0.0f, with_alpha(accent, 0.35f) },
      { 1.0f, with_alpha(accent, 0.0f) },
    };
    gtk_snapshot_push_fill(snapshot, self->fill, GSK_FILL_RULE_WINDING);
    gtk_snapshot_append_linear_gradient(snapshot,
                                        RECT(self->pl, self->pt, plot_w, plot_h),
                                        POINT(0.0f, self->pt),
                                        POINT(0.0f, self->pb), stops, 2);
    gtk_snapshot_pop(snapshot);
    gtk_snapshot_append_stroke(snapshot, self->line, self->line_stroke, &accent);
  }
  if (self->has_dot) {
    GskRoundedRect rr;
    gsk_rounded_rect_init_from_rect(
        &rr,
        RECT(self->dot_x - 2.0f, self->dot_y - 2.0f, 4.0f, 4.0f), 2.0f);
    gtk_snapshot_push_rounded_clip(snapshot, &rr);
    gtk_snapshot_append_color(snapshot, &accent, &rr.bounds);
    gtk_snapshot_pop(snapshot);
  }

  /* Crosshair and tooltip label. */
  if (self->hover != NO_HOVER) {
    GdkRGBA cross = with_alpha(neutral, 0.5f);
    gtk_snapshot_append_color(snapshot, &cross,
                              RECT(floorf(self->hover_x), self->pt, 1.0f, plot_h));
    GskRoundedRect rr;
    gsk_rounded_rect_init_from_rect(
        &rr,
        RECT(self->hover_x - DOT_RADIUS, self->hover_y - DOT_RADIUS,
                            2.0f * DOT_RADIUS, 2.0f * DOT_RADIUS),
        DOT_RADIUS);
    gtk_snapshot_push_rounded_clip(snapshot, &rr);
    gtk_snapshot_append_color(snapshot, &accent, &rr.bounds);
    gtk_snapshot_pop(snapshot);
    append_layout_at(snapshot, self->tip, self->tip_x, 4.0f, &neutral);
  }
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                  */
/* ------------------------------------------------------------------------ */

/**
 * stk_chart_dispose:
 * @object: (not nullable): the StkChart being disposed.
 *
 * Drops every GObject / GSK reference (layouts, strokes, paths) so that a
 * reference cycle through the widget cannot keep them alive.
 *
 * Preconditions:
 *  - @object is a StkChart; may run more than once (g_object_run_dispose()
 *    then the final unref).
 * Postconditions:
 *  - idempotent: "disposed" is set, hover/scrubbing/data are cleared and
 *    every reference is released with g_clear_pointer() / g_clear_object()
 *    (NULL-safe), so a second run frees nothing twice. Afterwards every
 *    method that dereferences those members returns early on "disposed"
 *    (rebuild(), snapshot(), set_hover_from_x(), style_changed(),
 *    stk_chart_set_series()); the parent dispose is chained. The sample
 *    copies stay allocated until finalize().
 * Thread affinity: main thread only.
 * Ownership: the released objects were owned by @object.
 *
 * Returns: void.
 */
static void
stk_chart_dispose(GObject *object)
{
  StkChart *self = STK_CHART(object);
  self->disposed = TRUE;
  self->hover = NO_HOVER;
  self->scrubbing = FALSE;
  self->has_data = FALSE;
  free_paths(self);
  g_clear_pointer(&self->line_stroke, gsk_stroke_free);
  g_clear_pointer(&self->dash_stroke, gsk_stroke_free);
  g_clear_pointer(&self->label_attrs, pango_attr_list_unref);
  for (gsize i = 0; i < MAX_Y_LABELS; i++)
    g_clear_object(&self->ylab[i]);
  for (gsize i = 0; i < MAX_X_LABELS; i++)
    g_clear_object(&self->xlab[i]);
  g_clear_object(&self->tip);
  g_clear_object(&self->stale_lbl);
  g_clear_object(&self->nodata);
  g_clear_object(&self->sample);
  G_OBJECT_CLASS(stk_chart_parent_class)->dispose(object);
}

/**
 * stk_chart_finalize:
 * @object: (not nullable): the StkChart being finalized.
 *
 * Frees the sample copies and the decimation index buffer.
 *
 * Preconditions:
 *  - @object is a StkChart; runs exactly once, after dispose(), when the last
 *    reference is gone (so no controller or snapshot can still use the
 *    buffers).
 * Postconditions:
 *  - t, v and idx_buf are freed (g_free() accepts NULL); the parent finalize
 *    is chained. The widget must not be used afterwards.
 * Thread affinity: main thread only.
 * Ownership: the buffers are owned by @object and released here.
 *
 * Returns: void.
 */
static void
stk_chart_finalize(GObject *object)
{
  StkChart *self = STK_CHART(object);
  g_free(self->t);
  g_free(self->v);
  g_free(self->idx_buf);
  G_OBJECT_CLASS(stk_chart_parent_class)->finalize(object);
}

/**
 * ignore_css_error:
 * @provider: the provider that reported the error (unused).
 * @section: the CSS section of the error (unused).
 * @error: the parse error (unused).
 * @user_data: unused.
 *
 * "parsing-error" handler that swallows errors of the built-in fallback CSS.
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: main thread only.
 * Ownership: none; no argument is retained.
 *
 * Returns: void.
 */
static void
ignore_css_error(GtkCssProvider *provider, GtkCssSection *section, GError *error,
                 gpointer user_data)
{
  (void)provider;
  (void)section;
  (void)error;
  (void)user_data;
}

/**
 * stk_chart_class_init:
 * @klass: (not nullable): the class structure being initialised.
 *
 * Class initialiser: installs dispose / finalize and the widget vfuncs, the
 * CSS node name and, if a default display exists, the fallback colour style.
 *
 * Preconditions:
 *  - called once by GObject when the type is first used (main thread).
 * Postconditions:
 *  - the style provider is added to the default display (if there is none,
 *    nothing is installed); the display holds the only remaining reference
 *    (the local reference is dropped).
 * Thread affinity: main thread only.
 * Ownership: @klass is owned by GObject; the provider is owned by the display
 *   after gtk_style_context_add_provider_for_display().
 *
 * Returns: void.
 */
static void
stk_chart_class_init(StkChartClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = stk_chart_dispose;
  object_class->finalize = stk_chart_finalize;
  widget_class->system_setting_changed = stk_chart_system_setting_changed;
  widget_class->measure = stk_chart_measure;
  widget_class->size_allocate = stk_chart_size_allocate;
  widget_class->css_changed = stk_chart_css_changed;
  widget_class->snapshot = stk_chart_snapshot;
  gtk_widget_class_set_css_name(widget_class, "stk-chart");

  /* Fallback colours. @define-color at FALLBACK priority only defines the
   * names when no theme/application provider does, so a theme's own
   * @success_color / @error_color (dark and light variants) win and a theme
   * without them still gets these fixed green/red values. */
  GdkDisplay *display = gdk_display_get_default();
  if (display != NULL) {
    GtkCssProvider *provider = gtk_css_provider_new();
    g_signal_connect(provider, "parsing-error", G_CALLBACK(ignore_css_error), NULL);
    gtk_css_provider_load_from_string(provider,
                                      "@define-color success_color #2ec27e;\n"
                                      "@define-color error_color #e01b24;\n"
                                      "stk-chart.positive { color: @success_color; }\n"
                                      "stk-chart.negative { color: @error_color; }\n");
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_FALLBACK);
    g_object_unref(provider);
  }
}

/**
 * stk_chart_init:
 * @self: (not nullable): the instance being initialised.
 *
 * Instance initialiser: builds the immutable strokes and label layouts and
 * attaches the hover (pointer) and long-press / drag (touch) controllers.
 *
 * Preconditions:
 *  - called by GObject on a freshly allocated, zero-filled instance.
 * Postconditions:
 *  - all layouts, strokes and label_attrs are non-NULL until dispose(); the
 *    chart has no data. The controllers are owned by the widget
 *    (gtk_widget_add_controller() takes the reference) and their handlers
 *    receive @self as raw user_data; that is safe because a controller can
 *    only emit while its widget exists, and handlers that run after dispose()
 *    see the "disposed" flag or touch only plain fields.
 * Thread affinity: main thread only.
 * Ownership: the strokes, attribute list and layouts are owned by @self and
 *   released in dispose(); the controllers by the widget.
 *
 * Returns: void.
 */
static void
stk_chart_init(StkChart *self)
{
  GtkWidget *widget = GTK_WIDGET(self);
  gtk_widget_set_hexpand(widget, TRUE);
  gtk_widget_set_vexpand(widget, TRUE);
  gtk_widget_set_overflow(widget, GTK_OVERFLOW_HIDDEN);

  self->hover = NO_HOVER;
  self->prev_close = NAN;
  self->layout_dirty = TRUE;

  self->line_stroke = gsk_stroke_new(1.5f);
  gsk_stroke_set_line_join(self->line_stroke, GSK_LINE_JOIN_ROUND);
  gsk_stroke_set_line_cap(self->line_stroke, GSK_LINE_CAP_ROUND);
  self->dash_stroke = gsk_stroke_new(1.0f);
  const float dashes[2] = { 4.0f, 4.0f };
  gsk_stroke_set_dash(self->dash_stroke, dashes, G_N_ELEMENTS(dashes));

  self->label_attrs = pango_attr_list_new();
  pango_attr_list_insert(self->label_attrs, pango_attr_scale_new(LABEL_SCALE));
  for (gsize i = 0; i < MAX_Y_LABELS; i++)
    self->ylab[i] = make_label(self);
  for (gsize i = 0; i < MAX_X_LABELS; i++)
    self->xlab[i] = make_label(self);
  self->tip = make_label(self);
  self->stale_lbl = make_label(self);
  self->nodata = make_label(self);
  self->sample = make_label(self);
  set_text(self->stale_lbl, "Stale");
  set_text(self->nodata, "No data");

  GtkEventController *motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "motion", G_CALLBACK(on_motion), self);
  g_signal_connect(motion, "leave", G_CALLBACK(on_leave), self);
  gtk_widget_add_controller(widget, motion);

  /* Touch: scrubbing starts with a long press, so plain drags keep scrolling
   * any ancestor scrolled window. */
  GtkGesture *press = gtk_gesture_long_press_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(press), TRUE);
  g_signal_connect(press, "pressed", G_CALLBACK(on_long_press), self);
  GtkGesture *drag = gtk_gesture_drag_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(drag), TRUE);
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), self);
  g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), self);
  gtk_gesture_group(press, drag);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(press));
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(drag));
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

/* Documented in stk-chart.h. */
GtkWidget *
stk_chart_new(void)
{
  return g_object_new(STK_TYPE_CHART, NULL);
}

/**
 * set_direction_class:
 * @self: (not nullable): the chart.
 * @name: (nullable): "positive", "negative" or NULL for none.
 *
 * Moves the direction class to @name, touching the style only when it
 * actually changes.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_CHART(); @name is NULL or a NUL-terminated string
 *    (the only caller passes NULL or a string literal).
 * Postconditions:
 *  - the widget carries exactly the class equal to @name among "positive" /
 *    "negative" (neither when @name is NULL or matches neither).
 * Thread affinity: main thread only.
 * Ownership: @name is borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
set_direction_class(StkChart *self, const char *name)
{
  GtkWidget *widget = GTK_WIDGET(self);
  static const char *const classes[] = { "positive", "negative" };
  for (gsize i = 0; i < G_N_ELEMENTS(classes); i++) {
    /* @name is NULL or one of the two static class names, all NUL-terminated. */
    gboolean want = name != NULL && g_str_equal(name, classes[i]);
    if (want && !gtk_widget_has_css_class(widget, classes[i]))
      gtk_widget_add_css_class(widget, classes[i]);
    else if (!want && gtk_widget_has_css_class(widget, classes[i]))
      gtk_widget_remove_css_class(widget, classes[i]);
  }
}

/* Documented in stk-chart.h. */
void
stk_chart_set_series(StkChart *self, StkSeries *series)
{
  g_return_if_fail(STK_IS_CHART(self));
  if (self->disposed)
    return;

  self->hover = NO_HOVER;
  self->scrubbing = FALSE;
  self->has_data = FALSE;
  self->win_n = 0;
  self->prev_close = NAN;

  gsize n = series != NULL ? series->n_points : 0;
  if (n > 0) {
    /* At most STK_CHART_GEOM_MAX_SAMPLES samples are ever copied, whatever
     * the series length. */
    gsize cap = MAX(MIN(n, (gsize)STK_CHART_GEOM_MAX_SAMPLES), (gsize)4);
    if (cap > self->n_alloc) {
      self->t = g_renew(double, self->t, cap);
      self->v = g_renew(double, self->v, cap);
      self->n_alloc = cap;
    }
    self->range = series->range;
    if (series->range == STK_RANGE_1D && isfinite(series->prev_close))
      self->prev_close = series->prev_close;

    self->win_n = stk_chart_geom_sample(self->range, series->points, n, cap, self->t, self->v,
                                        &self->t0, &self->t1);
    if (self->win_n > 0)
      self->has_data = stk_chart_geom_y_range(self->v, self->win_n, self->prev_close,
                                              Y_PAD_FRAC, &self->lo, &self->hi);
  }

  if (self->has_data)
    set_direction_class(self, stk_chart_geom_is_up_vs_close(self->v, self->win_n, self->prev_close)
                                  ? "positive" : "negative");
  else
    set_direction_class(self, NULL);

  self->layout_dirty = TRUE;
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

/* Documented in stk-chart.h. */
void
stk_chart_set_stale(StkChart *self, gboolean stale)
{
  g_return_if_fail(STK_IS_CHART(self));
  stale = !!stale;
  if (self->stale == stale)
    return;
  self->stale = stale;
  self->hover = NO_HOVER; /* the tooltip placement depends on the marker */
  gtk_widget_queue_draw(GTK_WIDGET(self));
}
