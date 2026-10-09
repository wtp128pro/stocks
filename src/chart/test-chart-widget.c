/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * test-chart-widget.c - StkChart / StkSparkline under a display.
 *
 * Needs a GDK display (run under xvfb-run); exits 77 (meson: skipped) when
 * gtk_init_check() fails. The tests run under a bare theme created in a temp
 * directory, so they do not depend on the installed theme.
 */
#include <glib/gstdio.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "stk-chart.h"
#include "stk-sparkline.h"

#define WIN_W 440
#define WIN_H 280

/* ------------------------------------------------------------------ */
/* Fixtures                                                           */
/* ------------------------------------------------------------------ */

static gint64
utc_ms(int y, int mo, int d, int h, int mi)
{
  g_autoptr(GDateTime) dt = g_date_time_new_utc(y, mo, d, h, mi, 0);
  g_assert_nonnull(dt);
  return g_date_time_to_unix(dt) * 1000;
}

/* Thursday 2026-10-08, one sample per minute from @from_h:@from_m (UTC) for
 * @minutes; session is 13:30-20:00 UTC. */
static StkSeries *
make_1d(int from_h, int from_m, int minutes, double start, double drift)
{
  StkSeries *s = stk_series_new(STK_RANGE_1D, 100.0, (gsize)minutes);
  gint64 t0 = utc_ms(2026, 10, 8, from_h, from_m);
  for (int i = 0; i < minutes; i++)
    g_assert_true(stk_series_append(s, t0 + i * 60000, start + drift * i + sin(i / 7.0)));
  return s;
}

static StkSeries *
make_daily(StkRange range, int days, double start, double drift)
{
  StkSeries *s = stk_series_new(range, NAN, (gsize)days);
  gint64 end = utc_ms(2026, 10, 8, 0, 0); /* daily bars: 00:00 UTC of the trading date */
  for (int i = 0; i < days; i++)
    g_assert_true(stk_series_append(s, end - (gint64)(days - 1 - i) * 86400000,
                                    start + drift * i + 2.0 * sin(i / 5.0)));
  return s;
}

typedef struct {
  GtkWidget *window;
  GtkWidget *widget;
} Fixture;

static void
pump(guint ms)
{
  gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;
  GMainContext *ctx = g_main_context_default();
  while (g_get_monotonic_time() < end) {
    while (g_main_context_iteration(ctx, FALSE))
      ;
    g_usleep(5000);
  }
}

static Fixture
fixture_new(GtkWidget *widget)
{
  Fixture f = { gtk_window_new(), widget };
  gtk_window_set_default_size(GTK_WINDOW(f.window), WIN_W, WIN_H);
  gtk_window_set_child(GTK_WINDOW(f.window), widget);
  gtk_window_present(GTK_WINDOW(f.window));
  for (int i = 0; i < 200 && gtk_widget_get_width(widget) <= 0; i++)
    pump(10);
  g_assert_cmpint(gtk_widget_get_width(widget), >, 0);
  g_assert_cmpint(gtk_widget_get_height(widget), >, 0);
  return f;
}

static void
fixture_free(Fixture *f)
{
  gtk_window_destroy(GTK_WINDOW(f->window));
  pump(20);
}

/* Serialized render tree of one snapshot() call (empty bytes for no nodes). */
static GBytes *
snap(GtkWidget *w)
{
  GtkSnapshot *s = gtk_snapshot_new();
  GTK_WIDGET_GET_CLASS(w)->snapshot(w, s);
  GskRenderNode *node = gtk_snapshot_free_to_node(s);
  if (node == NULL)
    return g_bytes_new_static("", 0);
  GBytes *bytes = gsk_render_node_serialize(node);
  gsk_render_node_unref(node);
  return bytes;
}

static void
assert_snaps_equal(GBytes *a, GBytes *b)
{
  g_assert_true(g_bytes_equal(a, b));
}

static void
assert_snaps_differ(GBytes *a, GBytes *b)
{
  g_assert_false(g_bytes_equal(a, b));
}

/* Size of the serialized render tree of one snapshot() call. */
static gsize
snap_size(GtkWidget *w)
{
  GBytes *b = snap(w);
  gsize size = g_bytes_get_size(b);
  g_bytes_unref(b);
  return size;
}

/* Asserts that a fresh snapshot of @w serializes exactly like @expected. */
static void
assert_snap_is(GBytes *expected, GtkWidget *w)
{
  GBytes *b = snap(w);
  assert_snaps_equal(expected, b);
  g_bytes_unref(b);
}

static GtkEventControllerMotion *
find_motion(GtkWidget *w)
{
  GListModel *m = gtk_widget_observe_controllers(w);
  GtkEventControllerMotion *found = NULL;
  for (guint i = 0; i < g_list_model_get_n_items(m); i++) {
    GObject *o = g_list_model_get_item(m, i);
    if (GTK_IS_EVENT_CONTROLLER_MOTION(o))
      found = GTK_EVENT_CONTROLLER_MOTION(o);
    g_object_unref(o);
  }
  g_object_unref(m);
  g_assert_nonnull(found);
  return found;
}

static void
motion(GtkWidget *w, double x)
{
  g_signal_emit_by_name(find_motion(w), "motion", x, 100.0);
}

static void
leave(GtkWidget *w)
{
  g_signal_emit_by_name(find_motion(w), "leave");
}

/* ------------------------------------------------------------------ */
/* StkChart tests                                                     */
/* ------------------------------------------------------------------ */

