/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-nasdaq.c - StkProvider implementation for the Nasdaq web API.
 * See stk-nasdaq.h for the behaviour contract.
 *
 * Layout: URL builders, JSON helpers, response parsers (pure), then the
 * request layer (one GTask per operation, one libsoup message per attempt)
 * shared by quote / series / search, then the StkProvider glue.
 */
#include "stk-nasdaq.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <math.h>
#include <string.h>

#include "stk-market.h"
#include "stk-parse.h"

#define STK_NASDAQ_USER_AGENT "stocks-app/0.1 (personal use)"

#define MS_PER_DAY G_GINT64_CONSTANT(86400000)
/* 09:30 as milliseconds since local midnight. */
#define SESSION_OPEN_WALL_MS G_GINT64_CONSTANT(34200000)
/* Unix epoch (1970-01-01) as a GDate Julian day (day 1 = 0001-01-01). */
#define UNIX_EPOCH_JULIAN G_GINT64_CONSTANT(719163)
/* 2022-01-01T00:00Z and 2101-01-01T00:00Z: accepted timestamp window, equal to
 * the years the core calendar supports. */
#define EPOCH_MIN_MS G_GINT64_CONSTANT(1640995200000)
#define EPOCH_MAX_MS G_GINT64_CONSTANT(4133980800000)

#define SEARCH_TEXT_MAX_BYTES 64
#define READ_CHUNK_BYTES (16 * 1024)
/* libsoup has no constant for 429. */
#define HTTP_TOO_MANY_REQUESTS 429

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static gboolean
kind_is_valid(StkKind kind)
{
  return kind == STK_KIND_STOCK || kind == STK_KIND_ETF || kind == STK_KIND_FUND;
}

static gboolean
range_is_valid(StkRange range)
{
  return range >= STK_RANGE_1D && range <= STK_RANGE_1Y;
}

static const char *
assetclass_of(StkKind kind)
{
  switch (kind) {
  case STK_KIND_STOCK:
    return "stocks";
  case STK_KIND_ETF:
    return "etf";
  case STK_KIND_FUND:
    return "mutualfunds";
  default:
    g_assert_not_reached();
  }
}

/* Calendar months covered by a daily range. */
static guint
months_of(StkRange range)
{
  switch (range) {
  case STK_RANGE_1M:
    return 1;
  case STK_RANGE_3M:
    return 3;
  case STK_RANGE_6M:
    return 6;
  case STK_RANGE_1Y:
    return 12;
  default:
    g_assert_not_reached();
  }
}

/* "limit" for /historical: above the number of trading days in the window. */
static guint
historical_limit_of(StkRange range)
{
  switch (range) {
  case STK_RANGE_1M:
    return 40;
  case STK_RANGE_3M:
    return 100;
  case STK_RANGE_6M:
    return 200;
  case STK_RANGE_1Y:
    return 400;
  default:
    g_assert_not_reached();
  }
}

static GTimeZone *
new_york_tz(void)
{
  static gsize tz_once = 0;
  if (g_once_init_enter(&tz_once)) {
    GTimeZone *tz = g_time_zone_new_identifier("America/New_York");
    if (tz == NULL)
      tz = g_time_zone_new_identifier("EST5EDT,M3.2.0,M11.1.0");
    g_assert(tz != NULL);
    g_once_init_leave(&tz_once, (gsize)tz);
  }
  return (GTimeZone *)tz_once;
}

/* ------------------------------------------------------------------------ */
/* URL builders                                                             */
/* ------------------------------------------------------------------------ */

static gboolean
check_symbol_kind(const char *symbol, StkKind kind, GError **error)
{
  if (!stk_symbol_is_valid(symbol)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG, "invalid symbol");
    return FALSE;
  }
  if (!kind_is_valid(kind)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG, "invalid instrument kind");
    return FALSE;
  }
  return TRUE;
}

/* Upper-cases and percent-encodes an already validated symbol. */
static char *
encode_symbol(const char *symbol)
{
  g_autofree char *upper = g_ascii_strup(symbol, -1);
  return g_uri_escape_string(upper, NULL, FALSE);
}

char *
stk_nasdaq_quote_url(const char *base_url, const char *symbol, StkKind kind, GError **error)
{
  g_return_val_if_fail(base_url != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (!check_symbol_kind(symbol, kind, error))
    return NULL;
  g_autofree char *sym = encode_symbol(symbol);
  return g_strdup_printf("%s/api/quote/%s/info?assetclass=%s", base_url, sym,
                         assetclass_of(kind));
}

static char *
date_to_iso(const GDate *date)
{
  return g_strdup_printf("%04u-%02u-%02u", (guint)g_date_get_year(date),
                         (guint)g_date_get_month(date), (guint)g_date_get_day(date));
}

char *
stk_nasdaq_series_url(const char *base_url, const char *symbol, StkKind kind, StkRange range,
                      const GDate *today, GError **error)
{
  g_return_val_if_fail(base_url != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (!check_symbol_kind(symbol, kind, error))
    return NULL;
  if (!range_is_valid(range)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG, "invalid chart range");
    return NULL;
  }
  if (range == STK_RANGE_1D && kind == STK_KIND_FUND) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG,
                        "mutual funds have no intraday (1D) data");
    return NULL;
  }

  g_autofree char *sym = encode_symbol(symbol);
  const char *asset = assetclass_of(kind);
  if (range == STK_RANGE_1D)
    return g_strdup_printf("%s/api/quote/%s/chart?assetclass=%s", base_url, sym, asset);

  if (today == NULL || !g_date_valid(today) || g_date_get_year(today) < STK_MARKET_MIN_YEAR ||
      g_date_get_year(today) > STK_MARKET_MAX_YEAR) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG,
                        "date outside the supported calendar");
    return NULL;
  }
  GDate from = *today;
  g_date_subtract_months(&from, months_of(range));
  g_autofree char *from_s = date_to_iso(&from);
  g_autofree char *to_s = date_to_iso(today);
  if (kind == STK_KIND_FUND)
    return g_strdup_printf(
        "%s/api/quote/%s/historical?assetclass=mutualfunds&fromdate=%s&todate=%s&limit=%u",
        base_url, sym, from_s, to_s, historical_limit_of(range));
  return g_strdup_printf("%s/api/quote/%s/chart?assetclass=%s&charttype=real&fromdate=%s&todate=%s",
                         base_url, sym, asset, from_s, to_s);
}

