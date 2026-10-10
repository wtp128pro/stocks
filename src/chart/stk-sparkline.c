/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-sparkline.c - StkSparkline: tiny fixed-size trend line for list rows.
 * See stk-sparkline.h for the public contract.
 *
 * The widget size is fixed, so the line is projected and decimated once in
 * set_series() into a GskPath; no sample data is kept and snapshot() only
 * replays that path.
 */
#include "stk-sparkline.h"

#include <math.h>
#include <string.h>

#include "stk-chart-geom.h"

#define PAD 2.0f
#define Y_PAD_FRAC 0.1
#define COLUMNS ((gsize)(STK_SPARKLINE_WIDTH - 4))
/* Samples kept from a series of any length (see stk_chart_geom_sample()). */
#define SAMPLES (4 * COLUMNS)

struct _StkSparkline {
  GtkWidget parent_instance;
  GskPath *line;     /* in widget-content coordinates, or NULL */
  gboolean has_dot;  /* a single visible point: draw a dot instead of a line */
  float dot_x, dot_y;
  GskStroke *stroke; /* immutable, built once */
};

/* Type boilerplate: G_DEFINE_FINAL_TYPE() defines stk_sparkline_get_type(),
 * stk_sparkline_parent_class and the instance-private plumbing; all are used
 * on the main thread only. */
G_DEFINE_FINAL_TYPE(StkSparkline, stk_sparkline, GTK_TYPE_WIDGET)

/**
 * stk_sparkline_measure:
 * @widget: unused.
 * @orientation: which dimension is measured.
 * @for_size: unused.
 * @minimum: (out): receives the minimum size.
 * @natural: (out): receives the natural size.
 * @minimum_baseline: (out): receives -1 (no baseline).
 * @natural_baseline: (out): receives -1 (no baseline).
 *
 * Reports the fixed STK_SPARKLINE_WIDTH x STK_SPARKLINE_HEIGHT size.
 *
 * Preconditions:
 *  - all four output pointers are non-NULL (GTK always passes them).
 * Postconditions:
 *  - minimum == natural == the width (horizontal) or height (vertical);
 *    both baselines are -1.
 * Thread affinity: main thread only.
 * Ownership: @widget is borrowed and unused; the outputs are caller-owned.
 *
 * Returns: void.
 */
static void
stk_sparkline_measure(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum,
                      int *natural, int *minimum_baseline, int *natural_baseline)
{
  (void)widget;
  (void)for_size;
  *minimum = *natural =
      orientation == GTK_ORIENTATION_HORIZONTAL ? STK_SPARKLINE_WIDTH : STK_SPARKLINE_HEIGHT;
  *minimum_baseline = -1;
  *natural_baseline = -1;
}

/**
 * stk_sparkline_snapshot:
 * @widget: (not nullable): the StkSparkline.
 * @snapshot: (not nullable): snapshot to append to.
 *
 * Replays the prepared path (or the single-sample dot) centred in the
 * allocation, in the widget colour.
 *
 * Preconditions:
 *  - called by GTK during drawing with a valid @snapshot.
 * Postconditions:
 *  - nothing is appended when disposed or when there is nothing to draw;
 *    otherwise the saved snapshot state is restored. The widget state is not
 *    changed.
 * Thread affinity: main thread only.
 * Ownership: @widget and @snapshot are borrowed; nothing is retained.
 *
 * Returns: void.
 */
static void
stk_sparkline_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
  StkSparkline *self = STK_SPARKLINE(widget);
  if (self->stroke == NULL || (self->line == NULL && !self->has_dot))
    return;

  GdkRGBA color;
  gtk_widget_get_color(widget, &color);

  /* Centre the fixed-size drawing if more room was allocated. */
  float ox = floorf(((float)gtk_widget_get_width(widget) - STK_SPARKLINE_WIDTH) / 2.0f);
  float oy = floorf(((float)gtk_widget_get_height(widget) - STK_SPARKLINE_HEIGHT) / 2.0f);
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &(graphene_point_t){ ox, oy });
  if (self->line != NULL) {
    gtk_snapshot_append_stroke(snapshot, self->line, self->stroke, &color);
  } else {
    GskRoundedRect rr;
    gsk_rounded_rect_init_from_rect(
        &rr, &(graphene_rect_t){ { self->dot_x - 1.5f, self->dot_y - 1.5f }, { 3.0f, 3.0f } },
        1.5f);
    gtk_snapshot_push_rounded_clip(snapshot, &rr);
    gtk_snapshot_append_color(snapshot, &color, &rr.bounds);
    gtk_snapshot_pop(snapshot);
  }
  gtk_snapshot_restore(snapshot);
}