static void
test_chart_measure(void)
{
  GtkWidget *chart = g_object_ref_sink(stk_chart_new());
  int min, nat;
  gtk_widget_measure(chart, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  g_assert_cmpint(min, ==, 120);
  g_assert_cmpint(nat, >=, min);
  gtk_widget_measure(chart, GTK_ORIENTATION_VERTICAL, -1, &min, &nat, NULL, NULL);
  g_assert_cmpint(min, ==, 80);
  g_assert_cmpint(nat, >=, min);
  g_assert_true(gtk_widget_get_hexpand(chart));
  g_object_unref(chart);
}

static void
test_chart_no_data(void)
{
  Fixture f = fixture_new(stk_chart_new());
  StkChart *chart = STK_CHART(f.widget);

  GBytes *empty = snap(f.widget);
  g_assert_cmpuint(g_bytes_get_size(empty), >, 0); /* the "No data" text */

  stk_chart_set_series(chart, NULL);
  GBytes *null_snap = snap(f.widget);
  assert_snaps_equal(empty, null_snap);

  g_autoptr(StkSeries) none = stk_series_new(STK_RANGE_1M, NAN, 4);
  stk_chart_set_series(chart, none); /* empty series */
  GBytes *empty_series = snap(f.widget);
  assert_snaps_equal(empty, empty_series);

  /* 1D samples that are all after the close: nothing in the domain */
  g_autoptr(StkSeries) after = make_1d(21, 0, 30, 100, 0.1);
  stk_chart_set_series(chart, NULL);
  stk_chart_set_series(chart, after);
  /* last sample (21:29Z) is on a session date, but outside 13:30-20:00 */
  GBytes *after_snap = snap(f.widget);
  assert_snaps_equal(empty, after_snap);
  g_assert_false(gtk_widget_has_css_class(f.widget, "positive"));
  g_assert_false(gtk_widget_has_css_class(f.widget, "negative"));

  /* hover without data is a harmless no-op */
  motion(f.widget, 100);
  leave(f.widget);
  assert_snap_is(empty, f.widget);

  g_bytes_unref(empty);
  g_bytes_unref(null_snap);
  g_bytes_unref(empty_series);
  g_bytes_unref(after_snap);
  fixture_free(&f);
}

static void
test_chart_data_and_direction(void)
{
  Fixture f = fixture_new(stk_chart_new());
  StkChart *chart = STK_CHART(f.widget);
  GBytes *empty = snap(f.widget);

  g_autoptr(StkSeries) up = make_daily(STK_RANGE_3M, 60, 100, 0.5);
  stk_chart_set_series(chart, up);
  GBytes *up_snap = snap(f.widget);
  assert_snaps_differ(empty, up_snap);
  g_assert_true(gtk_widget_has_css_class(f.widget, "positive"));
  g_assert_false(gtk_widget_has_css_class(f.widget, "negative"));
  pump(30);

  g_autoptr(StkSeries) down = make_daily(STK_RANGE_1M, 21, 100, -1.0);
  stk_chart_set_series(chart, down);
  GBytes *down_snap = snap(f.widget);
  assert_snaps_differ(up_snap, down_snap);
  g_assert_true(gtk_widget_has_css_class(f.widget, "negative"));
  g_assert_false(gtk_widget_has_css_class(f.widget, "positive"));
  pump(30);

  /* Resolved colours: positive is greenish, negative reddish (fallback CSS
   * or theme colours). */
  GdkRGBA c;
  gtk_widget_get_color(f.widget, &c);
  g_assert_cmpfloat(c.red, >, c.green);
  stk_chart_set_series(chart, up);
  pump(30);
  gtk_widget_get_color(f.widget, &c);
  g_assert_cmpfloat(c.green, >, c.red);

  /* All ranges, 1D with pre/after-market data, one sample, flat series. */
  StkRange ranges[4] = { STK_RANGE_1M, STK_RANGE_3M, STK_RANGE_6M, STK_RANGE_1Y };
  for (int i = 0; i < 4; i++) {
    g_autoptr(StkSeries) s = make_daily(ranges[i], 30 + 40 * i, 50, 0.1);
    stk_chart_set_series(chart, s);
    g_bytes_unref(snap(f.widget));
  }
  g_autoptr(StkSeries) wide = make_1d(8, 0, 800, 100, 0.01);
  stk_chart_set_series(chart, wide);
  g_bytes_unref(snap(f.widget));

  g_autoptr(StkSeries) one = stk_series_new(STK_RANGE_1D, NAN, 1);
  stk_series_append(one, utc_ms(2026, 10, 8, 15, 0), 42.0);
  stk_chart_set_series(chart, one);
  GBytes *one_snap = snap(f.widget);
  assert_snaps_differ(empty, one_snap);
  g_bytes_unref(one_snap);

  g_autoptr(StkSeries) flat = stk_series_new(STK_RANGE_1Y, NAN, 10);
  for (int i = 0; i < 10; i++)
    stk_series_append(flat, utc_ms(2026, 1, 1 + i, 12, 0), 77.0);
  stk_chart_set_series(chart, flat);
  g_bytes_unref(snap(f.widget));
  g_assert_true(gtk_widget_has_css_class(f.widget, "positive")); /* last >= first */

  g_autoptr(StkSeries) dense = stk_series_new(STK_RANGE_1D, 100.0, 10000);
  gint64 t0 = utc_ms(2026, 10, 8, 13, 30);
  for (int i = 0; i < 10000; i++)
    stk_series_append(dense, t0 + i * 2000, 100 + 5 * sin(i / 300.0) + (i % 7) * 0.1);
  stk_chart_set_series(chart, dense);
  g_bytes_unref(snap(f.widget));

  g_bytes_unref(empty);
  g_bytes_unref(up_snap);
  g_bytes_unref(down_snap);
  fixture_free(&f);
}

static void
test_chart_stale(void)
{
  Fixture f = fixture_new(stk_chart_new());
  StkChart *chart = STK_CHART(f.widget);
  g_autoptr(StkSeries) s = make_1d(13, 30, 120, 100, 0.02);
  stk_chart_set_series(chart, s);

  GBytes *fresh = snap(f.widget);
  stk_chart_set_stale(chart, TRUE);
  GBytes *stale = snap(f.widget);
  assert_snaps_differ(fresh, stale);
  stk_chart_set_stale(chart, TRUE); /* no change */
  GBytes *stale2 = snap(f.widget);
  assert_snaps_equal(stale, stale2);
  stk_chart_set_stale(chart, FALSE);
  GBytes *fresh2 = snap(f.widget);
  assert_snaps_equal(fresh, fresh2);

  /* stale marker also shows without data */
  stk_chart_set_series(chart, NULL);
  GBytes *nodata = snap(f.widget);
  stk_chart_set_stale(chart, TRUE);
  GBytes *nodata_stale = snap(f.widget);
  assert_snaps_differ(nodata, nodata_stale);

  g_bytes_unref(fresh);
  g_bytes_unref(stale);
  g_bytes_unref(stale2);
  g_bytes_unref(fresh2);
  g_bytes_unref(nodata);
  g_bytes_unref(nodata_stale);
  fixture_free(&f);
}

static void
test_chart_hover(void)
{
  Fixture f = fixture_new(stk_chart_new());
  StkChart *chart = STK_CHART(f.widget);
  /* 13:30Z..16:00Z of a 13:30Z..20:00Z session: data fills the left ~38% */
  g_autoptr(StkSeries) s = make_1d(13, 30, 151, 100, 0.02);
  stk_chart_set_series(chart, s);
  GBytes *base = snap(f.widget);

  motion(f.widget, 60);
  GBytes *h1 = snap(f.widget);
  assert_snaps_differ(base, h1);

  motion(f.widget, 61); /* may or may not change the nearest sample */
  motion(f.widget, 110);
  GBytes *h2 = snap(f.widget);
  assert_snaps_differ(h1, h2);

  /* far right of the plot, beyond the last sample: no hover */
  motion(f.widget, 330);
  GBytes *beyond = snap(f.widget);
  assert_snaps_equal(base, beyond);

  /* both ends of the data */
  motion(f.widget, 4);
  GBytes *left = snap(f.widget);
  assert_snaps_differ(base, left);
  motion(f.widget, 4 + 0.38 * 376 + 2);
  GBytes *right = snap(f.widget);
  assert_snaps_differ(base, right);
  assert_snaps_differ(left, right);

  /* outside on the left clears it */
  motion(f.widget, -50);
  assert_snap_is(base, f.widget);

  motion(f.widget, 60);
  leave(f.widget);
  GBytes *after_leave = snap(f.widget);
  assert_snaps_equal(base, after_leave);

  /* hover then stale toggles/resizes/new series all drop the crosshair */
  motion(f.widget, 60);
  stk_chart_set_series(chart, s);
  assert_snap_is(base, f.widget);
  motion(f.widget, 60);
  stk_chart_set_stale(chart, TRUE);
  stk_chart_set_stale(chart, FALSE);
  assert_snap_is(base, f.widget);

  g_bytes_unref(base);
  g_bytes_unref(h1);
  g_bytes_unref(h2);
  g_bytes_unref(beyond);
  g_bytes_unref(left);
  g_bytes_unref(right);
  g_bytes_unref(after_leave);
  fixture_free(&f);
}

static void
test_chart_resize(void)
{
  /* A spacer next to the chart takes width away from it when it grows. */
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *chart_widget = stk_chart_new();
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(box), chart_widget);
  gtk_box_append(GTK_BOX(box), spacer);
  Fixture f = fixture_new(box);
  StkChart *chart = STK_CHART(chart_widget);

  g_autoptr(StkSeries) s = make_daily(STK_RANGE_6M, 130, 100, 0.3);
  stk_chart_set_series(chart, s);
  GBytes *wide = snap(chart_widget);
  int old_w = gtk_widget_get_width(chart_widget);

  motion(chart_widget, 50);
  gtk_widget_set_size_request(spacer, 200, -1);
  for (int i = 0; i < 100 && gtk_widget_get_width(chart_widget) == old_w; i++)
    pump(20);
  g_assert_cmpint(gtk_widget_get_width(chart_widget), <, old_w);
  g_assert_cmpint(gtk_widget_get_width(chart_widget), >=, 120);
  GBytes *narrow = snap(chart_widget);
  assert_snaps_differ(wide, narrow);

  /* the resize dropped the old crosshair (positions were for the old size) */
  GBytes *again = snap(chart_widget);
  assert_snaps_equal(narrow, again);
  motion(chart_widget, 20);
  GBytes *hovered = snap(chart_widget);
  assert_snaps_differ(narrow, hovered);

  g_bytes_unref(wide);
  g_bytes_unref(narrow);
  g_bytes_unref(again);
  g_bytes_unref(hovered);
  fixture_free(&f);
}