char *
stk_nasdaq_search_url(const char *base_url, const char *text, GError **error)
{
  g_return_val_if_fail(base_url != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (text == NULL) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG, "empty search text");
    return NULL;
  }
  while (*text == ' ' || (*text >= '\t' && *text <= '\r'))
    text++;
  gsize len = strlen(text);
  while (len > 0 && (text[len - 1] == ' ' || (text[len - 1] >= '\t' && text[len - 1] <= '\r')))
    len--;
  if (len == 0 || len > SEARCH_TEXT_MAX_BYTES || !g_utf8_validate(text, (gssize)len, NULL)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG,
                        "search text must be 1-64 bytes of valid UTF-8");
    return NULL;
  }
  for (const char *p = text; p < text + len; p = g_utf8_next_char(p)) {
    if (g_unichar_iscntrl(g_utf8_get_char(p))) {
      g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG,
                          "search text contains control characters");
      return NULL;
    }
  }
  g_autofree char *trimmed = g_strndup(text, len);
  g_autofree char *escaped = g_uri_escape_string(trimmed, NULL, FALSE);
  return g_strdup_printf("%s/api/autocomplete/slookup/%d?search=%s", base_url,
                         STK_NASDAQ_MAX_SEARCH_HITS, escaped);
}

/* ------------------------------------------------------------------------ */
/* JSON helpers (every accessor tolerates missing / null / mistyped nodes)  */
/* ------------------------------------------------------------------------ */

static JsonObject *
member_object(JsonObject *obj, const char *name)
{
  JsonNode *node = json_object_get_member(obj, name);
  return (node != NULL && JSON_NODE_HOLDS_OBJECT(node)) ? json_node_get_object(node) : NULL;
}

static const char *
member_string(JsonObject *obj, const char *name)
{
  JsonNode *node = json_object_get_member(obj, name);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string(node);
}

static gboolean
member_bool(JsonObject *obj, const char *name)
{
  JsonNode *node = json_object_get_member(obj, name);
  return node != NULL && JSON_NODE_HOLDS_VALUE(node) &&
         json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}

static gboolean
node_number(JsonNode *node, double *out)
{
  if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
    return FALSE;
  GType type = json_node_get_value_type(node);
  if (type != G_TYPE_INT64 && type != G_TYPE_DOUBLE)
    return FALSE;
  double d = json_node_get_double(node);
  if (!isfinite(d))
    return FALSE;
  *out = d;
  return TRUE;
}

static gboolean
member_number(JsonObject *obj, const char *name, double *out)
{
  return node_number(json_object_get_member(obj, name), out);
}

/* Copy of the first @max_chars characters of the valid UTF-8 @text (all of it
 * if shorter), so a server message cannot bloat an error message or split a
 * UTF-8 sequence. The result is always NUL-terminated. */
static char *
truncated_copy(const char *text, glong max_chars)
{
  const char *end = text;
  for (glong i = 0; i < max_chars && *end != '\0'; i++)
    end = g_utf8_next_char(end);
  return g_strndup(text, (gsize)(end - text));
}

/*
 * Validates @data as a UTF-8 JSON envelope {"data":..., "status":{"rCode":N}}
 * and returns the parser (owner of *root). rCode != 200 is NOT_FOUND.
 */
static JsonParser *
parse_envelope(const guint8 *data, gsize len, JsonObject **root, GError **error)
{
  if (len == 0 || data == NULL) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "empty response");
    return NULL;
  }
  if (len > STK_NASDAQ_MAX_BODY_BYTES) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "response too large");
    return NULL;
  }
  const char *text = (const char *)data;
  if (!g_utf8_validate(text, (gssize)len, NULL)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "response is not valid UTF-8");
    return NULL;
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) json_error = NULL;
  if (!json_parser_load_from_data(parser, text, (gssize)len, &json_error)) {
    g_set_error(error, STK_ERROR, STK_ERROR_PARSE, "malformed JSON: %s", json_error->message);
    return NULL;
  }
  JsonNode *root_node = json_parser_get_root(parser);
  if (root_node == NULL || !JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "response is not a JSON object");
    return NULL;
  }
  JsonObject *obj = json_node_get_object(root_node);

  JsonObject *status = member_object(obj, "status");
  double rcode;
  if (status == NULL || !member_number(status, "rCode", &rcode)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "response has no status.rCode");
    return NULL;
  }
  if (rcode != 200.0) {
    const char *msg = NULL;
    JsonNode *codes = json_object_get_member(status, "bCodeMessage");
    if (codes != NULL && JSON_NODE_HOLDS_ARRAY(codes)) {
      JsonArray *arr = json_node_get_array(codes);
      JsonNode *first = json_array_get_length(arr) > 0 ? json_array_get_element(arr, 0) : NULL;
      if (first != NULL && JSON_NODE_HOLDS_OBJECT(first))
        msg = member_string(json_node_get_object(first), "errorMessage");
    }
    g_autofree char *short_msg = msg != NULL ? truncated_copy(msg, 80) : NULL;
    /* rCode is untrusted: print it as a double, never cast it to an integer. */
    g_set_error(error, STK_ERROR, STK_ERROR_NOT_FOUND, "provider status %.0f%s%s", rcode,
                short_msg != NULL ? ": " : "", short_msg != NULL ? short_msg : "");
    return NULL;
  }

  *root = obj;
  return g_steal_pointer(&parser);
}

/* Returns the envelope's "data" object or sets PARSE. */
static JsonObject *
envelope_data_object(JsonObject *root, GError **error)
{
  JsonObject *obj = member_object(root, "data");
  if (obj == NULL)
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "response has no data object");
  return obj;
}

/* ------------------------------------------------------------------------ */
/* Number / date parsing                                                    */
/* ------------------------------------------------------------------------ */

static gboolean
scan_uint(const char **p, guint min_digits, guint max_digits, guint *out)
{
  guint n = 0, value = 0;
  while (n < max_digits && g_ascii_isdigit(**p)) {
    value = value * 10 + (guint)(**p - '0');
    (*p)++;
    n++;
  }
  if (n < min_digits)
    return FALSE;
  *out = value;
  return TRUE;
}

static gboolean
scan_char(const char **p, char c)
{
  if (**p != c)
    return FALSE;
  (*p)++;
  return TRUE;
}

/*
 * Parses the timestamp strings of the /info endpoint:
 *   "Oct 8, 2026 7:25 PM ET", "Closed at Oct 8, 2026 4:00 PM ET", "Oct 8, 2026".
 * Times are America/New_York; a bare date is mapped to 12:00 that day.
 */