/**
 * stk_sparkline_dispose:
 * @object: (not nullable): the StkSparkline being disposed.
 *
 * Drops the GSK references in dispose() so a reference cycle through the
 * widget cannot keep them alive. A NULL stroke marks the disposed state:
 * snapshot() and set_series() are then no-ops.
 *
 * Preconditions:
 *  - @object is a StkSparkline; may run more than once (g_object_run_dispose()
 *    then final unref).
 * Postconditions:
 *  - self->line and self->stroke are NULL; the call is idempotent because
 *    g_clear_pointer() ignores NULL. The parent dispose is chained. The
 *    widget keeps no raw pointers to itself in any controller or callback
 *    (it has none), so nothing can outlive it.
 * Thread affinity: main thread only.
 * Ownership: the path and stroke are owned by @object and released here.
 *
 * Returns: void.
 */
static void
stk_sparkline_dispose(GObject *object)
{
  StkSparkline *self = STK_SPARKLINE(object);
  g_clear_pointer(&self->line, gsk_path_unref);
  g_clear_pointer(&self->stroke, gsk_stroke_free);
  G_OBJECT_CLASS(stk_sparkline_parent_class)->dispose(object);
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
 * stk_sparkline_class_init:
 * @klass: (not nullable): the class structure being initialised.
 *
 * Class initialiser: installs dispose / measure / snapshot, the CSS node name
 * and, if a default display exists, the fallback colour style (same as
 * StkChart, see stk-chart.h).
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
stk_sparkline_class_init(StkSparklineClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = stk_sparkline_dispose;
  widget_class->measure = stk_sparkline_measure;
  widget_class->snapshot = stk_sparkline_snapshot;
  gtk_widget_class_set_css_name(widget_class, "stk-sparkline");

  /* Same fallback colours as StkChart (see stk-chart.h): @define-color at
   * FALLBACK priority only applies when no theme defines the names. */
  GdkDisplay *display = gdk_display_get_default();
  if (display != NULL) {
    GtkCssProvider *provider = gtk_css_provider_new();
    g_signal_connect(provider, "parsing-error", G_CALLBACK(ignore_css_error), NULL);
    gtk_css_provider_load_from_string(provider,
                                      "@define-color success_color #2ec27e;\n"
                                      "@define-color error_color #e01b24;\n"
                                      "stk-sparkline.positive { color: @success_color; }\n"
                                      "stk-sparkline.negative { color: @error_color; }\n");
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_FALLBACK);
    g_object_unref(provider);
  }
}

/**
 * stk_sparkline_init:
 * @self: (not nullable): the instance being initialised.
 *
 * Instance initialiser: centres the widget in its allocation and creates the
 * immutable 1.5 px round stroke.
 *
 * Preconditions:
 *  - called by GObject on a freshly allocated, zero-filled instance.
 * Postconditions:
 *  - self->stroke is non-NULL (until dispose()); self->line is NULL and
 *    has_dot is FALSE.
 * Thread affinity: main thread only.
 * Ownership: the stroke is owned by @self and freed in dispose().
 *
 * Returns: void.
 */