/* Counts the chart's own snapshot() calls by wrapping the class vfunc for the
 * duration of the test (independent of window/compositor frame timing). */
static guint chart_snapshots;
static void (*orig_chart_snapshot)(GtkWidget *, GtkSnapshot *);

static void
counting_snapshot(GtkWidget *w, GtkSnapshot *s)
{
  chart_snapshots++;
  orig_chart_snapshot(w, s);
}

/* Pumps the main loop until the chart has been snapshotted at least @min
 * times since @from, or 10 s have passed. */
static void
wait_snapshots(guint from, guint min)
{
  gint64 end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (chart_snapshots - from < min && g_get_monotonic_time() < end)
    pump(10);
  g_assert_cmpuint(chart_snapshots - from, >=, min);
}

/* Pumps until no chart snapshot happened for 700 ms (max 15 s). */
static void
settle(void)
{
  gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
  guint seen = chart_snapshots;
  gint64 quiet_since = g_get_monotonic_time();
  while (g_get_monotonic_time() < deadline) {
    pump(10);
    if (chart_snapshots != seen) {
      seen = chart_snapshots;
      quiet_since = g_get_monotonic_time();
    } else if (g_get_monotonic_time() - quiet_since >= 700 * 1000) {
      return;
    }
  }
  g_assert_not_reached();
}

