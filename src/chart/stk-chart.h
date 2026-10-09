/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-chart.h - StkChart: price chart widget drawn with GtkSnapshot.
 *
 * Shows one StkSeries as a line with a gradient fill, horizontal grid lines
 * with price labels on the right (at most 5, 1-2-5 spacing), time labels
 * below (at most 6) and, while the pointer hovers or a finger scrubs, a
 * crosshair with a "price . date/time" label in the strip above the plot.
 * Pure GTK4 drawing (no cairo). The widget adds no tick callbacks, animations
 * or timers of its own (the only timer involved is GTK's long-press gesture
 * timeout while a finger is down) and queues a redraw only when the series,
 * the stale flag, the size or the hovered sample changes.
 *
 * Interaction: the pointer position over the plot selects the nearest sample;
 * no crosshair is shown beyond the last sample or outside the plot. On touch
 * screens a long press starts scrubbing, dragging then moves the crosshair and
 * lifting the finger clears it (plain touch drags are left to ancestors such
 * as a scrolled window).
 *
 * Axes: for STK_RANGE_1D the x axis spans the full regular session
 * (09:30-16:00 America/New_York, or the early close) of the New York date of
 * the series' last sample, so a partial day only fills the left part; samples
 * outside the session (pre/after market) are not drawn. For 1D, a finite
 * prev_close is drawn as a dashed line and the y axis includes it (other
 * ranges ignore prev_close). All other ranges span first..last sample.
 *
 * Colour: the widget carries the CSS node name "stk-chart" and the CSS class
 * "positive" when the chart is up, "negative" otherwise. For STK_RANGE_1D
 * with a finite prev_close the chart is up when the last drawn sample is >=
 * the previous close, which matches the change shown in the detail header and
 * list row; otherwise it is up when the last drawn sample is >= the first. The
 * line/fill/crosshair colour is the CSS `color` of the widget,
 * so applications theme it with e.g.
 *     stk-chart.positive { color: #26a269; }
 *     stk-chart.negative { color: #c01c28; }
 * A fallback style (priority GTK_STYLE_PROVIDER_PRIORITY_FALLBACK, installed
 * once, for the display that is the default when the first chart is created)
 * `@define-color`s success_color (#2ec27e) and error_color (#e01b24) and
 * colours the classes with them; because it has the lowest priority, a
 * theme's own @success_color / @error_color (light and dark variants) take
 * precedence, and a theme that defines neither still gets green and red.
 * Axis labels, grid and the hover label use the CSS `color` of the widget's
 * parent (the colour the chart would inherit without the class), falling
 * back to its own colour when it has no parent.
 *
 * Time stamps: for STK_RANGE_1D, t_ms are true UTC instants shown in
 * America/New_York time; for 1M/3M/6M/1Y, t_ms is 00:00:00 UTC of the
 * trading date and labels show that UTC date (see stk-chart-geom.h).
 *
 * Bounded work: whatever the series length, the chart keeps at most
 * STK_CHART_GEOM_MAX_SAMPLES (4096) samples (first, last and the min/max of
 * equal index buckets when the series is longer; hover then picks among the
 * kept samples) and decimates onto at most STK_CHART_GEOM_MAX_COLUMNS (8192)
 * pixel columns however wide the widget is.
 *
 * Lifecycle: all GObject/Pango/GSK references are dropped in dispose(); after
 * g_object_run_dispose() every method (stk_chart_set_series(), snapshot,
 * hover) is a harmless no-op and nothing is drawn;
 * fonts, scale and theme changes re-validate the cached label layouts and
 * clear the hover.
 *
 * Main-thread only, like every GtkWidget.
 */
#ifndef STK_CHART_H
#define STK_CHART_H

#include <gtk/gtk.h>

#include "stk-types.h"

G_BEGIN_DECLS

#define STK_TYPE_CHART (stk_chart_get_type())
G_DECLARE_FINAL_TYPE(StkChart, stk_chart, STK, CHART, GtkWidget)

/**
 * stk_chart_new:
 *
 * Returns: (transfer floating): a new, empty chart (shows "No data"). It
 *   expands in both directions and requests at least 120x80 px (natural
 *   320x200).
 */
GtkWidget *stk_chart_new(void);

/**
 * stk_chart_set_series:
 * @self: a StkChart.
 * @series: (nullable) (transfer none): the series to show, or NULL / an empty
 *   series for "No data".
 *
 * The chart copies the samples, range and prev_close it needs and keeps no
 * reference to @series, so later appends or unrefs do not affect it; call
 * again with the updated series. Replaces any previous data, clears the hover
 * state and updates the "positive"/"negative" CSS class.
 *
 * A series whose samples all lie outside the displayed domain (1D data that
 * is entirely pre-/after-market) shows "No data".
 */
void stk_chart_set_series(StkChart *self, StkSeries *series);

/**
 * stk_chart_set_stale:
 * @self: a StkChart.
 * @stale: whether the shown data is out of date (e.g. cached after a failed
 *   refresh).
 *
 * Shows (TRUE) or hides (FALSE) a "Stale" marker in the top-right of the
 * strip above the plot, also when there is no data. Changing the flag clears
 * the hover state and redraws; setting the current value does nothing.
 */
void stk_chart_set_stale(StkChart *self, gboolean stale);

G_END_DECLS

#endif /* STK_CHART_H */
