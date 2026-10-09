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

G_DEFINE_FINAL_TYPE(StkChart, stk_chart, GTK_TYPE_WIDGET)

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static GdkRGBA
with_alpha(GdkRGBA c, float a)
{
  c.alpha *= a;
  return c;
}

static void
free_paths(StkChart *self)
{
  g_clear_pointer(&self->line, gsk_path_unref);
  g_clear_pointer(&self->fill, gsk_path_unref);
  g_clear_pointer(&self->prev, gsk_path_unref);
  self->has_dot = FALSE;
}

static void
clear_hover(StkChart *self)
{
  if (self->hover != NO_HOVER) {
    self->hover = NO_HOVER;
    gtk_widget_queue_draw(GTK_WIDGET(self));
  }
}

static PangoLayout *
make_label(StkChart *self)
{
  PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), NULL);
  pango_layout_set_attributes(layout, self->label_attrs);
  return layout;
}

static void
set_text(PangoLayout *layout, const char *text)
{
  pango_layout_set_text(layout, text, -1);
}

/* ------------------------------------------------------------------------ */
/* Layout rebuild (size / data / style change only)                         */
/* ------------------------------------------------------------------------ */

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
  gsize max_y = (gsize)(plot_h / (3.0f * (float)self->text_h));
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
  self->n_xt = stk_chart_geom_time_ticks(self->range, (gint64)self->t0, (gint64)self->t1, max_x, xt);
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
  stk_chart_geom_format_hover(self->range, (gint64)wt[i], when, sizeof when);
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

static void
on_motion(GtkEventControllerMotion *controller G_GNUC_UNUSED, double x, double y G_GNUC_UNUSED,
          gpointer user_data)
{
  set_hover_from_x(STK_CHART(user_data), x);
}

static void
on_leave(GtkEventControllerMotion *controller G_GNUC_UNUSED, gpointer user_data)
{
  StkChart *self = STK_CHART(user_data);
  if (!self->scrubbing)
    clear_hover(self);
}

static void
on_long_press(GtkGestureLongPress *gesture, double x, double y G_GNUC_UNUSED, gpointer user_data)
{
  StkChart *self = STK_CHART(user_data);
  self->scrubbing = TRUE;
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  set_hover_from_x(self, x);
}

static void
on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y G_GNUC_UNUSED,
               gpointer user_data)
{
  StkChart *self = STK_CHART(user_data);
  if (!self->scrubbing)
    return;
  double sx, sy;
  gtk_gesture_drag_get_start_point(gesture, &sx, &sy);
  set_hover_from_x(self, sx + offset_x);
}

static void
on_drag_end(GtkGestureDrag *gesture G_GNUC_UNUSED, double offset_x G_GNUC_UNUSED,
            double offset_y G_GNUC_UNUSED, gpointer user_data)
{
  StkChart *self = STK_CHART(user_data);
  self->scrubbing = FALSE;
  clear_hover(self);
}

/* ------------------------------------------------------------------------ */
/* GtkWidget vfuncs                                                         */
/* ------------------------------------------------------------------------ */

static void
stk_chart_measure(GtkWidget *widget G_GNUC_UNUSED, GtkOrientation orientation,
                  int for_size G_GNUC_UNUSED, int *minimum, int *natural, int *minimum_baseline,
                  int *natural_baseline)
{
  *minimum = orientation == GTK_ORIENTATION_HORIZONTAL ? MIN_WIDTH : MIN_HEIGHT;
  *natural = orientation == GTK_ORIENTATION_HORIZONTAL ? NATURAL_WIDTH : NATURAL_HEIGHT;
  *minimum_baseline = -1;
  *natural_baseline = -1;
}

static void
stk_chart_size_allocate(GtkWidget *widget, int width, int height, int baseline G_GNUC_UNUSED)
{
  StkChart *self = STK_CHART(widget);
  if (width != self->width || height != self->height) {
    self->width = width;
    self->height = height;
    self->layout_dirty = TRUE;
    self->hover = NO_HOVER;
  }
}

/* Fonts, scale or text direction changed: every cached layout must re-read
 * the (updated) widget Pango context, and all geometry and the hover that
 * depended on the old label sizes are stale. */
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

static void
stk_chart_css_changed(GtkWidget *widget, GtkCssStyleChange *change)
{
  GTK_WIDGET_CLASS(stk_chart_parent_class)->css_changed(widget, change);
  style_changed(STK_CHART(widget));
}

static void
stk_chart_system_setting_changed(GtkWidget *widget, GtkSystemSetting setting)
{
  GTK_WIDGET_CLASS(stk_chart_parent_class)->system_setting_changed(widget, setting);
  style_changed(STK_CHART(widget));
  gtk_widget_queue_draw(widget);
}

static void
append_layout_at(GtkSnapshot *snapshot, PangoLayout *layout, float x, float y, const GdkRGBA *color)
{
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, POINT(x, y));
  gtk_snapshot_append_layout(snapshot, layout, color);
  gtk_snapshot_restore(snapshot);
}

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

/* Drops every GObject / GSK reference (layouts, strokes, paths) so that a
 * reference cycle through the widget cannot keep them alive. Idempotent. */
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

static void
stk_chart_finalize(GObject *object)
{
  StkChart *self = STK_CHART(object);
  g_free(self->t);
  g_free(self->v);
  g_free(self->idx_buf);
  G_OBJECT_CLASS(stk_chart_parent_class)->finalize(object);
}

static void
ignore_css_error(GtkCssProvider *provider G_GNUC_UNUSED, GtkCssSection *section G_GNUC_UNUSED,
                 GError *error G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
}

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

GtkWidget *
stk_chart_new(void)
{
  return g_object_new(STK_TYPE_CHART, NULL);
}

/* Moves the direction class to @name ("positive", "negative" or NULL for
 * none), touching the style only when it actually changes. */
static void
set_direction_class(StkChart *self, const char *name)
{
  GtkWidget *widget = GTK_WIDGET(self);
  static const char *const classes[] = { "positive", "negative" };
  for (gsize i = 0; i < G_N_ELEMENTS(classes); i++) {
    gboolean want = name != NULL && strcmp(name, classes[i]) == 0;
    if (want && !gtk_widget_has_css_class(widget, classes[i]))
      gtk_widget_add_css_class(widget, classes[i]);
    else if (!want && gtk_widget_has_css_class(widget, classes[i]))
      gtk_widget_remove_css_class(widget, classes[i]);
  }
}

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
    set_direction_class(self, stk_chart_geom_is_up(self->v, self->win_n) ? "positive" : "negative");
  else
    set_direction_class(self, NULL);

  self->layout_dirty = TRUE;
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

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