static gboolean
parse_quote_timestamp(const char *text, gint64 *out_ms)
{
  static const char months[12][4] = {"jan", "feb", "mar", "apr", "may", "jun",
                                     "jul", "aug", "sep", "oct", "nov", "dec"};
  if (text == NULL)
    return FALSE;
  const char *p = text;
  while (*p == ' ')
    p++;
  if (g_ascii_strncasecmp(p, "Closed at ", 10) == 0)
    p += 10;

  guint month = 0;
  for (guint i = 0; i < 12; i++) {
    if (g_ascii_strncasecmp(p, months[i], 3) == 0) {
      month = i + 1;
      break;
    }
  }
  if (month == 0)
    return FALSE;
  p += 3;

  guint day, year, hour = 12, minute = 0;
  if (!scan_char(&p, ' ') || !scan_uint(&p, 1, 2, &day) || !scan_char(&p, ',') ||
      !scan_char(&p, ' ') || !scan_uint(&p, 4, 4, &year))
    return FALSE;

  if (*p != '\0') {
    guint h12;
    if (!scan_char(&p, ' ') || !scan_uint(&p, 1, 2, &h12) || !scan_char(&p, ':') ||
        !scan_uint(&p, 2, 2, &minute) || !scan_char(&p, ' '))
      return FALSE;
    if (h12 < 1 || h12 > 12 || minute > 59)
      return FALSE;
    gboolean pm;
    if (g_ascii_strncasecmp(p, "PM", 2) == 0)
      pm = TRUE;
    else if (g_ascii_strncasecmp(p, "AM", 2) == 0)
      pm = FALSE;
    else
      return FALSE;
    p += 2;
    hour = (h12 % 12) + (pm ? 12u : 0u);
    if (g_ascii_strncasecmp(p, " ET", 3) != 0)
      return FALSE;
    p += 3;
  }
  while (*p == ' ')
    p++;
  if (*p != '\0' || !g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year))
    return FALSE;

  GDateTime *dt = g_date_time_new(new_york_tz(), (gint)year, (gint)month, (gint)day, (gint)hour,
                                  (gint)minute, 0);
  if (dt == NULL)
    return FALSE;
  *out_ms = g_date_time_to_unix(dt) * 1000;
  g_date_time_unref(dt);
  return TRUE;
}

/* Parses "MM/DD/YYYY" into 00:00 UTC of that date, epoch ms. */
static gboolean
parse_row_date(const char *text, gint64 *out_ms)
{
  if (text == NULL)
    return FALSE;
  const char *p = text;
  guint month, day, year;
  if (!scan_uint(&p, 2, 2, &month) || !scan_char(&p, '/') || !scan_uint(&p, 2, 2, &day) ||
      !scan_char(&p, '/') || !scan_uint(&p, 4, 4, &year) || *p != '\0')
    return FALSE;
  if (!g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year))
    return FALSE;
  GDate date;
  g_date_clear(&date, 1);
  g_date_set_dmy(&date, (GDateDay)day, (GDateMonth)month, (GDateYear)year);
  gint64 ms = ((gint64)g_date_get_julian(&date) - UNIX_EPOCH_JULIAN) * MS_PER_DAY;
  if (ms < EPOCH_MIN_MS || ms >= EPOCH_MAX_MS)
    return FALSE;
  *out_ms = ms;
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Parsers                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Implementation of stk_nasdaq_parse_quote(). If @regular_block is not NULL
 * it is set to whether the price was taken from the regular-session block
 * (primaryData while open or after the nightly roll, else secondaryData)
 * rather than from the fallback block.
 */
static StkQuote *
parse_quote_impl(const guint8 *data, gsize len, const char *symbol, StkKind kind,
                 gboolean *regular_block, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (!check_symbol_kind(symbol, kind, error))
    return NULL;
  JsonObject *root = NULL;
  g_autoptr(JsonParser) parser = parse_envelope(data, len, &root, error);
  if (parser == NULL)
    return NULL;
  JsonObject *info = envelope_data_object(root, error);
  if (info == NULL)
    return NULL;

  const char *status = member_string(info, "marketStatus");
  gboolean market_open = status != NULL && g_ascii_strcasecmp(status, "Open") == 0;

  JsonObject *primary = member_object(info, "primaryData");
  JsonObject *secondary = member_object(info, "secondaryData");
  if (primary == NULL && secondary == NULL) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "quote has no price data");
    return NULL;
  }
  /* Regular-session block first: live data while open; otherwise the official
   * close in secondaryData. After the nightly roll ("Closed") the provider
   * nulls secondaryData and puts the official close in primaryData. A Closed
   * response that still carries a secondaryData keeps using it as the close. */
  gboolean closed = status != NULL && g_ascii_strcasecmp(status, "Closed") == 0;
  gboolean primary_regular = market_open || (closed && secondary == NULL);
  JsonObject *candidates[2] = {primary_regular ? primary : secondary,
                               primary_regular ? secondary : primary};

  JsonObject *block = NULL;
  double price = NAN;
  gboolean saw_na = FALSE;
  for (guint i = 0; i < 2 && block == NULL; i++) {
    if (candidates[i] == NULL)
      continue;
    double p;
    StkParseResult r = stk_parse_price(member_string(candidates[i], "lastSalePrice"), &p);
    if (r == STK_PARSE_OK) {
      block = candidates[i];
      price = p;
    } else if (r == STK_PARSE_NA) {
      saw_na = TRUE;
    }
  }
  if (block == NULL && !saw_na) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "quote price is malformed");
    return NULL;
  }
  if (regular_block != NULL)
    *regular_block = block != NULL && block == candidates[0];

  const char *currency = block != NULL ? member_string(block, "currency") : NULL;
  if (currency != NULL &&
      !(strlen(currency) == 3 && g_ascii_isupper(currency[0]) && g_ascii_isupper(currency[1]) &&
        g_ascii_isupper(currency[2])))
    currency = NULL;

  StkQuote *quote = stk_quote_new(symbol, member_string(info, "companyName"), kind, currency);
  quote->price = price;
  quote->market_open = market_open;
  if (block != NULL) {
    double v;
    if (stk_parse_change(member_string(block, "netChange"), &v) == STK_PARSE_OK)
      quote->change = v;
    if (stk_parse_percent(member_string(block, "percentageChange"), &v) == STK_PARSE_OK)
      quote->change_pct = v;
    gint64 ms;
    if (parse_quote_timestamp(member_string(block, "lastTradeTimestamp"), &ms))
      quote->as_of_ms = ms;
    quote->is_realtime = member_bool(block, "isRealTime");
  }
  return quote;
}

StkQuote *
stk_nasdaq_parse_quote(const guint8 *data, gsize len, const char *symbol, StkKind kind,
                       GError **error)
{
  return parse_quote_impl(data, len, symbol, kind, NULL, error);
}

/* Returns data.chart as an array; *empty is set for an explicit JSON null. */
static JsonArray *
chart_array(JsonObject *info, gboolean *empty, GError **error)
{
  JsonNode *node = json_object_get_member(info, "chart");
  *empty = FALSE;
  if (node != NULL && JSON_NODE_HOLDS_ARRAY(node))
    return json_node_get_array(node);
  if (node != NULL && JSON_NODE_HOLDS_NULL(node)) {
    *empty = TRUE;
    return NULL;
  }
  g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "response has no chart array");
  return NULL;
}

