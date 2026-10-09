/*
 * stk-sparkline.h - StkSparkline: tiny fixed-size trend line for list rows.
 *
 * A line only (no axes, labels, fill or interaction), drawn with GtkSnapshot.
 * It requests exactly STK_SPARKLINE_WIDTH x STK_SPARKLINE_HEIGHT and is not
 * expandable (it is centred if given more room). The x axis follows the same
 * rule as StkChart (1D: the full regular session; otherwise first..last
 * sample); the y axis spans the drawn samples with a small padding. Like
 * StkChart it uses the CSS node name "stk-sparkline" and the CSS classes
 * "positive" (last >= first) / "negative", the CSS `color` being the line
 * colour; the same kind of fallback style applies (see stk-chart.h). No
 * timers or tick callbacks; it redraws only when the series changes (the
 * path is computed once per series, for the fixed size). Memory is bounded
 * independently of the series length: set_series() reads the series in
 * place and keeps at most 4 * 68 samples in on-stack arrays (see
 * stk_chart_geom_sample()). Daily-range timestamps are 00:00 UTC of the
 * trading date (cross-unit convention, stk-chart-geom.h).
 *
 * Main-thread only.
 */
#ifndef STK_SPARKLINE_H
#define STK_SPARKLINE_H

#include <gtk/gtk.h>

#include "stk-types.h"

G_BEGIN_DECLS

#define STK_SPARKLINE_WIDTH 72
#define STK_SPARKLINE_HEIGHT 24

#define STK_TYPE_SPARKLINE (stk_sparkline_get_type())
G_DECLARE_FINAL_TYPE(StkSparkline, stk_sparkline, STK, SPARKLINE, GtkWidget)

/**
 * stk_sparkline_new:
 *
 * Returns: (transfer floating): a new sparkline that draws nothing until a
 *   series with samples is set.
 */
GtkWidget *stk_sparkline_new(void);

/**
 * stk_sparkline_set_series:
 * @self: a StkSparkline.
 * @series: (nullable) (transfer none): series to draw, or NULL / empty to
 *   draw nothing.
 *
 * Copies what it needs and keeps no reference to @series (see
 * stk_chart_set_series(); later appends need another call; 1D samples outside
 * the session are not drawn). Updates the "positive"/"negative" CSS class.
 */
void stk_sparkline_set_series(StkSparkline *self, StkSeries *series);

G_END_DECLS

#endif /* STK_SPARKLINE_H */