/* The widget must neither animate nor repaint while nothing changes, and a
 * hover on the same sample must not queue another draw. */
static void
test_chart_idle_and_redraw(void)
{
  Fixture f = fixture_new(stk_chart_new());
  StkChart *chart = STK_CHART(f.widget);
  g_autoptr(StkSeries) s = make_1d(13, 30, 151, 100, 0.02);
  stk_chart_set_series(chart, s);

  GtkWidgetClass *klass = GTK_WIDGET_GET_CLASS(f.widget);
  orig_chart_snapshot = klass->snapshot;
  klass->snapshot = counting_snapshot;
  chart_snapshots = 0;

  settle();
  guint base = chart_snapshots;
  pump(500);
  g_assert_cmpuint(chart_snapshots, ==, base); /* idle: no repaint */

  motion(f.widget, 60);
  wait_snapshots(base, 1);
  settle();
  base = chart_snapshots;
  motion(f.widget, 60); /* same sample again */
  pump(500);
  g_assert_cmpuint(chart_snapshots, ==, base);

  leave(f.widget);
  wait_snapshots(base, 1);
  settle();
  base = chart_snapshots;
  leave(f.widget); /* nothing to clear */
  pump(500);
  g_assert_cmpuint(chart_snapshots, ==, base);

  stk_chart_set_stale(chart, TRUE);
  wait_snapshots(base, 1);
  settle();
  base = chart_snapshots;
  stk_chart_set_stale(chart, TRUE);
  pump(500);
  g_assert_cmpuint(chart_snapshots, ==, base);

  klass->snapshot = orig_chart_snapshot;
  fixture_free(&f);
}

/* ------------------------------------------------------------------ */
/* StkSparkline tests                                                 */
/* ------------------------------------------------------------------ */

static void
test_sparkline(void)
{
  GtkWidget *spark = g_object_ref_sink(stk_sparkline_new());
  int min, nat;
  gtk_widget_measure(spark, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  g_assert_cmpint(min, ==, STK_SPARKLINE_WIDTH);
  g_assert_cmpint(nat, ==, STK_SPARKLINE_WIDTH);
  gtk_widget_measure(spark, GTK_ORIENTATION_VERTICAL, -1, &min, &nat, NULL, NULL);
  g_assert_cmpint(min, ==, STK_SPARKLINE_HEIGHT);
  g_assert_cmpint(nat, ==, STK_SPARKLINE_HEIGHT);
  g_object_unref(spark);

  Fixture f = fixture_new(stk_sparkline_new());
  StkSparkline *sp = STK_SPARKLINE(f.widget);
  g_assert_cmpint(gtk_widget_get_width(f.widget), ==, STK_SPARKLINE_WIDTH);
  g_assert_cmpint(gtk_widget_get_height(f.widget), ==, STK_SPARKLINE_HEIGHT);

  GBytes *empty = snap(f.widget);
  g_assert_cmpuint(g_bytes_get_size(empty), ==, 0);
  stk_sparkline_set_series(sp, NULL);
  g_assert_cmpuint(g_bytes_get_size(empty), ==, 0);

  g_autoptr(StkSeries) up = make_1d(13, 30, 200, 100, 0.05);
  stk_sparkline_set_series(sp, up);
  GBytes *up_snap = snap(f.widget);
  g_assert_cmpuint(g_bytes_get_size(up_snap), >, 0);
  g_assert_true(gtk_widget_has_css_class(f.widget, "positive"));

  g_autoptr(StkSeries) down = make_daily(STK_RANGE_1Y, 250, 100, -0.2);
  stk_sparkline_set_series(sp, down);
  GBytes *down_snap = snap(f.widget);
  assert_snaps_differ(up_snap, down_snap);
  g_assert_true(gtk_widget_has_css_class(f.widget, "negative"));
  g_assert_false(gtk_widget_has_css_class(f.widget, "positive"));

  g_autoptr(StkSeries) one = stk_series_new(STK_RANGE_1M, NAN, 1);
  stk_series_append(one, utc_ms(2026, 10, 8, 12, 0), 10.0);
  stk_sparkline_set_series(sp, one);
  GBytes *one_snap = snap(f.widget);
  g_assert_cmpuint(g_bytes_get_size(one_snap), >, 0);

  g_autoptr(StkSeries) flat = stk_series_new(STK_RANGE_3M, NAN, 5);
  for (int i = 0; i < 5; i++)
    stk_series_append(flat, utc_ms(2026, 9, 1 + i, 12, 0), 5.0);
  stk_sparkline_set_series(sp, flat);
  g_assert_cmpuint(snap_size(f.widget), >, 0);

  g_autoptr(StkSeries) dense = stk_series_new(STK_RANGE_1D, NAN, 10000);
  for (int i = 0; i < 10000; i++)
    stk_series_append(dense, utc_ms(2026, 10, 8, 13, 30) + i * 2000, 10 + sin(i / 50.0));
  stk_sparkline_set_series(sp, dense);
  g_assert_cmpuint(snap_size(f.widget), >, 0);

  /* 1D data entirely outside the session or an empty series draw nothing */
  g_autoptr(StkSeries) after = make_1d(21, 0, 30, 100, 0.1);
  stk_sparkline_set_series(sp, after);
  g_assert_cmpuint(snap_size(f.widget), ==, 0);
  g_assert_false(gtk_widget_has_css_class(f.widget, "positive"));
  g_assert_false(gtk_widget_has_css_class(f.widget, "negative"));
  g_autoptr(StkSeries) none = stk_series_new(STK_RANGE_1M, NAN, 0);
  stk_sparkline_set_series(sp, up);
  stk_sparkline_set_series(sp, none);
  g_assert_cmpuint(snap_size(f.widget), ==, 0);

  g_bytes_unref(empty);
  g_bytes_unref(up_snap);
  g_bytes_unref(down_snap);
  g_bytes_unref(one_snap);
  fixture_free(&f);
}

/* Both widgets in one list-like box, as the app will use them. */
static void
test_in_box(void)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *spark = stk_sparkline_new();
  GtkWidget *chart = stk_chart_new();
  gtk_box_append(GTK_BOX(box), spark);
  gtk_box_append(GTK_BOX(box), chart);
  Fixture f = fixture_new(box);

  g_autoptr(StkSeries) s = make_daily(STK_RANGE_1M, 21, 100, 1.0);
  stk_sparkline_set_series(STK_SPARKLINE(spark), s);
  stk_chart_set_series(STK_CHART(chart), s);
  pump(100);
  g_assert_cmpint(gtk_widget_get_width(spark), ==, STK_SPARKLINE_WIDTH);
  g_assert_cmpint(gtk_widget_get_width(chart), ==, WIN_W);
  g_bytes_unref(snap(chart));
  g_bytes_unref(snap(spark));
  fixture_free(&f);
}