StkSeries *
stk_nasdaq_parse_chart(const guint8 *data, gsize len, StkRange range, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (!range_is_valid(range)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG, "invalid chart range");
    return NULL;
  }
  JsonObject *root = NULL;
  g_autoptr(JsonParser) parser = parse_envelope(data, len, &root, error);
  if (parser == NULL)
    return NULL;
  JsonObject *info = envelope_data_object(root, error);
  if (info == NULL)
    return NULL;
  gboolean empty;
  JsonArray *points = chart_array(info, &empty, error);
  if (points == NULL && !empty)
    return NULL;

  guint n = points != NULL ? json_array_get_length(points) : 0;
  StkSeries *series = stk_series_new(range, NAN, n);
  /* prev_close stays NAN: data.previousClose lags after hours (see
   * stk-nasdaq.h); the provider glue derives it from the quote. */

  /* 1D: per-date session window, recomputed only when the date changes. */
  gint64 cached_day = -1, session_start = 0, session_end = 0, offset = 0;
  gboolean has_session = FALSE;

  for (guint i = 0; i < n; i++) {
    JsonNode *element = json_array_get_element(points, i);
    double x, y;
    if (!JSON_NODE_HOLDS_OBJECT(element))
      continue;
    JsonObject *point = json_node_get_object(element);
    if (!member_number(point, "x", &x) || !member_number(point, "y", &y) || y < 0.0 ||
        x < (double)EPOCH_MIN_MS || x >= (double)EPOCH_MAX_MS)
      continue;
    gint64 t_ms = (gint64)x;

    if (range == STK_RANGE_1D) {
      /* x is New York wall-clock time read as UTC: its UTC date is the New
       * York trading date, and (true instant - x) is that date's UTC offset,
       * constant across a weekday. */
      gint64 day = t_ms / MS_PER_DAY;
      if (day != cached_day) {
        GDate date;
        g_date_clear(&date, 1);
        g_date_set_julian(&date, (guint32)(day + UNIX_EPOCH_JULIAN));
        has_session = stk_market_session(&date, &session_start, &session_end);
        offset = session_start - (day * MS_PER_DAY + SESSION_OPEN_WALL_MS);
        cached_day = day;
      }
      if (!has_session)
        continue;
      t_ms += offset;
      /* Half-open: a bucket is labelled by its start, so the one labelled
       * with the close time is the first after-hours minute. */
      if (t_ms < session_start || t_ms >= session_end)
        continue;
    }
    (void)stk_series_append(series, t_ms, y);
  }
  return series;
}

static int
compare_points(gconstpointer a, gconstpointer b)
{
  const StkPoint *pa = a, *pb = b;
  return (pa->t_ms > pb->t_ms) - (pa->t_ms < pb->t_ms);
}

StkSeries *
stk_nasdaq_parse_historical(const guint8 *data, gsize len, StkRange range, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (!range_is_valid(range) || range == STK_RANGE_1D) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG,
                        "historical data supports the daily ranges only");
    return NULL;
  }
  JsonObject *root = NULL;
  g_autoptr(JsonParser) parser = parse_envelope(data, len, &root, error);
  if (parser == NULL)
    return NULL;
  JsonObject *info = envelope_data_object(root, error);
  if (info == NULL)
    return NULL;

  JsonArray *rows = NULL;
  JsonNode *table_node = json_object_get_member(info, "tradesTable");
  if (table_node != NULL && JSON_NODE_HOLDS_OBJECT(table_node)) {
    JsonNode *rows_node = json_object_get_member(json_node_get_object(table_node), "rows");
    if (rows_node != NULL && JSON_NODE_HOLDS_ARRAY(rows_node))
      rows = json_node_get_array(rows_node);
  } else if (table_node == NULL || !JSON_NODE_HOLDS_NULL(table_node)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "response has no tradesTable");
    return NULL;
  }

  guint n = rows != NULL ? json_array_get_length(rows) : 0;
  g_autoptr(GArray) collected = g_array_sized_new(FALSE, FALSE, sizeof(StkPoint), n);
  for (guint i = 0; i < n; i++) {
    JsonNode *element = json_array_get_element(rows, i);
    if (!JSON_NODE_HOLDS_OBJECT(element))
      continue;
    JsonObject *row = json_node_get_object(element);
    StkPoint pt;
    if (!parse_row_date(member_string(row, "date"), &pt.t_ms) ||
        stk_parse_price(member_string(row, "close"), &pt.v) != STK_PARSE_OK)
      continue;
    g_array_append_val(collected, pt);
  }
  /* The provider lists newest first; the series must ascend. */
  g_array_sort(collected, compare_points);

  StkSeries *series = stk_series_new(range, NAN, collected->len);
  for (guint i = 0; i < collected->len; i++) {
    const StkPoint *pt = &g_array_index(collected, StkPoint, i);
    (void)stk_series_append(series, pt->t_ms, pt->v);
  }
  return series;
}

GPtrArray *
stk_nasdaq_parse_search(const guint8 *data, gsize len, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  JsonObject *root = NULL;
  g_autoptr(JsonParser) parser = parse_envelope(data, len, &root, error);
  if (parser == NULL)
    return NULL;

  JsonNode *node = json_object_get_member(root, "data");
  JsonArray *items = NULL;
  if (node != NULL && JSON_NODE_HOLDS_ARRAY(node)) {
    items = json_node_get_array(node);
  } else if (node == NULL || !JSON_NODE_HOLDS_NULL(node)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "response has no data array");
    return NULL;
  }

  GPtrArray *hits = g_ptr_array_new_with_free_func((GDestroyNotify)stk_search_hit_free);
  guint n = items != NULL ? json_array_get_length(items) : 0;
  for (guint i = 0; i < n && hits->len < STK_NASDAQ_MAX_SEARCH_HITS; i++) {
    JsonNode *element = json_array_get_element(items, i);
    if (!JSON_NODE_HOLDS_OBJECT(element))
      continue;
    JsonObject *item = json_node_get_object(element);
    const char *symbol = member_string(item, "symbol");
    const char *asset = member_string(item, "asset");
    StkKind kind;
    if (asset == NULL || !stk_symbol_is_valid(symbol))
      continue;
    if (g_ascii_strcasecmp(asset, "STOCKS") == 0)
      kind = STK_KIND_STOCK;
    else if (g_ascii_strcasecmp(asset, "ETF") == 0)
      kind = STK_KIND_ETF;
    else if (g_ascii_strcasecmp(asset, "MUTUALFUNDS") == 0)
      kind = STK_KIND_FUND;
    else
      continue;

    const char *name = member_string(item, "name");
    StkSearchHit *hit = g_new0(StkSearchHit, 1);
    hit->symbol = g_ascii_strup(symbol, -1);
    hit->name = g_strdup(name != NULL ? name : "");
    hit->kind = kind;
    g_ptr_array_add(hits, hit);
  }
  return hits;
}

/* ------------------------------------------------------------------------ */
/* StkNasdaq object                                                         */
/* ------------------------------------------------------------------------ */

struct _StkNasdaq {
  GObject parent_instance;
  SoupSession *session; /* created once, shared by every request */
  char *base_url;       /* validated origin, no trailing slash */
  guint timeout_ms;
  guint backoff_base_ms;
  guint inflight; /* live Ops (requests not yet released), see STK_NASDAQ_MAX_INFLIGHT */
};

