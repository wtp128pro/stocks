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

G_DEFINE_FINAL_TYPE(StkSparkline, stk_sparkline, GTK_TYPE_WIDGET)

static void
stk_sparkline_measure(GtkWidget *widget G_GNUC_UNUSED, GtkOrientation orientation,
                      int for_size G_GNUC_UNUSED, int *minimum, int *natural,
                      int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural =
      orientation == GTK_ORIENTATION_HORIZONTAL ? STK_SPARKLINE_WIDTH : STK_SPARKLINE_HEIGHT;
  *minimum_baseline = -1;
  *natural_baseline = -1;
}

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

/* Drops the GSK references in dispose() so a reference cycle through the
 * widget cannot keep them alive. Idempotent. A NULL stroke marks the disposed
 * state: snapshot() and set_series() are then no-ops. */
static void
stk_sparkline_dispose(GObject *object)
{
  StkSparkline *self = STK_SPARKLINE(object);
  g_clear_pointer(&self->line, gsk_path_unref);
  g_clear_pointer(&self->stroke, gsk_stroke_free);
  G_OBJECT_CLASS(stk_sparkline_parent_class)->dispose(object);
}

static void
ignore_css_error(GtkCssProvider *provider G_GNUC_UNUSED, GtkCssSection *section G_GNUC_UNUSED,
                 GError *error G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
}

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

GtkWidget *
stk_sparkline_new(void)
{
  return g_object_new(STK_TYPE_SPARKLINE, NULL);
}

/* Builds the path for the visible window of @series; returns TRUE if there
 * is anything to draw. Sets *up for the CSS class. The series is read in
 * place; only SAMPLES doubles per axis are ever held, whatever its length. */
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
    gboolean on = want != NULL && strcmp(want, classes[i]) == 0;
    if (on && !gtk_widget_has_css_class(widget, classes[i]))
      gtk_widget_add_css_class(widget, classes[i]);
    else if (!on && gtk_widget_has_css_class(widget, classes[i]))
      gtk_widget_remove_css_class(widget, classes[i]);
  }
  gtk_widget_queue_draw(widget);
}