/* ------------------------------------------------------------------ */
/* Review-driven regression tests                                     */
/* ------------------------------------------------------------------ */

static gboolean
color_is(const GdkRGBA *c, int r, int g, int b)
{
  return fabsf(c->red - (float)r / 255.0f) < 0.01f && fabsf(c->green - (float)g / 255.0f) < 0.01f &&
         fabsf(c->blue - (float)b / 255.0f) < 0.01f;
}

/* With a theme that defines no @success_color / @error_color the fixed
 * fallback colours (#2ec27e / #e01b24) must apply to both widgets. */
static void
test_fallback_colours(void)
{
  /* The bare test theme paints windows #101010. */
  GtkWidget *probe = g_object_ref_sink(gtk_window_new());
  gtk_widget_realize(probe);
  GdkRGBA wc;
  gtk_widget_get_color(probe, &wc);
  g_object_unref(probe);
  if (!color_is(&wc, 0x10, 0x10, 0x10)) {
    g_test_skip("could not install the test theme");
    return;
  }

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *spark = stk_sparkline_new();
  GtkWidget *chart = stk_chart_new();
  gtk_box_append(GTK_BOX(box), spark);
  gtk_box_append(GTK_BOX(box), chart);
  Fixture f = fixture_new(box);

  g_autoptr(StkSeries) up = make_daily(STK_RANGE_1M, 21, 100, 1.0);
  g_autoptr(StkSeries) down = make_daily(STK_RANGE_1M, 21, 100, -1.0);
  GdkRGBA c;
  stk_chart_set_series(STK_CHART(chart), up);
  stk_sparkline_set_series(STK_SPARKLINE(spark), up);
  pump(30);
  gtk_widget_get_color(chart, &c);
  g_assert_true(color_is(&c, 0x2e, 0xc2, 0x7e));
  gtk_widget_get_color(spark, &c);
  g_assert_true(color_is(&c, 0x2e, 0xc2, 0x7e));
  stk_chart_set_series(STK_CHART(chart), down);
  stk_sparkline_set_series(STK_SPARKLINE(spark), down);
  pump(30);
  gtk_widget_get_color(chart, &c);
  g_assert_true(color_is(&c, 0xe0, 0x1b, 0x24));
  gtk_widget_get_color(spark, &c);
  g_assert_true(color_is(&c, 0xe0, 0x1b, 0x24));
  fixture_free(&f);
}

/* Glyph line of the hover label (the text node in the strip above the plot),
 * or NULL when no label is drawn. The serialized glyph line encodes the
 * characters of the label (digits as glyph ids, other text verbatim). */
static char *
tooltip_glyphs(GBytes *bytes)
{
  gsize len;
  const char *data = g_bytes_get_data(bytes, &len);
  if (len == 0)
    return NULL;
  g_auto(GStrv) lines = g_strsplit(data, "\n", -1);
  GString *out = g_string_new(NULL);
  gboolean after_crosshair = FALSE;
  for (guint i = 0; lines[i] != NULL; i++) {
    const char *l = g_strstrip(lines[i]);
    double bx, by, bw, bh;
    if (sscanf(l, "bounds: %lf %lf %lf %lf", &bx, &by, &bw, &bh) == 4 && bw == 1.0) {
      after_crosshair = TRUE; /* the 1 px wide crosshair is drawn right before the label */
    } else if (after_crosshair && g_str_has_prefix(l, "glyphs: ")) {
      g_string_append(out, l); /* the label may be split in several runs */
    }
  }
  return out->len > 0 ? g_string_free(out, FALSE) : (g_string_free(out, TRUE), NULL);
}

/* One bar per weekday 2026-09-08..2026-10-07 stamped like the provider does
 * (00:00:00 UTC of the trading date) plus @shift_ms; value = 100 + day. */
static StkSeries *
make_provider_daily(gint64 shift_ms)
{
  StkSeries *s = stk_series_new(STK_RANGE_1M, NAN, 32);
  for (int i = 0; i < 30; i++) {
    gint64 t = utc_ms(2026, 9, 8, 0, 0) + (gint64)i * 86400000;
    g_autoptr(GDateTime) dt = g_date_time_new_from_unix_utc(t / 1000);
    if (g_date_time_get_day_of_week(dt) >= 6)
      continue;
    g_assert_true(stk_series_append(s, t + shift_ms, 100.0 + g_date_time_get_day_of_month(dt)));
  }
  return s;
}