static void stk_nasdaq_provider_init(StkProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(StkNasdaq, stk_nasdaq, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(STK_TYPE_PROVIDER, stk_nasdaq_provider_init))

static void
stk_nasdaq_init(StkNasdaq *self)
{
  /* No cookie jar and no HTTP cache feature are added: nothing is persisted. */
  self->session = g_object_new(SOUP_TYPE_SESSION, "user-agent", STK_NASDAQ_USER_AGENT, NULL);
  self->base_url = g_strdup(STK_NASDAQ_BASE_URL);
  self->timeout_ms = STK_NASDAQ_TIMEOUT_MS;
  self->backoff_base_ms = STK_NASDAQ_BACKOFF_BASE_MS;
}

static void
stk_nasdaq_dispose(GObject *object)
{
  StkNasdaq *self = STK_NASDAQ(object);
  g_clear_object(&self->session);
  G_OBJECT_CLASS(stk_nasdaq_parent_class)->dispose(object);
}

static void
stk_nasdaq_finalize(GObject *object)
{
  StkNasdaq *self = STK_NASDAQ(object);
  g_free(self->base_url);
  G_OBJECT_CLASS(stk_nasdaq_parent_class)->finalize(object);
}

static void
stk_nasdaq_class_init(StkNasdaqClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = stk_nasdaq_dispose;
  object_class->finalize = stk_nasdaq_finalize;
}

StkNasdaq *
stk_nasdaq_new(void)
{
  return g_object_new(STK_TYPE_NASDAQ, NULL);
}

/* Returns a normalised copy of @base_url if it is an acceptable origin. */
static char *
validate_base_url(const char *base_url)
{
  g_autoptr(GUri) uri = g_uri_parse(base_url, G_URI_FLAGS_NONE, NULL);
  if (uri == NULL)
    return NULL;
  const char *scheme = g_uri_get_scheme(uri);
  const char *host = g_uri_get_host(uri);
  const char *path = g_uri_get_path(uri);
  if (host == NULL || *host == '\0' || g_uri_get_userinfo(uri) != NULL ||
      g_uri_get_query(uri) != NULL || g_uri_get_fragment(uri) != NULL ||
      !(path[0] == '\0' || strcmp(path, "/") == 0))
    return NULL;
  gboolean loopback = strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0 ||
                      strcmp(host, "localhost") == 0;
  if (!(strcmp(scheme, "https") == 0 || (strcmp(scheme, "http") == 0 && loopback)))
    return NULL;

  gsize len = strlen(base_url);
  if (len > 0 && base_url[len - 1] == '/')
    len--;
  return g_strndup(base_url, len);
}

StkNasdaq *
stk_nasdaq_new_custom(const char *base_url, guint timeout_ms, guint backoff_base_ms)
{
  g_return_val_if_fail(base_url != NULL, NULL);
  g_return_val_if_fail(timeout_ms >= 1 && timeout_ms <= 600000, NULL);
  g_return_val_if_fail(backoff_base_ms <= 600000, NULL);
  char *normalised = validate_base_url(base_url);
  if (normalised == NULL)
    return NULL;
  StkNasdaq *self = g_object_new(STK_TYPE_NASDAQ, NULL);
  g_free(self->base_url);
  self->base_url = normalised;
  self->timeout_ms = timeout_ms;
  self->backoff_base_ms = backoff_base_ms;
  return self;
}

/* ------------------------------------------------------------------------ */
/* Request layer                                                            */
/* ------------------------------------------------------------------------ */

/* OP_PREV_CLOSE: a quote request whose result is the previous session's close
 * (a heap double, NAN when unknown), see stk-nasdaq.h. */
typedef enum { OP_QUOTE, OP_PREV_CLOSE, OP_SERIES, OP_SEARCH } OpType;

/* Task data of one operation: what to parse and the state of the request. */
typedef struct {
  OpType type;
  char *url;
  char *symbol;   /* quote only */
  StkKind kind;   /* quote and series */
  StkRange range; /* series only */
  GCancellable *user_cancellable; /* the caller's (nullable), strong ref */
  StkNasdaq *owner;               /* strong ref; this Op counts in owner->inflight */

  guint attempt;     /* 0 for the first attempt */
  guint http_status; /* HTTP status of the current attempt, 0 until known */

  /* Per-attempt state, released by attempt_cleanup(). */
  SoupMessage *msg;
  GCancellable *attempt_cancel; /* cancelled by the deadline or by the user */
  gulong user_cancel_id;        /* handler on user_cancellable, 0 if none */
  GSource *deadline;
  gboolean timed_out;
  GInputStream *stream;
  GByteArray *body;
  gsize body_filled; /* bytes of body->data already read */
  goffset declared;  /* Content-Length of an undecoded body, else -1 */

  /* Back-off wait state, released by backoff_cleanup(). */
  GSource *backoff_timer;
  GSource *backoff_cancel;
} Op;

static char tag_quote, tag_series, tag_search;

static void
attempt_cleanup(Op *op)
{
  if (op->deadline != NULL) {
    g_source_destroy(op->deadline);
    g_clear_pointer(&op->deadline, g_source_unref);
  }
  if (op->user_cancel_id != 0) {
    g_cancellable_disconnect(op->user_cancellable, op->user_cancel_id);
    op->user_cancel_id = 0;
  }
  g_clear_object(&op->attempt_cancel);
  if (op->stream != NULL) {
    /* Closing hands the connection back to the pool; nothing waits for it. */
    g_input_stream_close_async(op->stream, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
    g_clear_object(&op->stream);
  }
  g_clear_object(&op->msg);
  g_clear_pointer(&op->body, g_byte_array_unref);
  op->body_filled = 0;
}

static void
backoff_cleanup(Op *op)
{
  if (op->backoff_timer != NULL) {
    g_source_destroy(op->backoff_timer);
    g_clear_pointer(&op->backoff_timer, g_source_unref);
  }
  if (op->backoff_cancel != NULL) {
    g_source_destroy(op->backoff_cancel);
    g_clear_pointer(&op->backoff_cancel, g_source_unref);
  }
}

static void
op_free(gpointer data)
{
  Op *op = data;
  attempt_cleanup(op);
  backoff_cleanup(op);
  g_clear_object(&op->user_cancellable);
  op->owner->inflight--;
  g_object_unref(op->owner);
  g_free(op->url);
  g_free(op->symbol);
  g_free(op);
}

static void
quote_destroy(gpointer quote)
{
  stk_quote_free(quote);
}

static void
series_destroy(gpointer series)
{
  stk_series_unref(series);
}

static void
hits_destroy(gpointer hits)
{
  g_ptr_array_unref(hits);
}

/* Parses the finished body and completes @task with the result. */
static void
op_complete(GTask *task, GBytes *body)
{
  Op *op = g_task_get_task_data(task);
  gsize len;
  const guint8 *data = g_bytes_get_data(body, &len);
  GError *error = NULL;

  switch (op->type) {
  case OP_QUOTE: {
    StkQuote *quote = stk_nasdaq_parse_quote(data, len, op->symbol, op->kind, &error);
    if (quote != NULL)
      g_task_return_pointer(task, quote, quote_destroy);
    break;
  }
  case OP_PREV_CLOSE: {
    gboolean regular;
    StkQuote *quote = parse_quote_impl(data, len, op->symbol, op->kind, &regular, &error);
    if (quote != NULL) {
      /* The regular-session block's change is relative to the previous close;
       * the fallback block's (after-hours) is relative to today's close. */
      double *prev = g_new(double, 1);
      double value = quote->price - quote->change;
      *prev = (regular && isfinite(value) && value > 0.0) ? value : NAN;
      stk_quote_free(quote);
      g_task_return_pointer(task, prev, g_free);
    }
    break;
  }
  case OP_SERIES:
    if (op->kind == STK_KIND_FUND) {
      StkSeries *series = stk_nasdaq_parse_historical(data, len, op->range, &error);
      if (series != NULL)
        g_task_return_pointer(task, series, series_destroy);
    } else {
      StkSeries *series = stk_nasdaq_parse_chart(data, len, op->range, &error);
      if (series != NULL)
        g_task_return_pointer(task, series, series_destroy);
    }
    break;
  case OP_SEARCH: {
    GPtrArray *hits = stk_nasdaq_parse_search(data, len, &error);
    if (hits != NULL)
      g_task_return_pointer(task, hits, hits_destroy);
    break;
  }
  default:
    g_assert_not_reached();
  }
  if (error != NULL)
    g_task_return_error(task, error);
}

static GError *
http_status_error(guint status)
{
  if (status == HTTP_TOO_MANY_REQUESTS)
    return g_error_new(STK_ERROR, STK_ERROR_RATE_LIMITED, "HTTP %u: rate limited", status);
  if (status == SOUP_STATUS_BAD_REQUEST || status == SOUP_STATUS_NOT_FOUND)
    return g_error_new(STK_ERROR, STK_ERROR_NOT_FOUND, "HTTP %u: not found", status);
  return g_error_new(STK_ERROR, STK_ERROR_HTTP, "HTTP %u %s", status,
                     soup_status_get_phrase(status));
}

/* Turns a libsoup/GIO failure of the current attempt into the error the
 * caller sees. Takes ownership of @err. */
static GError *
normalize_error(Op *op, GError *err)
{
  if (op->user_cancellable != NULL && g_cancellable_is_cancelled(op->user_cancellable)) {
    g_error_free(err);
    return g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "operation was cancelled");
  }
  if (err->domain == STK_ERROR)
    return err;
  GError *out;
  if (op->timed_out || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
    out = g_error_new_literal(STK_ERROR, STK_ERROR_TIMEOUT, "request timed out");
  else
    out = g_error_new(STK_ERROR, STK_ERROR_NETWORK, "network error: %s", err->message);
  g_error_free(err);
  return out;
}

static void
schedule_retry(GTask *task);

/* Ends the current attempt with @err: retries after a back-off when the
 * failure class allows it, otherwise completes @task with the error. */
static void
fail_attempt(GTask *task, GError *err)
{
  Op *op = g_task_get_task_data(task);
  GError *final = normalize_error(op, err);
  gboolean retryable = g_error_matches(final, STK_ERROR, STK_ERROR_NETWORK) ||
                       g_error_matches(final, STK_ERROR, STK_ERROR_TIMEOUT) ||
                       (g_error_matches(final, STK_ERROR, STK_ERROR_HTTP) &&
                        op->http_status >= 500 && op->http_status <= 599);
  attempt_cleanup(op);
  if (retryable && op->attempt < STK_NASDAQ_MAX_RETRIES) {
    g_error_free(final);
    schedule_retry(task);
  } else {
    g_task_return_error(task, final);
  }
}

static void
on_user_cancelled(GCancellable *user, gpointer attempt_cancel)
{
  (void)user;
  g_cancellable_cancel(G_CANCELLABLE(attempt_cancel));
}

static gboolean
on_deadline(gpointer user_data)
{
  GTask *task = user_data;
  Op *op = g_task_get_task_data(task);
  op->timed_out = TRUE;
  g_cancellable_cancel(op->attempt_cancel); /* the pending I/O callback reports it */
  return G_SOURCE_REMOVE;
}

static void
read_next(GTask *task);

static void
on_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = user_data;
  Op *op = g_task_get_task_data(task);
  GError *err = NULL;
  gssize n = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &err);
  if (n < 0) {
    fail_attempt(task, err);
    return;
  }
  op->body_filled += (gsize)n;
  if (op->body_filled > STK_NASDAQ_MAX_BODY_BYTES) {
    fail_attempt(task, g_error_new_literal(STK_ERROR, STK_ERROR_PARSE, "response too large"));
    return;
  }
  if (n > 0) {
    read_next(task);
    return;
  }
  /* A body shorter than its declared length is a dropped connection, not a
   * complete response (libsoup reports it as a plain end of stream). */
  if (op->declared >= 0 && (goffset)op->body_filled != op->declared) {
    fail_attempt(task, g_error_new_literal(STK_ERROR, STK_ERROR_NETWORK,
                                           "connection closed before the whole body arrived"));
    return;
  }
  /* End of body: hand it to the parser. */
  g_byte_array_set_size(op->body, (guint)op->body_filled);
  GBytes *body = g_byte_array_free_to_bytes(g_steal_pointer(&op->body));
  attempt_cleanup(op);
  op_complete(task, body);
  g_bytes_unref(body);
}