static void
stk_sparkline_init(StkSparkline *self)
{
  GtkWidget *widget = GTK_WIDGET(self);
  gtk_widget_set_halign(widget, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(widget, GTK_ALIGN_CENTER);

  self->stroke = gsk_stroke_new(1.5f);
  gsk_stroke_set_line_join(self->stroke, GSK_LINE_JOIN_ROUND);
  gsk_stroke_set_line_cap(self->stroke, GSK_LINE_CAP_ROUND);
}

/* Documented in stk-sparkline.h. */
GtkWidget *
stk_sparkline_new(void)
{
  return g_object_new(STK_TYPE_SPARKLINE, NULL);
}

/**
 * build:
 * @self: (not nullable): the sparkline whose path / dot is replaced.
 * @series: (not nullable): series to read in place.
 * @up: (out): receives whether the drawn samples are up (last >= first).
 *
 * Builds the path for the visible window of @series; returns TRUE if there
 * is anything to draw. Sets *up for the CSS class. The series is read in
 * place; only SAMPLES doubles per axis are ever held, whatever its length.
 *
 * Preconditions:
 *  - @self satisfies STK_IS_SPARKLINE(), is not disposed and has no path
 *    (self->line == NULL; set_series() clears it first); @series and @up are
 *    non-NULL; series->points is valid for series->n_points samples.
 *  - stack bound: t[] and v[] are SAMPLES = 4 * COLUMNS = 272 doubles each
 *    (4352 bytes) and idx[] is 2 * COLUMNS = 136 gsize (1088 bytes). They are
 *    never overrun because stk_chart_geom_sample() writes at most its
 *    @max_out = SAMPLES entries, stk_chart_geom_decimate() at most its
 *    @out_cap = G_N_ELEMENTS(idx) entries, and every idx[k] it returns is
 *    below win_n <= SAMPLES.
 * Postconditions:
 *  - on TRUE, either self->line is a new path (two or more kept samples) or
 *    self->has_dot / dot_x / dot_y describe the single kept sample; *@up is
 *    set. On FALSE self->line stays NULL, there is no dot and *@up is
 *    unspecified. The temporary GskPathBuilder is always consumed (freed to
 *    a path or unreferenced).
 *  - the float conversions of x and y are of values mapped into
 *    [PAD, size - PAD] from finite samples inside the domain, so they are in
 *    float range.
 * Thread affinity: main thread only.
 * Ownership: @series is borrowed for the call; self->line (if set) is owned
 *   by @self and released by set_series() or dispose().
 *
 * Returns: TRUE if something can be drawn, FALSE otherwise.
 */
static gboolean
build(StkSparkline *self, const StkSeries *series, gboolean *up)
{
  double t[SAMPLES], v[SAMPLES];
  double t0, t1, lo, hi;
  gsize win_n = stk_chart_geom_sample(series->range, series->points, series->n_points, SAMPLES, t,
                                      v, &t0, &t1);
  if (win_n == 0 || !stk_chart_geom_y_range(v, win_n, NAN, Y_PAD_FRAC, &lo, &hi))
    return FALSE;

  gsize idx[2 * COLUMNS];
  gsize kept = stk_chart_geom_decimate(t, v, win_n, t0, t1, COLUMNS, idx, G_N_ELEMENTS(idx));
  GskPathBuilder *builder = gsk_path_builder_new();
  float x = 0.0f, y = 0.0f;
  for (gsize k = 0; k < kept; k++) {
    x = (float)stk_chart_geom_map(t[idx[k]], t0, t1, PAD, STK_SPARKLINE_WIDTH - PAD);
    y = (float)stk_chart_geom_map(v[idx[k]], lo, hi, STK_SPARKLINE_HEIGHT - PAD, PAD);
    if (k == 0)
      gsk_path_builder_move_to(builder, x, y);
    else
      gsk_path_builder_line_to(builder, x, y);
  }
  if (kept >= 2) {
    self->line = gsk_path_builder_free_to_path(builder);
  } else {
    gsk_path_builder_unref(builder);
    self->has_dot = kept == 1;
    self->dot_x = x;
    self->dot_y = y;
  }
  *up = stk_chart_geom_is_up(v, win_n);
  return kept > 0;
}

/* Documented in stk-sparkline.h. */
void
stk_sparkline_set_series(StkSparkline *self, StkSeries *series)
{
  g_return_if_fail(STK_IS_SPARKLINE(self));
  if (self->stroke == NULL) /* disposed: nothing may be built any more */
    return;

  g_clear_pointer(&self->line, gsk_path_unref);
  self->has_dot = FALSE;

  gboolean up = TRUE;
  gboolean drawn = series != NULL && series->n_points > 0 && build(self, series, &up);

  GtkWidget *widget = GTK_WIDGET(self);
  const char *want = drawn ? (up ? "positive" : "negative") : NULL;
  static const char *const classes[] = { "positive", "negative" };
  for (gsize i = 0; i < G_N_ELEMENTS(classes); i++) {
    /* @want is NULL or one of the two static class names, both NUL-terminated. */
    gboolean on = want != NULL && g_str_equal(want, classes[i]);
    if (on && !gtk_widget_has_css_class(widget, classes[i]))
      gtk_widget_add_css_class(widget, classes[i]);
    else if (!on && gtk_widget_has_css_class(widget, classes[i]))
      gtk_widget_remove_css_class(widget, classes[i]);
  }
  gtk_widget_queue_draw(widget);
}