/* The hover label of a daily series shows the UTC date of the 00:00Z stamp:
 * it is the same for a stamp anywhere inside that UTC day (00:00Z, 12:00Z,
 * 23:00Z) and differs for the next day. (Converting 00:00Z to New York time
 * would label the previous day and break the first equality.) */
static void
test_chart_daily_dates(void)
{
  Fixture f = fixture_new(stk_chart_new());
  StkChart *chart = STK_CHART(f.widget);
  double xs[4] = { 60, 150, 230, 330 };
  for (int k = 0; k < 4; k++) {
    g_autoptr(StkSeries) a = make_provider_daily(0);
    g_autoptr(StkSeries) b = make_provider_daily(12 * 3600 * 1000);
    g_autoptr(StkSeries) c = make_provider_daily(23 * 3600 * 1000);
    g_autoptr(StkSeries) next = make_provider_daily(86400000);

    stk_chart_set_series(chart, a);
    motion(f.widget, xs[k]);
    GBytes *ba = snap(f.widget);
    g_autofree char *ta = tooltip_glyphs(ba);
    g_assert_nonnull(ta);

    stk_chart_set_series(chart, b);
    motion(f.widget, xs[k]);
    GBytes *bb = snap(f.widget);
    g_autofree char *tb = tooltip_glyphs(bb);
    stk_chart_set_series(chart, c);
    motion(f.widget, xs[k]);
    GBytes *bc = snap(f.widget);
    g_autofree char *tc = tooltip_glyphs(bc);
    stk_chart_set_series(chart, next);
    motion(f.widget, xs[k]);
    GBytes *bn = snap(f.widget);
    g_autofree char *tn = tooltip_glyphs(bn);

    g_assert_cmpstr(ta, ==, tb);
    g_assert_cmpstr(ta, ==, tc);
    g_assert_cmpstr(ta, !=, tn);
    g_bytes_unref(ba);
    g_bytes_unref(bb);
    g_bytes_unref(bc);
    g_bytes_unref(bn);
  }
  fixture_free(&f);
}

/* A font change re-lays the chart out without changing its size: the hover
 * crosshair/tooltip cached for the old layout must be dropped. */
static void
test_chart_style_change(void)
{
  Fixture f = fixture_new(stk_chart_new());
  StkChart *chart = STK_CHART(f.widget);
  g_autoptr(StkSeries) s = make_1d(13, 30, 151, 100, 0.02);
  stk_chart_set_series(chart, s);
  GBytes *base = snap(f.widget);
  motion(f.widget, 60);
  GBytes *hovered = snap(f.widget);
  g_autofree char *tip = tooltip_glyphs(hovered);
  g_assert_nonnull(tip);

  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, "stk-chart.big { font-size: 24px; }");
  gtk_style_context_add_provider_for_display(gtk_widget_get_display(f.widget),
                                             GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_USER);
  gtk_widget_add_css_class(f.widget, "big");
  pump(100);

  GBytes *after = snap(f.widget);
  g_autofree char *tip_after = tooltip_glyphs(after);
  g_assert_null(tip_after);         /* stale hover was dropped */
  assert_snaps_differ(base, after); /* labels use the bigger font */
  leave(f.widget);
  GBytes *after_leave = snap(f.widget);
  assert_snaps_equal(after, after_leave);

  /* hovering again works on the new layout, and the font can change back */
  motion(f.widget, 60);
  GBytes *again = snap(f.widget);
  g_autofree char *tip_again = tooltip_glyphs(again);
  g_assert_nonnull(tip_again);
  gtk_widget_remove_css_class(f.widget, "big");
  pump(100);
  GBytes *back = snap(f.widget);
  g_autofree char *tip_back = tooltip_glyphs(back);
  g_assert_null(tip_back);
  assert_snaps_equal(base, back);

  gtk_style_context_remove_provider_for_display(gtk_widget_get_display(f.widget),
                                                GTK_STYLE_PROVIDER(provider));
  g_object_unref(provider);
  g_bytes_unref(base);
  g_bytes_unref(hovered);
  g_bytes_unref(after);
  g_bytes_unref(after_leave);
  g_bytes_unref(again);
  g_bytes_unref(back);
  fixture_free(&f);
}

/* dispose() must release every Pango layout (each holds a reference on the
 * widget's Pango context) even while the widget itself is kept alive, e.g.
 * by a reference cycle. */
static void
test_chart_dispose(void)
{
  GtkWidget *chart = g_object_ref_sink(stk_chart_new());
  PangoContext *ctx = g_object_ref(gtk_widget_get_pango_context(chart));
  guint before = ((GObject *)ctx)->ref_count;
  g_assert_cmpuint(before, >=, 17); /* 5 y + 6 x + 4 other layouts + ours */
  g_object_run_dispose(G_OBJECT(chart));
  g_assert_cmpuint(((GObject *)ctx)->ref_count, <=, 2);
  g_object_run_dispose(G_OBJECT(chart)); /* idempotent */
  g_object_unref(ctx);
  g_object_unref(chart);

  GtkWidget *spark = g_object_ref_sink(stk_sparkline_new());
  g_autoptr(StkSeries) s = make_daily(STK_RANGE_1M, 21, 100, 1.0);
  stk_sparkline_set_series(STK_SPARKLINE(spark), s);
  g_object_run_dispose(G_OBJECT(spark));
  g_object_run_dispose(G_OBJECT(spark));
  g_object_unref(spark);
}

/* P2-005: methods called after g_object_run_dispose() must not touch the
 * cleared layouts/strokes. The chart is sized and dirty, so rebuild() would
 * dereference self->sample on the old code. */