static void
read_next(GTask *task)
{
  Op *op = g_task_get_task_data(task);
  /* Never ask for more than one byte beyond the cap, so an oversized body is
   * detected without buffering it. */
  gsize chunk = MIN((gsize)READ_CHUNK_BYTES, (gsize)STK_NASDAQ_MAX_BODY_BYTES + 1 - op->body_filled);
  g_byte_array_set_size(op->body, (guint)(op->body_filled + chunk));
  g_input_stream_read_async(op->stream, op->body->data + op->body_filled, chunk,
                            G_PRIORITY_DEFAULT, op->attempt_cancel, on_read,
                            g_object_ref(task));
}

static void
on_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = user_data;
  Op *op = g_task_get_task_data(task);
  GError *err = NULL;
  GInputStream *stream = soup_session_send_finish(SOUP_SESSION(source), result, &err);
  if (stream == NULL) {
    fail_attempt(task, err);
    return;
  }
  op->stream = stream;
  op->http_status = soup_message_get_status(op->msg);
  if (op->http_status < 200 || op->http_status >= 300) {
    fail_attempt(task, http_status_error(op->http_status));
    return;
  }
  SoupMessageHeaders *headers = soup_message_get_response_headers(op->msg);
  goffset declared = soup_message_headers_get_content_length(headers);
  if (declared > STK_NASDAQ_MAX_BODY_BYTES) {
    fail_attempt(task, g_error_new_literal(STK_ERROR, STK_ERROR_PARSE, "response too large"));
    return;
  }
  /* Content-Length counts the encoded bytes; the stream yields decoded ones,
   * so the length can only be compared when no content coding is applied. */
  op->declared = soup_message_headers_get_one(headers, "Content-Encoding") == NULL &&
                         soup_message_headers_get_encoding(headers) == SOUP_ENCODING_CONTENT_LENGTH
                     ? declared
                     : -1;
  op->body = g_byte_array_sized_new(declared > 0 ? (guint)declared : READ_CHUNK_BYTES);
  op->body_filled = 0;
  read_next(task);
}