static void
test_use_after_dispose(void)
{
  g_autoptr(StkSeries) s = make_1d(13, 30, 60, 100, 0.1);
  GtkWidget *chart = g_object_ref_sink(stk_chart_new());
  Fixture f = fixture_new(chart);
  stk_chart_set_series(STK_CHART(chart), s);
  pump(30);
  g_assert_cmpuint(snap_size(chart), >, 0);
  gtk_window_set_child(GTK_WINDOW(f.window), NULL); /* disposing a parented widget is a GTK error */
  g_object_run_dispose(G_OBJECT(chart));
  stk_chart_set_series(STK_CHART(chart), s);
  stk_chart_set_stale(STK_CHART(chart), TRUE);
  g_assert_cmpuint(snap_size(chart), ==, 0);
  gtk_widget_add_css_class(chart, "x"); /* css_changed -> style_changed */
  g_assert_cmpuint(snap_size(chart), ==, 0);
  g_object_run_dispose(G_OBJECT(chart));
  fixture_free(&f);
  g_object_unref(chart);

  /* disposed before it was ever sized/drawn */
  GtkWidget *c2 = g_object_ref_sink(stk_chart_new());
  g_object_run_dispose(G_OBJECT(c2));
  stk_chart_set_series(STK_CHART(c2), s);
  g_assert_cmpuint(snap_size(c2), ==, 0);
  g_object_unref(c2);

  GtkWidget *spark = g_object_ref_sink(stk_sparkline_new());
  stk_sparkline_set_series(STK_SPARKLINE(spark), s);
  g_object_run_dispose(G_OBJECT(spark));
  stk_sparkline_set_series(STK_SPARKLINE(spark), s);
  g_assert_cmpuint(snap_size(spark), ==, 0);
  stk_sparkline_set_series(STK_SPARKLINE(spark), NULL);
  g_object_unref(spark);
}

static StkSeries *
make_huge_series(void)
{
  const gsize n = 2000000;
  StkSeries *s = stk_series_new(STK_RANGE_1Y, NAN, n);
  gint64 t0 = utc_ms(2000, 1, 1, 0, 0);
  for (gsize i = 0; i < n; i++)
    g_assert_true(
        stk_series_append(s, t0 + (gint64)i * 3600000, 100.0 + 10.0 * sin((double)i / 997.0)));
  return s;
}

/* Series length and widget width must not translate into unbounded memory:
 * a 2-million-point series and a 20000 px wide chart are handled (and drawn)
 * with the fixed sample / column caps. */
static void
test_chart_huge_inputs(void)
{
  g_autoptr(StkSeries) big = make_huge_series();
  GtkWidget *scroller = gtk_scrolled_window_new();
  GtkWidget *chart = stk_chart_new();
  gtk_widget_set_size_request(chart, 20000, 200);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), chart);
  Fixture f = fixture_new(scroller);
  for (int i = 0; i < 100 && gtk_widget_get_width(chart) < 20000; i++)
    pump(10);
  g_assert_cmpint(gtk_widget_get_width(chart), ==, 20000);

  stk_chart_set_series(STK_CHART(chart), big);
  GBytes *base = snap(chart);
  g_assert_cmpuint(g_bytes_get_size(base), >, 0);
  motion(chart, 15000);
  GBytes *hovered = snap(chart);
  assert_snaps_differ(base, hovered);
  g_autofree char *tip = tooltip_glyphs(hovered);
  g_assert_nonnull(tip);

  g_bytes_unref(base);
  g_bytes_unref(hovered);
  fixture_free(&f);
}

static void
test_sparkline_huge(void)
{
  g_autoptr(StkSeries) big = make_huge_series();
  GtkWidget *spark = g_object_ref_sink(stk_sparkline_new());
  stk_sparkline_set_series(STK_SPARKLINE(spark), big);
  GBytes *bytes = snap(spark);
  g_assert_cmpuint(g_bytes_get_size(bytes), >, 0);
  g_bytes_unref(bytes);
  g_object_unref(spark);
}

/* g_test_init() makes every warning fatal, but the installed GTK theme itself
 * may log parser warnings for its own stylesheet while it loads; those are not
 * ours and are reported without failing. Any other warning (this writer) or
 * critical (fatal mask) still aborts the test. */
static GLogWriterOutput
log_writer(GLogLevelFlags level, const GLogField *fields, gsize n_fields, gpointer user_data)
{
  if (level & G_LOG_LEVEL_WARNING) {
    for (gsize i = 0; i < n_fields; i++) {
      if (strcmp(fields[i].key, "MESSAGE") == 0 && fields[i].value != NULL &&
          g_str_has_prefix(fields[i].value, "Theme parser error: gtk.css")) {
        g_printerr("# ignored (GTK theme): %s\n", (const char *)fields[i].value);
        return G_LOG_WRITER_HANDLED;
      }
    }
    g_log_writer_default(level, fields, n_fields, user_data);
    abort();
  }
  return g_log_writer_default(level, fields, n_fields, user_data);
}

/* A theme that defines neither @success_color nor @error_color, so the tests
 * (and the fallback-colour check) are independent of the installed theme.
 * Must run before GTK reads its environment. Returns the temp dir to remove. */