static void
attempt_start(GTask *task)
{
  Op *op = g_task_get_task_data(task);
  StkNasdaq *self = STK_NASDAQ(g_task_get_source_object(task));

  op->msg = soup_message_new(SOUP_METHOD_GET, op->url);
  if (op->msg == NULL) {
    g_task_return_error(task, g_error_new_literal(STK_ERROR, STK_ERROR_INVALID_ARG,
                                              "malformed request URL"));
    return;
  }
  soup_message_add_flags(op->msg, SOUP_MESSAGE_NO_REDIRECT);
  soup_message_headers_replace(soup_message_get_request_headers(op->msg), "Accept",
                               "application/json");
  op->timed_out = FALSE;
  op->http_status = 0;
  op->declared = -1;
  op->attempt_cancel = g_cancellable_new();
  if (op->user_cancellable != NULL)
    op->user_cancel_id = g_cancellable_connect(op->user_cancellable, G_CALLBACK(on_user_cancelled),
                                               g_object_ref(op->attempt_cancel), g_object_unref);

  op->deadline = g_timeout_source_new(self->timeout_ms);
  g_source_set_callback(op->deadline, on_deadline, g_object_ref(task), g_object_unref);
  g_source_attach(op->deadline, g_task_get_context(task));

  soup_session_send_async(self->session, op->msg, G_PRIORITY_DEFAULT, op->attempt_cancel, on_sent,
                          g_object_ref(task));
}

static gboolean
on_backoff_elapsed(gpointer user_data)
{
  g_autoptr(GTask) task = g_object_ref(user_data);
  backoff_cleanup(g_task_get_task_data(task));
  attempt_start(task);
  return G_SOURCE_REMOVE;
}

static gboolean
on_backoff_cancelled(GCancellable *user, gpointer user_data)
{
  (void)user;
  g_autoptr(GTask) task = g_object_ref(user_data);
  backoff_cleanup(g_task_get_task_data(task));
  g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                            "operation was cancelled"));
  return G_SOURCE_REMOVE;
}

/* Waits backoff_base_ms * 2^(retries so far), then starts the next attempt;
 * a cancellation during the wait completes the task immediately. */
static void
schedule_retry(GTask *task)
{
  Op *op = g_task_get_task_data(task);
  StkNasdaq *self = STK_NASDAQ(g_task_get_source_object(task));
  guint64 delay_ms = (guint64)self->backoff_base_ms << op->attempt;
  op->attempt++;

  op->backoff_timer = g_timeout_source_new((guint)delay_ms);
  g_source_set_callback(op->backoff_timer, on_backoff_elapsed, g_object_ref(task), g_object_unref);
  g_source_attach(op->backoff_timer, g_task_get_context(task));
  if (op->user_cancellable != NULL) {
    op->backoff_cancel = g_cancellable_source_new(op->user_cancellable);
    g_source_set_callback(op->backoff_cancel, G_SOURCE_FUNC(on_backoff_cancelled),
                          g_object_ref(task), g_object_unref);
    g_source_attach(op->backoff_cancel, g_task_get_context(task));
  }
}

/* ------------------------------------------------------------------------ */
/* StkProvider glue                                                         */
/* ------------------------------------------------------------------------ */

static Op *
op_new(StkNasdaq *owner, OpType type, char *url, const char *symbol, StkKind kind, StkRange range,
       GCancellable *user_cancellable)
{
  Op *op = g_new0(Op, 1);
  op->type = type;
  op->url = url;
  op->symbol = g_strdup(symbol);
  op->kind = kind;
  op->range = range;
  op->user_cancellable = user_cancellable != NULL ? g_object_ref(user_cancellable) : NULL;
  op->owner = g_object_ref(owner);
  owner->inflight++;
  return op;
}

/*
 * Admission control for an operation of @requests GETs. Returns TRUE if it
 * may start. Otherwise @task is completed with the error (@url_error for a
 * rejected argument, taken, when @url is NULL; STK_ERROR_RATE_LIMITED when
 * the in-flight cap would be exceeded), @url (taken) is freed, no network
 * traffic happens and FALSE is returned; the caller then drops its reference.
 */
static gboolean
admit(GTask *task, StkNasdaq *self, char *url, GError *url_error, guint requests)
{
  if (url == NULL) {
    g_task_return_error(task, url_error);
    return FALSE;
  }
  if (self->inflight + requests > STK_NASDAQ_MAX_INFLIGHT) {
    g_free(url);
    g_task_return_new_error(task, STK_ERROR, STK_ERROR_RATE_LIMITED,
                            "too many outstanding requests (limit %d)", STK_NASDAQ_MAX_INFLIGHT);
    return FALSE;
  }
  return TRUE;
}

/* Starts one admitted request as @task. Takes @url and the caller's
 * reference to @task. */
static void
op_start(GTask *task, OpType type, char *url, const char *symbol, StkKind kind, StkRange range)
{
  StkNasdaq *self = STK_NASDAQ(g_task_get_source_object(task));
  g_task_set_task_data(task,
                       op_new(self, type, url, symbol, kind, range, g_task_get_cancellable(task)),
                       op_free);
  if (!g_task_return_error_if_cancelled(task)) {
    attempt_start(task);
  }
  g_object_unref(task);
}

static void
nasdaq_fetch_quote_async(StkProvider *provider, const char *symbol, StkKind kind,
                         GCancellable *cancellable, GAsyncReadyCallback callback,
                         gpointer user_data)
{
  StkNasdaq *self = STK_NASDAQ(provider);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, &tag_quote);
  GError *error = NULL;
  char *url = stk_nasdaq_quote_url(self->base_url, symbol, kind, &error);
  if (admit(task, self, url, error, 1))
    op_start(task, OP_QUOTE, url, symbol, kind, STK_RANGE_1D);
  else
    g_object_unref(task);
}

static StkQuote *
nasdaq_fetch_quote_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_quote, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* ------------------------------------------------------------------------ */
/* 1D series: chart + quote joined                                          */
/* ------------------------------------------------------------------------ */