static char *
install_bare_theme(void)
{
  char *root = g_dir_make_tmp("stk-chart-test-XXXXXX", NULL);
  if (root == NULL)
    return NULL;
  char *dir = g_build_filename(root, "themes", "Bare", "gtk-4.0", NULL);
  char *css = g_build_filename(dir, "gtk.css", NULL);
  char *cfg = g_build_filename(root, "config", NULL);
  if (g_mkdir_with_parents(dir, 0700) == 0 && g_mkdir_with_parents(cfg, 0700) == 0 &&
      g_file_set_contents(css, "window { color: #101010; background-color: #ffffff; }\n", -1,
                          NULL)) {
    g_setenv("XDG_DATA_HOME", root, TRUE);
    g_setenv("XDG_CONFIG_HOME", cfg, TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    g_setenv("GTK_THEME", "Bare", TRUE);
  }
  g_free(dir);
  g_free(css);
  g_free(cfg);
  return root;
}

static void
remove_tree(const char *root)
{
  if (root == NULL)
    return;
  char *dir = g_build_filename(root, "themes", "Bare", "gtk-4.0", NULL);
  char *css = g_build_filename(dir, "gtk.css", NULL);
  char *cfg = g_build_filename(root, "config", NULL);
  char *bare = g_build_filename(root, "themes", "Bare", NULL);
  char *themes = g_build_filename(root, "themes", NULL);
  g_remove(css);
  g_rmdir(dir);
  g_rmdir(bare);
  g_rmdir(themes);
  g_rmdir(cfg);
  g_rmdir(root);
  g_free(dir);
  g_free(css);
  g_free(cfg);
  g_free(bare);
  g_free(themes);
}

/* Direction is checked synchronously after set_series: no pump, no snapshot. */
static StkSeries *
make_values(StkRange range, double prev_close, const double *v, int n)
{
  StkSeries *s = stk_series_new(range, prev_close, (gsize)n);
  gint64 t0 = range == STK_RANGE_1D ? utc_ms(2026, 10, 8, 13, 30) : utc_ms(2026, 9, 1, 0, 0);
  gint64 step = range == STK_RANGE_1D ? 60000 : 86400000;
  for (int i = 0; i < n; i++)
    g_assert_true(stk_series_append(s, t0 + i * step, v[i]));
  return s;
}

static void
test_chart_direction_vs_prev_close(void)
{
  double fade[3] = { 105.0, 99.0, 102.0 };   /* first > last >= prev 100 */
  double recover[3] = { 95.0, 101.0, 98.0 }; /* first < last < prev 100 */
  Fixture f = fixture_new(stk_chart_new());
  StkChart *c = STK_CHART(f.widget);

  StkSeries *s = make_values(STK_RANGE_1D, 100.0, fade, 3);
  stk_chart_set_series(c, s);
  g_assert_true(gtk_widget_has_css_class(f.widget, "positive"));
  g_assert_false(gtk_widget_has_css_class(f.widget, "negative"));
  stk_series_unref(s);

  s = make_values(STK_RANGE_1D, 100.0, recover, 3);
  stk_chart_set_series(c, s);
  g_assert_true(gtk_widget_has_css_class(f.widget, "negative"));
  g_assert_false(gtk_widget_has_css_class(f.widget, "positive"));
  stk_series_unref(s);

  /* Same values as 1M: last-vs-first. */
  s = make_values(STK_RANGE_1M, 100.0, fade, 3);
  stk_chart_set_series(c, s);
  g_assert_true(gtk_widget_has_css_class(f.widget, "negative"));
  g_assert_false(gtk_widget_has_css_class(f.widget, "positive"));
  stk_series_unref(s);

  s = make_values(STK_RANGE_1M, 100.0, recover, 3);
  stk_chart_set_series(c, s);
  g_assert_true(gtk_widget_has_css_class(f.widget, "positive"));
  g_assert_false(gtk_widget_has_css_class(f.widget, "negative"));
  stk_series_unref(s);

  fixture_free(&f);
}

int
main(int argc, char **argv)
{
  char *tmp_root = install_bare_theme();
  g_log_set_writer_func(log_writer, NULL, NULL);
  g_test_init(&argc, &argv, NULL);
  g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  /* Pin the X11 backend (Xvfb) so the test never touches a live compositor. */
  g_setenv("GDK_BACKEND", "x11", TRUE);
  gdk_set_allowed_backends("x11");
  if (!gtk_init_check()) {
    g_print("1..0 # SKIP no display\n");
    remove_tree(tmp_root);
    g_free(tmp_root);
    return 77;
  }
  g_assert_true(g_str_has_prefix(G_OBJECT_TYPE_NAME(gdk_display_get_default()), "GdkX11"));

  g_test_add_func("/chart-widget/chart/measure", test_chart_measure);
  g_test_add_func("/chart-widget/chart/no-data", test_chart_no_data);
  g_test_add_func("/chart-widget/chart/data-and-direction", test_chart_data_and_direction);
  g_test_add_func("/chart-widget/chart/fallback-colours", test_fallback_colours);
  g_test_add_func("/chart-widget/chart/stale", test_chart_stale);
  g_test_add_func("/chart-widget/chart/hover", test_chart_hover);
  g_test_add_func("/chart-widget/chart/daily-dates", test_chart_daily_dates);
  g_test_add_func("/chart-widget/chart/style-change", test_chart_style_change);
  g_test_add_func("/chart-widget/chart/dispose", test_chart_dispose);
  g_test_add_func("/chart-widget/use-after-dispose", test_use_after_dispose);
  g_test_add_func("/chart-widget/chart/huge-inputs", test_chart_huge_inputs);
  g_test_add_func("/chart-widget/chart/resize", test_chart_resize);
  g_test_add_func("/chart-widget/chart/idle-and-redraw", test_chart_idle_and_redraw);
  g_test_add_func("/chart-widget/sparkline", test_sparkline);
  g_test_add_func("/chart-widget/sparkline/huge-and-dispose", test_sparkline_huge);
  g_test_add_func("/chart-widget/chart/direction-vs-prev-close", test_chart_direction_vs_prev_close);
  g_test_add_func("/chart-widget/in-box", test_in_box);
  int rc = g_test_run();
  remove_tree(tmp_root);
  g_free(tmp_root);
  return rc;
}