/* Task data of the series task of a 1D operation. The chart and the quote
 * run as two inner tasks on @child, a cancellable of their own, so that a
 * failed chart can cancel the quote and a caller's cancellation reaches both. */
typedef struct {
  GCancellable *child;
  GSource *user_cancel; /* watches the caller's cancellable (nullable) */
  StkSeries *chart;     /* parsed chart, once chart_done */
  double prev_close;    /* NAN until the quote supplied one */
  gboolean chart_done;
  gboolean quote_done;
  gboolean finished; /* the series task has been given its result */
} Join;

static void
join_unwatch(Join *join)
{
  if (join->user_cancel != NULL) {
    g_source_destroy(join->user_cancel);
    g_clear_pointer(&join->user_cancel, g_source_unref);
  }
}

static void
join_free(gpointer data)
{
  Join *join = data;
  join_unwatch(join);
  g_clear_object(&join->child);
  g_clear_pointer(&join->chart, stk_series_unref);
  g_free(join);
}

/* Fails the series with @error (taken) and cancels whatever is still running. */
static void
join_fail(GTask *series_task, GError *error)
{
  Join *join = g_task_get_task_data(series_task);
  join->finished = TRUE;
  join_unwatch(join);
  g_cancellable_cancel(join->child);
  g_task_return_error(series_task, error);
}

static void
join_try_complete(GTask *series_task)
{
  Join *join = g_task_get_task_data(series_task);
  if (!join->chart_done || !join->quote_done)
    return;
  join->finished = TRUE;
  join_unwatch(join);
  StkSeries *series = g_steal_pointer(&join->chart);
  series->prev_close = join->prev_close;
  g_task_return_pointer(series_task, series, series_destroy);
}

static void
on_chart_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  g_autoptr(GTask) series_task = user_data;
  Join *join = g_task_get_task_data(series_task);
  GError *error = NULL;
  StkSeries *series = g_task_propagate_pointer(G_TASK(result), &error);
  if (join->finished) { /* failed or cancelled meanwhile */
    g_clear_pointer(&series, stk_series_unref);
    g_clear_error(&error);
    return;
  }
  if (series == NULL) {
    join_fail(series_task, error);
    return;
  }
  join->chart = series;
  join->chart_done = TRUE;
  join_try_complete(series_task);
}

static void
on_prev_close_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  g_autoptr(GTask) series_task = user_data;
  Join *join = g_task_get_task_data(series_task);
  GError *error = NULL;
  double *prev = g_task_propagate_pointer(G_TASK(result), &error);
  if (join->finished) {
    g_free(prev);
    g_clear_error(&error);
    return;
  }
  /* Any quote failure, a cancellation included, only leaves prev_close NAN: a
   * caller's cancellation is reported by the series task itself (GTask
   * checks its cancellable when the result is collected). */
  if (prev != NULL)
    join->prev_close = *prev;
  g_free(prev);
  g_clear_error(&error);
  join->quote_done = TRUE;
  join_try_complete(series_task);
}

static gboolean
on_join_user_cancelled(GCancellable *user, gpointer child)
{
  (void)user;
  g_cancellable_cancel(G_CANCELLABLE(child));
  return G_SOURCE_REMOVE;
}

/*
 * Starts the two requests of a 1D series for @series_task, whose chart URL is
 * @chart_url (taken). Takes the caller's reference to @series_task.
 */
static void
series_1d_start(GTask *series_task, StkNasdaq *self, const char *symbol, StkKind kind,
                char *chart_url)
{
  /* The chart URL was built, so symbol and kind are valid. */
  char *quote_url = stk_nasdaq_quote_url(self->base_url, symbol, kind, NULL);
  g_assert(quote_url != NULL);

  Join *join = g_new0(Join, 1);
  join->child = g_cancellable_new();
  join->prev_close = NAN;
  g_task_set_task_data(series_task, join, join_free);

  GCancellable *user = g_task_get_cancellable(series_task);
  if (user != NULL) {
    /* A main-loop source rather than a signal handler: the cancellation then
     * never runs inside the caller's g_cancellable_cancel(). */
    join->user_cancel = g_cancellable_source_new(user);
    g_source_set_callback(join->user_cancel, G_SOURCE_FUNC(on_join_user_cancelled),
                          g_object_ref(join->child), g_object_unref);
    g_source_attach(join->user_cancel, g_task_get_context(series_task));
  }

  GTask *chart = g_task_new(self, join->child, on_chart_done, g_object_ref(series_task));
  GTask *quote = g_task_new(self, join->child, on_prev_close_done, g_object_ref(series_task));
  g_object_unref(series_task);
  op_start(chart, OP_SERIES, chart_url, symbol, kind, STK_RANGE_1D);
  op_start(quote, OP_PREV_CLOSE, quote_url, symbol, kind, STK_RANGE_1D);
}

static void
nasdaq_fetch_series_async(StkProvider *provider, const char *symbol, StkKind kind, StkRange range,
                          GCancellable *cancellable, GAsyncReadyCallback callback,
                          gpointer user_data)
{
  StkNasdaq *self = STK_NASDAQ(provider);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, &tag_series);

  /* The window ends on today's New York date; an invalid GDate (clock outside
   * the supported calendar) makes the builder reject daily ranges. */
  GDate today;
  g_date_clear(&today, 1);
  (void)stk_market_local_date(g_get_real_time() / 1000, &today);

  GError *error = NULL;
  char *url = stk_nasdaq_series_url(self->base_url, symbol, kind, range, &today, &error);
  gboolean one_day = range == STK_RANGE_1D;
  if (!admit(task, self, url, error, one_day ? 2 : 1)) {
    g_object_unref(task);
  } else if (one_day) {
    if (g_task_return_error_if_cancelled(task)) {
      g_free(url);
      g_object_unref(task);
    } else {
      series_1d_start(task, self, symbol, kind, url);
    }
  } else {
    op_start(task, OP_SERIES, url, symbol, kind, range);
  }
}

static StkSeries *
nasdaq_fetch_series_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_series, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
nasdaq_search_async(StkProvider *provider, const char *text, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer user_data)
{
  StkNasdaq *self = STK_NASDAQ(provider);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, &tag_search);
  GError *error = NULL;
  char *url = stk_nasdaq_search_url(self->base_url, text, &error);
  if (admit(task, self, url, error, 1))
    op_start(task, OP_SEARCH, url, NULL, STK_KIND_STOCK, STK_RANGE_1D);
  else
    g_object_unref(task);
}

static GPtrArray *
nasdaq_search_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_search, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
stk_nasdaq_provider_init(StkProviderInterface *iface)
{
  iface->fetch_quote_async = nasdaq_fetch_quote_async;
  iface->fetch_quote_finish = nasdaq_fetch_quote_finish;
  iface->fetch_series_async = nasdaq_fetch_series_async;
  iface->fetch_series_finish = nasdaq_fetch_series_finish;
  iface->search_async = nasdaq_search_async;
  iface->search_finish = nasdaq_search_finish;
}
