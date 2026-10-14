/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-yahoo.c - StkProvider implementation for the Yahoo Finance chart
 * endpoint (market indices only). See stk-yahoo.h for the behaviour contract.
 *
 * Layout: URL builder, JSON helpers, response parsers (pure), the StkYahoo
 * object, then the request layer: one GTask per operation, one shared queue (FIFO for new
 * operations; a retry re-enters at its head) that paces the attempts (at most one on the
 * wire, a minimum gap
 * between attempt starts), one libsoup message per attempt.
 */
#include "stk-yahoo.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <math.h>
#include <string.h>

#include "stk-market.h"

#define STK_YAHOO_USER_AGENT "stocks-app/0.1 (personal use)"

#define MS_PER_DAY G_GINT64_CONSTANT(86400000)
/* Unix epoch (1970-01-01) as a GDate Julian day (day 1 = 0001-01-01). */
#define UNIX_EPOCH_JULIAN G_GINT64_CONSTANT(719163)
/* 2022-01-01T00:00Z and 2101-01-01T00:00Z in epoch seconds: accepted
 * timestamp window, equal to the years the core calendar supports. */
#define EPOCH_MIN_S G_GINT64_CONSTANT(1640995200)
#define EPOCH_MAX_S G_GINT64_CONSTANT(4133980800)

#define NAME_MAX_BYTES 256
#define READ_CHUNK_BYTES (16 * 1024)
/* libsoup has no constant for 429. */
#define HTTP_TOO_MANY_REQUESTS 429

/* Size arithmetic of the request layer: the body buffer is sized with guint (GByteArray), so
 * cap + 1 must fit (on_sent(), on_read() and read_next() cast values <= cap + 1 to guint). */
G_STATIC_ASSERT((guint64)STK_YAHOO_MAX_BODY_BYTES + 1 <= G_MAXUINT);

/* ------------------------------------------------------------------------ */
/* URL builder                                                              */
/* ------------------------------------------------------------------------ */

/**
 * range_is_valid:
 * @range: any StkRange value (or an out-of-range integer)
 *
 * Tells whether @range is one of the StkRange enumerators STK_RANGE_1D .. STK_RANGE_1Y (the
 * values 0..4; stk_yahoo_chart_url() asserts at compile time that its lookup tables have exactly
 * these indices).
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure)
 * Ownership: none (no pointers).
 *
 * Returns: TRUE if STK_RANGE_1D <= @range <= STK_RANGE_1Y, FALSE otherwise.
 */
static gboolean
range_is_valid(StkRange range)
{
  return range >= STK_RANGE_1D && range <= STK_RANGE_1Y;
}

/**
 * check_symbol_kind:
 * @symbol: (nullable): candidate ticker
 * @kind: candidate kind
 * @error: (nullable): return location for a #GError
 *
 * Validates the symbol and kind of a request: the symbol must satisfy stk_symbol_is_valid() and
 * the kind must be STK_KIND_INDEX. The symbol is checked first.
 *
 * Preconditions:
 *  - @symbol is NULL or NUL-terminated (stk_symbol_is_valid() bounds its reads to 16 bytes);
 *    @error is NULL or points to a NULL GError*.
 * Postconditions:
 *  - on TRUE nothing is set; on FALSE *@error (when non-NULL) is a STK_ERROR /
 *    STK_ERROR_INVALID_ARG error.
 * Thread affinity: any thread (pure)
 * Ownership: @symbol and @error stay with the caller; nothing is retained.
 *
 * Returns: TRUE if stk_symbol_is_valid(@symbol) and @kind is STK_KIND_INDEX, else FALSE.
 */
static gboolean
check_symbol_kind(const char *symbol, StkKind kind, GError **error)
{
  if (!stk_symbol_is_valid(symbol)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG, "invalid symbol");
    return FALSE;
  }
  if (kind != STK_KIND_INDEX) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG,
                        "only market indices are served by this provider");
    return FALSE;
  }
  return TRUE;
}

/**
 * format_chart_url:
 * @base_url: (not nullable): origin, inserted verbatim
 * @symbol: (not nullable): a symbol that satisfies stk_symbol_is_valid()
 * @range_param: (not nullable): value of the range query parameter
 * @interval_param: (not nullable): value of the interval query parameter
 *
 * Formats the chart URL of @symbol (upper-cased and percent-encoded into the path).
 *
 * Preconditions:
 *  - all arguments are NUL-terminated; @symbol was checked by check_symbol_kind().
 * Postconditions:
 *  - nothing is modified.
 * Thread affinity: any thread (pure)
 * Ownership: the arguments stay with the caller; the caller owns the returned string.
 *
 * Returns: (transfer full): "<base>/v8/finance/chart/SYM?range=R&interval=I".
 */
static char *
format_chart_url(const char *base_url, const char *symbol, const char *range_param,
                 const char *interval_param)
{
  g_autofree char *upper = g_ascii_strup(symbol, -1);
  g_autofree char *escaped = g_uri_escape_string(upper, NULL, FALSE);
  return g_strdup_printf("%s/v8/finance/chart/%s?range=%s&interval=%s", base_url, escaped,
                         range_param, interval_param);
}

/* Documented in stk-yahoo.h. */
char *
stk_yahoo_chart_url(const char *base_url, const char *symbol, StkKind kind, StkRange range,
                    GError **error)
{
  g_return_val_if_fail(base_url != NULL, NULL);
  if (!check_symbol_kind(symbol, kind, error))
    return NULL;
  if (!range_is_valid(range)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG, "invalid range");
    return NULL;
  }
  static const char *const range_param[] = {"1d", "1mo", "3mo", "6mo", "1y"};
  static const char *const interval_param[] = {"5m", "1d", "1d", "1d", "1d"};
  /* range_is_valid() admits exactly the indices 0..4 (1D .. 1Y): the tables must cover them. */
  G_STATIC_ASSERT(STK_RANGE_1D == 0 && (gsize)STK_RANGE_1Y + 1 == G_N_ELEMENTS(range_param) &&
                  G_N_ELEMENTS(interval_param) == G_N_ELEMENTS(range_param));
  return format_chart_url(base_url, symbol, range_param[range], interval_param[range]);
}

/* Documented in stk-yahoo.h. */
char *
stk_yahoo_quote_url(const char *base_url, const char *symbol, StkKind kind, GError **error)
{
  g_return_val_if_fail(base_url != NULL, NULL);
  if (!check_symbol_kind(symbol, kind, error))
    return NULL;
  return format_chart_url(base_url, symbol, "5d", "1d");
}

/* ------------------------------------------------------------------------ */
/* JSON helpers (every accessor tolerates missing / null / mistyped nodes)  */
/* ------------------------------------------------------------------------ */

/**
 * member_object:
 * @obj: (nullable): a JSON object, or NULL
 * @name: (not nullable): member name
 *
 * Looks up an object-valued member; tolerates a NULL @obj and a missing, null or mistyped member.
 *
 * Preconditions:
 *  - @obj is NULL or a valid JsonObject; @name is NUL-terminated.
 * Postconditions:
 *  - nothing is modified.
 * Thread affinity: any thread (pure)
 * Ownership: the returned object is owned by the tree that owns @obj; nothing is retained.
 *
 * Returns: (transfer none) (nullable): the member object, or NULL if @obj is NULL or the member is
 *   missing, null or not an object.
 */
static JsonObject *
member_object(JsonObject *obj, const char *name)
{
  JsonNode *node = obj != NULL ? json_object_get_member(obj, name) : NULL;
  return (node != NULL && JSON_NODE_HOLDS_OBJECT(node)) ? json_node_get_object(node) : NULL;
}

/**
 * member_array:
 * @obj: (nullable): a JSON object, or NULL
 * @name: (not nullable): member name
 *
 * Looks up an array-valued member; tolerates a NULL @obj and a missing, null or mistyped member.
 *
 * Preconditions:
 *  - @obj is NULL or a valid JsonObject; @name is NUL-terminated.
 * Postconditions:
 *  - nothing is modified.
 * Thread affinity: any thread (pure)
 * Ownership: the returned array is owned by the tree that owns @obj; nothing is retained.
 *
 * Returns: (transfer none) (nullable): the member array, or NULL if @obj is NULL or the member is
 *   missing, null or not an array.
 */
static JsonArray *
member_array(JsonObject *obj, const char *name)
{
  JsonNode *node = obj != NULL ? json_object_get_member(obj, name) : NULL;
  return (node != NULL && JSON_NODE_HOLDS_ARRAY(node)) ? json_node_get_array(node) : NULL;
}

/**
 * member_string:
 * @obj: (nullable): a JSON object, or NULL
 * @name: (not nullable): member name
 *
 * Looks up a string-valued member; tolerates a NULL @obj and a missing, null or mistyped member.
 *
 * Preconditions:
 *  - @obj is NULL or a valid JsonObject; @name is NUL-terminated.
 * Postconditions:
 *  - nothing is modified.
 * Thread affinity: any thread (pure)
 * Ownership: the returned string is owned by the tree that owns @obj; nothing is retained.
 *
 * Returns: (transfer none) (nullable): the NUL-terminated string, or NULL if @obj is NULL or the
 *   member is missing, null or not a string.
 */
static const char *
member_string(JsonObject *obj, const char *name)
{
  JsonNode *node = obj != NULL ? json_object_get_member(obj, name) : NULL;
  if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string(node);
}

/**
 * node_number:
 * @node: (nullable): a JSON node, or NULL
 * @out: (out): receives the number
 *
 * Reads a JSON integer or floating-point value as a finite double.
 *
 * Preconditions:
 *  - @node is NULL or a valid JsonNode; @out is non-NULL and writable.
 * Postconditions:
 *  - on TRUE *@out is a finite double; on FALSE *@out is untouched.
 * Thread affinity: any thread (pure)
 * Ownership: @node stays with its tree; @out is caller-owned; nothing is retained.
 *
 * Returns: TRUE if @node holds a finite int64 or double value, FALSE for NULL, any other type
 *   (including strings and null) and non-finite values.
 */
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

/**
 * member_number:
 * @obj: (nullable): a JSON object, or NULL
 * @name: (not nullable): member name
 * @out: (out): receives the number
 *
 * Reads a numeric member as a finite double (see node_number()); a NULL @obj yields FALSE.
 *
 * Preconditions:
 *  - @obj is NULL or a valid JsonObject; @name is NUL-terminated; @out is non-NULL and writable.
 * Postconditions:
 *  - on TRUE *@out is a finite double; on FALSE *@out is untouched.
 * Thread affinity: any thread (pure)
 * Ownership: @obj stays with its tree; @out is caller-owned; nothing is retained.
 *
 * Returns: TRUE if the member exists and node_number() accepts it, FALSE otherwise.
 */
static gboolean
member_number(JsonObject *obj, const char *name, double *out)
{
  return obj != NULL && node_number(json_object_get_member(obj, name), out);
}

/**
 * node_epoch_s:
 * @node: (nullable): a JSON node, or NULL
 * @out: (out): receives the epoch seconds
 *
 * Reads epoch seconds inside the supported window [EPOCH_MIN_S, EPOCH_MAX_S) (2022-01-01 ..
 * 2100-12-31). A fractional part is truncated toward zero.
 *
 * Preconditions:
 *  - @node is NULL or a valid JsonNode; @out is non-NULL and writable.
 * Postconditions:
 *  - on TRUE *@out is in the window; on FALSE *@out is untouched. The double is converted to
 *    gint64 only after node_number() proved it finite and the comparison proved it inside the
 *    window, so the conversion is defined.
 * Thread affinity: any thread (pure)
 * Ownership: @node stays with its tree; @out is caller-owned; nothing is retained.
 *
 * Returns: TRUE if @node is a finite number inside the window, FALSE otherwise.
 */
static gboolean
node_epoch_s(JsonNode *node, gint64 *out)
{
  double d;
  if (!node_number(node, &d) || d < (double)EPOCH_MIN_S || d >= (double)EPOCH_MAX_S)
    return FALSE;
  *out = (gint64)d;
  return TRUE;
}

/**
 * truncated_copy:
 * @text: (not nullable): valid UTF-8 text
 * @max_chars: maximum number of characters to copy (a value <= 0 copies none)
 *
 * Copy of at most the first @max_chars characters of valid UTF-8 @text, so a server message cannot
 * bloat an error message or split a UTF-8 sequence.
 *
 * Preconditions:
 *  - @text is non-NULL, NUL-terminated and valid UTF-8, so g_utf8_next_char() never steps past the
 *    terminator. The only caller, load_chart(), passes a JSON string from a document whose bytes
 *    passed g_utf8_validate() and which json-glib decoded.
 * Postconditions:
 *  - @text is unchanged; the copy ends on a character boundary.
 * Thread affinity: any thread (pure)
 * Ownership: @text stays with the caller; the returned string is new.
 *
 * Returns: (transfer full): the NUL-terminated copy, to be released with g_free().
 */
static char *
truncated_copy(const char *text, glong max_chars)
{
  const char *end = text;
  for (glong i = 0; i < max_chars && *end != '\0'; i++)
    end = g_utf8_next_char(end);
  return g_strndup(text, (gsize)(end - text));
}

/**
 * load_chart:
 * @data: (nullable): response body, @len bytes, need not be NUL-terminated
 * @len: body length in bytes
 * @result: (out): receives chart.result[0]
 * @meta: (out): receives the "meta" object of chart.result[0]
 * @error: (nullable): return location for a #GError
 *
 * Validates @data as a UTF-8 JSON chart envelope and returns the parser (owner of the objects).
 * chart.error (any non-null value) is NOT_FOUND.
 *
 * Preconditions:
 *  - when @data is non-NULL and @len <= STK_YAHOO_MAX_BODY_BYTES, @data points to @len readable
 *    bytes (a larger @len is rejected before any byte is read); @result and @meta are non-NULL and
 *    writable; @error is NULL or points to a NULL GError*.
 * Postconditions:
 *  - on success *@result and *@meta point into the tree owned by the returned parser and are valid
 *    only until it is released; on failure NULL is returned, *@result and *@meta are untouched and
 *    *@error (when non-NULL) is STK_ERROR_PARSE (empty or NULL data, too large, invalid UTF-8,
 *    malformed JSON, not an object, no chart object, no usable chart.result[0].meta) or
 *    STK_ERROR_NOT_FOUND (chart.error set; its description, cut to 80 characters, is appended).
 *    The length is cast to gssize only after the cap check proved it is <= 2 MiB.
 * Thread affinity: any thread (pure)
 * Ownership: @data stays with the caller and is not retained; the caller owns the returned
 *   parser.
 *
 * Returns: (transfer full) (nullable): the parser, or NULL with @error set.
 */
static JsonParser *
load_chart(const guint8 *data, gsize len, JsonObject **result, JsonObject **meta, GError **error)
{
  if (len == 0 || data == NULL) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "empty response");
    return NULL;
  }
  if (len > STK_YAHOO_MAX_BODY_BYTES) {
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
  JsonObject *chart = member_object(json_node_get_object(root_node), "chart");
  if (chart == NULL) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "response has no chart object");
    return NULL;
  }

  JsonNode *err_node = json_object_get_member(chart, "error");
  if (err_node != NULL && !JSON_NODE_HOLDS_NULL(err_node)) {
    const char *desc = JSON_NODE_HOLDS_OBJECT(err_node)
                           ? member_string(json_node_get_object(err_node), "description")
                           : NULL;
    g_autofree char *short_desc = desc != NULL ? truncated_copy(desc, 80) : NULL;
    g_set_error(error, STK_ERROR, STK_ERROR_NOT_FOUND, "chart error%s%s",
                short_desc != NULL ? ": " : "", short_desc != NULL ? short_desc : "");
    return NULL;
  }

  JsonArray *results = member_array(chart, "result");
  JsonNode *first = results != NULL && json_array_get_length(results) > 0
                        ? json_array_get_element(results, 0)
                        : NULL;
  JsonObject *res = first != NULL && JSON_NODE_HOLDS_OBJECT(first) ? json_node_get_object(first) : NULL;
  JsonObject *m = member_object(res, "meta");
  if (m == NULL) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "response has no chart result");
    return NULL;
  }
  *result = res;
  *meta = m;
  return g_steal_pointer(&parser);
}

/* ------------------------------------------------------------------------ */
/* Parsers                                                                  */
/* ------------------------------------------------------------------------ */

/**
 * close_before:
 * @result: (not nullable): chart.result[0] of a daily (interval=1d) response
 * @day: (not nullable): New York date of a trading day
 *
 * Previous close of the session on @day, read from the daily samples: of the samples dated (New
 * York date of the timestamp) before @day, only those of the LATEST such date count, and the close
 * of the latest of them with a usable close is returned. A date whose samples all lack a usable
 * close yields NAN rather than the close of an earlier date, which would belong to an older
 * session. Samples with an unusable timestamp are ignored; timestamp[] and close[] are paired by
 * index up to the shorter length.
 *
 * meta.previousClose is not used: Yahoo has served it stale (on 2026-10-13 the ^GSPC and ^DJI
 * responses gave the close of 2026-10-09 although 2026-10-12 was a full session, which their own
 * daily samples showed).
 *
 * Preconditions:
 *  - @result is a valid JsonObject; @day is a valid GDate.
 * Postconditions:
 *  - nothing is modified.
 * Thread affinity: any thread (pure; the only shared state is a read-only, once-initialised time
 *   zone)
 * Ownership: @result stays with its tree; @day stays with the caller; nothing is retained.
 *
 * Returns: the previous close (finite, > 0), or NAN if there is none.
 */
static double
close_before(JsonObject *result, const GDate *day)
{
  JsonArray *timestamps = member_array(result, "timestamp");
  JsonArray *quotes = member_array(member_object(result, "indicators"), "quote");
  JsonNode *q0 = quotes != NULL && json_array_get_length(quotes) > 0 ? json_array_get_element(quotes, 0)
                                                                      : NULL;
  JsonArray *closes = member_array(q0 != NULL && JSON_NODE_HOLDS_OBJECT(q0) ? json_node_get_object(q0) : NULL,
                                   "close");
  if (timestamps == NULL || closes == NULL)
    return NAN;
  guint n = MIN(json_array_get_length(timestamps), json_array_get_length(closes));

  GDate best_date; /* latest date before @day seen so far; valid once have_date is TRUE */
  gboolean have_date = FALSE;
  gint64 best_s = 0; /* raw timestamp of the sample that gave best */
  double best = NAN;
  for (guint i = 0; i < n; i++) {
    gint64 t_s;
    GDate date;
    if (!node_epoch_s(json_array_get_element(timestamps, i), &t_s) ||
        !stk_market_local_date(t_s * 1000, &date) || g_date_compare(&date, day) >= 0)
      continue;
    int cmp = have_date ? g_date_compare(&date, &best_date) : 1;
    if (cmp < 0)
      continue;
    if (cmp > 0) { /* a later date: what an earlier one gave no longer counts */
      best_date = date;
      have_date = TRUE;
      best = NAN;
    }
    double v;
    if (node_number(json_array_get_element(closes, i), &v) && v > 0.0 && (isnan(best) || t_s >= best_s)) {
      best = v;
      best_s = t_s;
    }
  }
  return best;
}

/**
 * name_is_usable:
 * @name: (nullable): NUL-terminated text from a JSON string
 *
 * Tells whether an external display name may be stored: non-empty, at most NAME_MAX_BYTES bytes,
 * valid UTF-8 and free of control characters. The length is established with a bounded memchr().
 *
 * Preconditions:
 *  - @name is NULL or NUL-terminated (JSON-parsed text); at most NAME_MAX_BYTES + 1 bytes are
 *    examined for the terminator.
 * Postconditions:
 *  - nothing is modified.
 * Thread affinity: any thread (pure)
 * Ownership: @name stays with the caller; nothing is retained.
 *
 * Returns: TRUE if @name is usable, FALSE for NULL, "", a name longer than NAME_MAX_BYTES, invalid
 *   UTF-8 or a name containing a control character.
 */
static gboolean
name_is_usable(const char *name)
{
  if (name == NULL || name[0] == '\0')
    return FALSE;
  const char *nul = memchr(name, '\0', NAME_MAX_BYTES + 1);
  if (nul == NULL || !g_utf8_validate(name, (gssize)(nul - name), NULL))
    return FALSE;
  for (const char *p = name; p < nul; p = g_utf8_next_char(p))
    if (g_unichar_iscntrl(g_utf8_get_char(p)))
      return FALSE;
  return TRUE;
}

/* Documented in stk-yahoo.h. */
StkQuote *
stk_yahoo_parse_quote(const guint8 *data, gsize len, const char *symbol, StkKind kind,
                      GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (!check_symbol_kind(symbol, kind, error))
    return NULL;
  JsonObject *result = NULL, *meta = NULL;
  g_autoptr(JsonParser) parser = load_chart(data, len, &result, &meta, error);
  if (parser == NULL)
    return NULL;

  double price;
  if (!member_number(meta, "regularMarketPrice", &price) || price <= 0.0) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "quote has no usable price");
    return NULL;
  }

  const char *currency = member_string(meta, "currency");
  if (currency != NULL &&
      !(g_ascii_isupper(currency[0]) && g_ascii_isupper(currency[1]) &&
        g_ascii_isupper(currency[2]) && currency[3] == '\0'))
    currency = NULL;
  const char *name = member_string(meta, "longName");
  if (!name_is_usable(name))
    name = member_string(meta, "shortName");
  if (!name_is_usable(name))
    name = NULL;

  StkQuote *quote = stk_quote_new(symbol, name, kind, currency);
  quote->price = price;
  gint64 as_of_s;
  GDate session;
  if (node_epoch_s(json_object_get_member(meta, "regularMarketTime"), &as_of_s)) {
    quote->as_of_ms = as_of_s * 1000;
    /* The price belongs to the session of regularMarketTime's date; its change is measured
     * against the close of the session before it. */
    double prev = stk_market_local_date(quote->as_of_ms, &session) ? close_before(result, &session)
                                                                   : NAN;
    if (!isnan(prev)) {
      double change = price - prev;
      double pct = change / prev * 100.0;
      if (isfinite(change) && isfinite(pct)) { /* a tiny previous close can overflow */
        quote->change = change;
        quote->change_pct = pct;
      }
    }
  }
  return quote;
}

/* Documented in stk-yahoo.h. */
double
stk_yahoo_parse_prev_close(const guint8 *data, gsize len, gint64 day_ms, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NAN);
  JsonObject *result = NULL, *meta = NULL;
  g_autoptr(JsonParser) parser = load_chart(data, len, &result, &meta, error);
  if (parser == NULL)
    return NAN;
  GDate day;
  return stk_market_local_date(day_ms, &day) ? close_before(result, &day) : NAN;
}

/**
 * parse_series:
 * @data: (nullable): chart response body fetched for @range
 * @len: body length
 * @range: the requested range
 * @day_ms: (out) (optional): for STK_RANGE_1D, receives the start of the regular session the
 *   response describes (epoch ms); untouched for daily ranges and on failure
 * @error: (nullable): return location for a #GError
 *
 * stk_yahoo_parse_series() that also reports the trading day of a 1D response, which the request
 * layer needs to ask for that day's previous close.
 *
 * Preconditions / postconditions / errors: those of stk_yahoo_parse_series(); @day_ms is NULL or
 *   writable.
 * Thread affinity: any thread (pure; the only shared state is a read-only, once-initialised time
 *   zone)
 * Ownership: @data stays with the caller and is not retained; the caller owns the returned series.
 *
 * Returns: (transfer full) (nullable): the series, or NULL with @error set.
 */
static StkSeries *
parse_series(const guint8 *data, gsize len, StkRange range, gint64 *day_ms, GError **error)
{
  if (!range_is_valid(range)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG, "invalid range");
    return NULL;
  }
  JsonObject *result = NULL, *meta = NULL;
  g_autoptr(JsonParser) parser = load_chart(data, len, &result, &meta, error);
  if (parser == NULL)
    return NULL;

  const gboolean intraday = range == STK_RANGE_1D;
  gint64 start_ms = 0, end_ms = 0;
  if (intraday) {
    JsonObject *regular = member_object(member_object(meta, "currentTradingPeriod"), "regular");
    gint64 start_s, end_s;
    if (regular == NULL || !node_epoch_s(json_object_get_member(regular, "start"), &start_s) ||
        !node_epoch_s(json_object_get_member(regular, "end"), &end_s) || start_s >= end_s) {
      g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE,
                          "response has no regular trading period");
      return NULL;
    }
    start_ms = start_s * 1000;
    end_ms = end_s * 1000;
  }

  /* timestamp absent or null: no samples (a valid, empty series). */
  JsonNode *ts_node = json_object_get_member(result, "timestamp");
  JsonArray *timestamps = NULL;
  if (ts_node != NULL && !JSON_NODE_HOLDS_NULL(ts_node)) {
    if (!JSON_NODE_HOLDS_ARRAY(ts_node)) {
      g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "timestamp is not an array");
      return NULL;
    }
    timestamps = json_node_get_array(ts_node);
  }
  guint n_ts = timestamps != NULL ? json_array_get_length(timestamps) : 0;

  JsonArray *closes = NULL;
  if (n_ts > 0) {
    JsonArray *quotes = member_array(member_object(result, "indicators"), "quote");
    JsonNode *q0 = quotes != NULL && json_array_get_length(quotes) > 0 ? json_array_get_element(quotes, 0)
                                                                        : NULL;
    closes = member_array(q0 != NULL && JSON_NODE_HOLDS_OBJECT(q0) ? json_node_get_object(q0) : NULL,
                          "close");
    if (closes == NULL) {
      g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "response has no close prices");
      return NULL;
    }
  }
  guint n_close = closes != NULL ? json_array_get_length(closes) : 0;

  /* Collected first: for daily ranges a later sample may replace the point of
   * the same date. */
  g_autoptr(GArray) points = g_array_sized_new(FALSE, FALSE, sizeof(StkPoint), MIN(n_ts, 4096u));
  gint64 last_s = 0; /* raw epoch of the sample that produced the last point */
  for (guint i = 0; i < n_ts && i < n_close; i++) {
    gint64 t_s;
    double v;
    if (!node_epoch_s(json_array_get_element(timestamps, i), &t_s) ||
        !node_number(json_array_get_element(closes, i), &v))
      continue;
    gint64 t_ms = t_s * 1000;
    if (intraday) {
      if (t_ms < start_ms || t_ms >= end_ms)
        continue;
    } else {
      GDate date;
      if (!stk_market_local_date(t_ms, &date))
        continue;
      t_ms = ((gint64)g_date_get_julian(&date) - UNIX_EPOCH_JULIAN) * MS_PER_DAY;
    }
    /* Samples whose raw timestamp goes backwards are skipped, so the sample
     * that produced the point of a date is always the latest one of it. */
    StkPoint *last = points->len > 0 ? &g_array_index(points, StkPoint, points->len - 1) : NULL;
    if (last != NULL && t_s < last_s)
      continue;
    last_s = t_s;
    if (last != NULL && !intraday && t_ms == last->t_ms) {
      last->v = v;
      continue;
    }
    StkPoint p = {t_ms, v};
    g_array_append_val(points, p);
  }

  /* prev_close stays NAN: meta.previousClose can be stale (see close_before()) and the
   * chartPreviousClose of a daily response is the close before the range start. */
  StkSeries *series = stk_series_new(range, NAN, points->len);
  for (guint i = 0; i < points->len; i++) {
    const StkPoint *p = &g_array_index(points, StkPoint, i);
    /* Points are collected in ascending order with finite values: the append cannot fail. */
    (void)stk_series_append(series, p->t_ms, p->v);
  }
  if (intraday && day_ms != NULL)
    *day_ms = start_ms;
  return series;
}

/* Documented in stk-yahoo.h. */
StkSeries *
stk_yahoo_parse_series(const guint8 *data, gsize len, StkRange range, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  return parse_series(data, len, range, NULL, error);
}

/* ------------------------------------------------------------------------ */
/* StkYahoo object                                                          */
/* ------------------------------------------------------------------------ */

struct _StkYahoo {
  GObject parent_instance;
  SoupSession *session; /* created once, shared by every request */
  char *base_url;       /* validated origin, no trailing slash */
  guint timeout_ms;
  guint min_gap_ms;
  guint inflight; /* live Ops (queued, on the wire or between attempts) */

  /* Pacing. queue holds the GTasks (one reference each) waiting for their
   * next attempt; on_wire is TRUE from an attempt's start until it has ended;
   * next_start_us is the earliest g_get_monotonic_time() at which the next
   * attempt may start; pump_timer wakes pump() when that time is reached
   * (NULL unless the queue head is waiting for it). */
  GQueue queue;
  gboolean on_wire;
  gint64 next_start_us;
  GSource *pump_timer;
};

static void stk_yahoo_provider_init(StkProviderInterface *iface);

/*
 * G_DEFINE_TYPE_WITH_CODE(StkYahoo, stk_yahoo, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(
 * STK_TYPE_PROVIDER, stk_yahoo_provider_init)) generates the following (checked with gcc -E of
 * this file with the project's compile flags):
 *  - GType stk_yahoo_get_type(void): returns the GType of StkYahoo and registers it on the first
 *    call, inside a once-initialisation guard, by calling stk_yahoo_get_type_once().
 *    Thread affinity: any thread. Ownership: none (no pointers).
 *  - static GType stk_yahoo_get_type_once(void): registers StkYahoo as a static type derived
 *    from G_TYPE_OBJECT (class structure StkYahooClass, instance structure StkYahoo, class
 *    initialiser stk_yahoo_class_intern_init(), instance initialiser stk_yahoo_init()). The
 *    _WITH_CODE argument G_IMPLEMENT_INTERFACE() then adds the StkProvider interface to the new
 *    type with g_type_add_interface_static(), using stk_yahoo_provider_init() as the interface
 *    initialiser (no interface data), and the new GType is returned.
 *    Thread affinity: only via stk_yahoo_get_type(). Ownership: none (no pointers).
 *  - static void stk_yahoo_class_intern_init(gpointer klass): @klass is the class structure
 *    being initialised (borrowed, written; nothing is retained). It stores the parent class in
 *    stk_yahoo_parent_class, adjusts the private offset only if StkYahoo_private_offset is
 *    non-zero, then calls stk_yahoo_class_init(). Called by GObject once per type, from the
 *    thread that first uses the type. Ownership: @klass belongs to GObject.
 *  - static gpointer stk_yahoo_parent_class: NULL until class_intern_init() runs; used by
 *    dispose() and finalize() to chain up.
 *  - static gint StkYahoo_private_offset: stays 0 (no G_ADD_PRIVATE is used).
 *  - static inline gpointer stk_yahoo_get_instance_private(StkYahoo *self): returns self plus
 *    the private offset; unused here (G_GNUC_UNUSED). @self is borrowed. Thread affinity: any
 *    thread.
 * The interface initialiser stk_yahoo_provider_init(StkProviderInterface *iface) is not
 * generated; it is the function named in the _WITH_CODE argument and is defined below. GObject
 * calls it with the interface structure of StkYahoo (@iface is borrowed, written, nothing is
 * retained; owned by GObject) when the interface is set up for the type, and it stores the six
 * yahoo_*_async / yahoo_*_finish implementations in the vtable.
 */
G_DEFINE_TYPE_WITH_CODE(StkYahoo, stk_yahoo, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(STK_TYPE_PROVIDER, stk_yahoo_provider_init))

/**
 * stk_yahoo_init:
 * @self: (not nullable): the new instance
 *
 * GObject instance initialiser: creates the shared HTTP session and sets the production defaults.
 *
 * Preconditions:
 *  - @self is a zero-initialised StkYahoo instance being constructed by GObject.
 * Postconditions:
 *  - @self->session is a new SoupSession with the app user agent; base_url, timeout_ms and
 *    min_gap_ms hold the STK_YAHOO_* defaults; the queue is empty, inflight is 0, nothing is on
 *    the wire and no pump timer is armed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @self owns the session and the base_url copy; they are released by dispose and
 *   finalize.
 */
static void
stk_yahoo_init(StkYahoo *self)
{
  /* No cookie jar and no HTTP cache feature are added: nothing is persisted. */
  self->session = g_object_new(SOUP_TYPE_SESSION, "user-agent", STK_YAHOO_USER_AGENT, NULL);
  self->base_url = g_strdup(STK_YAHOO_BASE_URL);
  self->timeout_ms = STK_YAHOO_TIMEOUT_MS;
  self->min_gap_ms = STK_YAHOO_MIN_GAP_MS;
  g_queue_init(&self->queue);
}

/**
 * stk_yahoo_dispose:
 * @object: (not nullable): the StkYahoo being disposed
 *
 * GObject dispose: destroys the pump timer and drops the HTTP session. Operations hold a
 * reference to the instance (Op.owner), so none is outstanding here and the queue is empty.
 *
 * Preconditions:
 *  - @object is a StkYahoo; may run more than once (the clears below make that safe).
 * Postconditions:
 *  - @self->pump_timer is destroyed and NULL (so the timer can never fire on a finalised
 *    instance); @self->session is NULL; the parent dispose has run.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the timer source and session references owned by the instance are released;
 *   @object stays with the caller.
 */
static void
stk_yahoo_dispose(GObject *object)
{
  StkYahoo *self = STK_YAHOO(object);
  /* Operations hold a reference, so none is outstanding here. */
  if (self->pump_timer != NULL) {
    g_source_destroy(self->pump_timer);
    g_clear_pointer(&self->pump_timer, g_source_unref);
  }
  g_clear_object(&self->session);
  G_OBJECT_CLASS(stk_yahoo_parent_class)->dispose(object);
}

/**
 * stk_yahoo_finalize:
 * @object: (not nullable): the StkYahoo being finalized
 *
 * GObject finalize: frees the base URL copy.
 *
 * Preconditions:
 *  - @object is a StkYahoo whose last reference is gone (runs once); its queue is empty.
 * Postconditions:
 *  - base_url is freed; the parent finalize has run.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the instance owns base_url; it is freed here.
 */
static void
stk_yahoo_finalize(GObject *object)
{
  StkYahoo *self = STK_YAHOO(object);
  g_free(self->base_url);
  G_OBJECT_CLASS(stk_yahoo_parent_class)->finalize(object);
}

/**
 * stk_yahoo_class_init:
 * @klass: (not nullable): the class structure being initialised
 *
 * GObject class initialiser: installs dispose and finalize.
 *
 * Preconditions:
 *  - called once by the GType system.
 * Postconditions:
 *  - dispose and finalize are overridden; no properties or signals exist.
 * Thread affinity: any thread (called by GObject under its type lock).
 * Ownership: @klass is owned by the type system and not retained.
 */
static void
stk_yahoo_class_init(StkYahooClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = stk_yahoo_dispose;
  object_class->finalize = stk_yahoo_finalize;
}

/* Documented in stk-yahoo.h. */
StkYahoo *
stk_yahoo_new(void)
{
  return g_object_new(STK_TYPE_YAHOO, NULL);
}

/**
 * validate_base_url:
 * @base_url: NUL-terminated URL text
 *
 * Returns a normalised copy of @base_url if it is an acceptable origin (see
 * stk_yahoo_new_custom()).
 *
 * Preconditions:
 *  - @base_url is non-NULL and NUL-terminated (the caller, stk_yahoo_new_custom(), checks
 *    non-NULL; g_uri_parse() relies on the terminator as well). The parsed GUri accessors are
 *    compared with g_str_equal(): for a successfully parsed URI the scheme and path are non-NULL
 *    and the host is checked for NULL first.
 * Postconditions:
 *  - @base_url is unchanged. The copy is made with g_strdup() and its length measured with strlen()
 *    on that GLib-created copy; one trailing "/" is removed. NULL is returned for an unparsable
 *    URL, a missing/empty host, userinfo, query, fragment, a path other than "" or "/", a scheme
 *    other than https, or http to a non-loopback host.
 * Thread affinity: any thread (pure)
 * Ownership: @base_url stays with the caller.
 *
 * Returns: (transfer full) (nullable): the normalised origin, or NULL if not acceptable.
 */
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
      !(path[0] == '\0' || g_str_equal(path, "/")))
    return NULL;
  gboolean loopback = g_str_equal(host, "127.0.0.1") || g_str_equal(host, "::1") ||
                      g_str_equal(host, "localhost");
  if (!(g_str_equal(scheme, "https") || (g_str_equal(scheme, "http") && loopback)))
    return NULL;

  char *normalised = g_strdup(base_url);
  gsize len = strlen(normalised); /* GLib-created copy: NUL-terminated */
  if (len > 0 && normalised[len - 1] == '/')
    normalised[len - 1] = '\0';
  return normalised;
}

/* Documented in stk-yahoo.h. */
StkYahoo *
stk_yahoo_new_custom(const char *base_url, guint timeout_ms, guint min_gap_ms, GError **error)
{
  g_return_val_if_fail(base_url != NULL, NULL);
  g_return_val_if_fail(timeout_ms >= 1 && timeout_ms <= 600000, NULL);
  g_return_val_if_fail(min_gap_ms <= 600000, NULL);
  char *normalised = validate_base_url(base_url);
  if (normalised == NULL) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_INVALID_ARG, "unacceptable base URL");
    return NULL;
  }
  StkYahoo *self = g_object_new(STK_TYPE_YAHOO, NULL);
  g_free(self->base_url);
  self->base_url = normalised;
  self->timeout_ms = timeout_ms;
  self->min_gap_ms = min_gap_ms;
  return self;
}

/* ------------------------------------------------------------------------ */
/* Request layer                                                            */
/* ------------------------------------------------------------------------ */

typedef enum { OP_QUOTE, OP_SERIES } OpType;

/*
 * Task data of one operation: what to parse and the state of the request. Created by op_new(),
 * owned by the GTask (task data, freed by op_free() when the task is finalised) and used from the
 * main thread only.
 *
 * A STK_RANGE_1D series is two requests made one after the other: the chart (url), then the quote
 * URL (next_url) for the previous close of the chart's trading day. Between the two, series holds
 * the parsed chart and day_ms its trading day; series != NULL is what tells that the second
 * request is the current one.
 *
 * Ownership of the members: url, next_url (NULL except for the first request of a 1D series),
 * symbol (a copy; set for quotes and series alike, used only by quotes), series (NULL until the
 * chart of a 1D series has been parsed), user_cancellable (a strong reference, nullable) and owner
 * (a strong reference; the Op counts in owner->inflight from op_new() to op_free()) are owned by
 * the Op. cancel_watch is owned
 * by the Op between enqueue() and the moment the task leaves the queue; the per-attempt members are
 * owned by the Op between attempt_start() and attempt_cleanup().
 *
 * Lifetime: every GSource and GAsyncReadyCallback of an operation holds its own reference to
 * the GTask as callback data (released when the callback has run or the source is finalised;
 * the user-cancel handler holds the attempt cancellable instead), and the queue holds one
 * reference per queued task, so the task, and with it the Op
 * and its owner, outlives every pending callback. That cycle (task -> Op -> source -> task) is
 * broken by source_clear() on cancel_watch (attempt_start(), pump() cancelled branch,
 * on_queued_cancelled()) and by attempt_cleanup() (fail_attempt(), on_read()), which every path
 * that ends a wait or an attempt runs before the task is completed or re-queued.
 *
 * Callback user_data: on_sent() and on_read() receive a task reference taken for the pending
 * call and release it; on_deadline() and on_queued_cancelled() receive the task reference held
 * as callback data of the deadline / cancel_watch source; on_user_cancelled() receives
 * attempt_cancel, referenced by the cancellable connection; on_pump_timer() receives the raw
 * StkYahoo pointer WITHOUT a reference (see pump()).
 */
typedef struct {
  OpType type;
  char *url;      /* URL of the current request */
  char *next_url; /* 1D series: the quote URL, until it becomes url; else NULL */
  char *symbol;   /* owned copy; used by quotes only */
  StkKind kind;   /* quote only */
  StkRange range; /* series only */
  StkSeries *series; /* 1D series: the parsed chart, while its previous close is fetched */
  gint64 day_ms;     /* 1D series: start of the chart's regular session */
  GCancellable *user_cancellable; /* the caller's (nullable), strong ref */
  StkYahoo *owner;                /* strong ref; this Op counts in owner->inflight */

  guint attempt;     /* 0 for the first attempt */
  guint http_status; /* HTTP status of the current attempt, 0 until known */

  /* While queued: wakes the task when the caller cancels. Owned reference; its callback data is
   * a task reference. NULL while the task is not queued. */
  GSource *cancel_watch;

  /* Per-attempt state, released by attempt_cleanup(). */
  SoupMessage *msg;
  GCancellable *attempt_cancel; /* cancelled by the deadline or by the user; the user_cancellable
                                 * handler holds a second reference as its data */
  gulong user_cancel_id;        /* handler on user_cancellable, 0 if none */
  GSource *deadline;            /* owned reference; its callback data is a task reference */
  gboolean timed_out;
  GInputStream *stream;
  GByteArray *body;  /* owned until moved into the GBytes handed to op_complete() */
  gsize body_filled; /* bytes of body->data already read; <= STK_YAHOO_MAX_BODY_BYTES + 1 */
  goffset declared;  /* Content-Length of an undecoded body, else -1 */
} Op;

static char tag_quote, tag_series, tag_search;

static void pump(StkYahoo *self);
static void enqueue(GTask *task, gboolean at_head);

/**
 * source_clear:
 * @source: (not nullable): location holding a GSource reference owned by the caller, or NULL
 *
 * Destroys the source stored at @source (if any) and drops the reference stored there.
 *
 * Preconditions:
 *  - @source is non-NULL and points to a variable that is NULL or holds one owned reference to a
 *    GSource.
 * Postconditions:
 *  - the source is removed from its context and will not be dispatched again (a dispatch already
 *    running completes); *@source is NULL. Dropping the reference releases the source's callback
 *    data (a task reference) when GLib finalises the source.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the reference stored at @source is released.
 */
static void
source_clear(GSource **source)
{
  if (*source != NULL) {
    g_source_destroy(*source);
    g_clear_pointer(source, g_source_unref);
  }
}

/**
 * attempt_cleanup:
 * @op: (not nullable): the operation
 *
 * Releases the per-attempt state of @op: the deadline source, the user-cancellation handler, the
 * attempt cancellable, the stream (closed asynchronously, which hands the connection back to the
 * pool), the message and the partial body.
 *
 * Preconditions:
 *  - @op is live (owned by its GTask). It may be called with no attempt running (everything
 *    NULL/0) and more than once.
 *  - It is not called from inside on_user_cancelled() (g_cancellable_disconnect() would wait for
 *    that handler): its callers are fail_attempt(), on_read() and op_free(), reached from I/O
 *    callbacks and from task finalisation. This relies on a cancelled read/send being completed
 *    from the main loop and not from inside g_cancellable_cancel(): the pre-existing design,
 *    exercised by the cancel tests, not something checked here.
 * Postconditions:
 *  - deadline, user_cancel_id, attempt_cancel, stream, msg and body are NULL/0 and body_filled is
 *    0. Dropping the deadline source also drops the task reference held by its callback data, and
 *    disconnecting the handler drops the attempt_cancel reference held by it. http_status,
 *    timed_out and declared keep their values.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @op stays with its task; every per-attempt resource owned by @op is released here.
 */
static void
attempt_cleanup(Op *op)
{
  source_clear(&op->deadline);
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

/**
 * op_free:
 * @data: (not nullable) (transfer full): the Op to free (a #Op, passed as gpointer)
 *
 * GDestroyNotify of the task data: releases every member of the Op and the owner reference.
 *
 * Preconditions:
 *  - @data is the Op created by op_new(), freed once, when its GTask has been finalised (so no
 *    source or callback can still reference the task: each of them holds a task reference that
 *    attempt_cleanup() / source_clear() release first, hence the clears below find nothing armed
 *    in practice and are defensive).
 *  - op->owner->inflight is > 0: op_new() incremented it once for this Op.
 * Postconditions:
 *  - all members are released; owner->inflight is decremented once; the owner reference taken in
 *    op_new() is dropped (which may finalise the provider). @data is invalid afterwards.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @data and of every member of the Op (url, next_url, symbol, series,
 *   user_cancellable ref, owner ref, cancel_watch and per-attempt state).
 */
static void
op_free(gpointer data)
{
  Op *op = data;
  attempt_cleanup(op);
  source_clear(&op->cancel_watch);
  g_clear_object(&op->user_cancellable);
  op->owner->inflight--;
  g_object_unref(op->owner);
  g_free(op->url);
  g_free(op->next_url);
  g_free(op->symbol);
  g_clear_pointer(&op->series, stk_series_unref);
  g_free(op);
}

/**
 * quote_destroy:
 * @quote: (not nullable) (transfer full): a #StkQuote, passed as gpointer
 *
 * GDestroyNotify of a quote task result that was never propagated.
 *
 * Preconditions:
 *  - @quote was returned by stk_yahoo_parse_quote() and is owned by the task result.
 * Postconditions:
 *  - the quote is freed with stk_quote_free().
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @quote.
 */
static void
quote_destroy(gpointer quote)
{
  stk_quote_free(quote);
}

/**
 * series_destroy:
 * @series: (not nullable) (transfer full): a #StkSeries, passed as gpointer
 *
 * GDestroyNotify of a series task result that was never propagated.
 *
 * Preconditions:
 *  - @series was returned by stk_yahoo_parse_series() and is owned by the task result.
 * Postconditions:
 *  - the reference is dropped with stk_series_unref().
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of one reference to @series.
 */
static void
series_destroy(gpointer series)
{
  stk_series_unref(series);
}

/**
 * op_complete:
 * @task: (not nullable): the operation's task; its task data is an Op
 * @body: (not nullable): the complete response body of the current request
 *
 * Parses the finished body and completes @task with the result (or the parse error), except after
 * the chart of a 1D series, whose previous close is requested next.
 *
 * Preconditions:
 *  - @task has an Op as task data that has not been returned yet; @body is a GBytes of at most
 *    STK_YAHOO_MAX_BODY_BYTES bytes (on_read() fails the attempt instead of completing when the
 *    body exceeds the cap; load_chart() re-checks the length). Nothing is on the wire and the
 *    caller holds a reference to @task for the duration of the call (on_read()).
 * Postconditions:
 *  - the chart of a 1D series parsed successfully: op->series holds it, op->day_ms its trading day,
 *    op->url is the former next_url (next_url is NULL), op->attempt is 0 and the task is queued at
 *    the head (enqueue() pumps; the task completes later).
 *  - otherwise @task is completed exactly once: with a quote (OP_QUOTE), a series (OP_SERIES; after
 *    the second request of a 1D series, the held chart with prev_close set from @body, NAN when
 *    @body has none or does not parse), or with the parser's error (every NULL return of the
 *    parsers sets the error). GTask may invoke the caller's callback synchronously from inside
 *    this call.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @task and @body stay with the caller; the result is handed to @task, which owns it
 *   until the caller propagates it.
 */
static void
op_complete(GTask *task, GBytes *body)
{
  Op *op = g_task_get_task_data(task);
  gsize len;
  const guint8 *data = g_bytes_get_data(body, &len);
  GError *error = NULL;

  if (op->type == OP_QUOTE) {
    StkQuote *quote = stk_yahoo_parse_quote(data, len, op->symbol, op->kind, &error);
    if (quote != NULL)
      g_task_return_pointer(task, quote, quote_destroy);
  } else if (op->series != NULL) {
    /* The second request of a 1D series. The chart was parsed here and is not shared yet, so its
     * prev_close may be written; a body without a usable previous close leaves it NAN. */
    op->series->prev_close = stk_yahoo_parse_prev_close(data, len, op->day_ms, NULL);
    g_task_return_pointer(task, g_steal_pointer(&op->series), series_destroy);
  } else {
    StkSeries *series = parse_series(data, len, op->range, &op->day_ms, &error);
    if (series != NULL && op->next_url != NULL) {
      op->series = series;
      g_free(op->url);
      op->url = g_steal_pointer(&op->next_url);
      op->attempt = 0; /* the second request has retries of its own */
      enqueue(task, TRUE);
    } else if (series != NULL) {
      g_task_return_pointer(task, series, series_destroy);
    }
  }
  if (error != NULL)
    g_task_return_error(task, error);
}

/**
 * http_status_error:
 * @status: the HTTP status of the response
 *
 * Maps a non-2xx HTTP status to the error the caller sees: 429 is STK_ERROR_RATE_LIMITED, 404 is
 * STK_ERROR_NOT_FOUND and everything else (including 400, which only the Nasdaq provider treats
 * as not found) is STK_ERROR_HTTP.
 *
 * Preconditions:
 *  - none (any value; soup_status_get_phrase() returns "Unknown Error" for unknown statuses).
 * Postconditions:
 *  - no state is changed; a new error is returned.
 * Thread affinity: any thread (pure)
 * Ownership: the returned error is owned by the caller.
 *
 * Returns: (transfer full): the new error, in the STK_ERROR domain.
 */
static GError *
http_status_error(guint status)
{
  if (status == HTTP_TOO_MANY_REQUESTS)
    return g_error_new(STK_ERROR, STK_ERROR_RATE_LIMITED, "HTTP %u: rate limited", status);
  if (status == SOUP_STATUS_NOT_FOUND)
    return g_error_new(STK_ERROR, STK_ERROR_NOT_FOUND, "HTTP %u: not found", status);
  return g_error_new(STK_ERROR, STK_ERROR_HTTP, "HTTP %u %s", status,
                     soup_status_get_phrase(status));
}

/**
 * normalize_error:
 * @op: (not nullable): the operation whose attempt failed
 * @err: (not nullable) (transfer full): the libsoup/GIO or STK_ERROR failure of the attempt
 *
 * Turns a failure of the current attempt into the error the caller sees: G_IO_ERROR_CANCELLED if
 * the caller's cancellable is cancelled, an STK_ERROR error unchanged, STK_ERROR_TIMEOUT for the
 * deadline or G_IO_ERROR_TIMED_OUT, else STK_ERROR_NETWORK.
 *
 * Preconditions:
 *  - @op is live; @err is non-NULL and owned by the caller, who passes that ownership on.
 * Postconditions:
 *  - @err is either returned (STK_ERROR domain, not cancelled) or freed; the result is never NULL.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @err; the returned error is owned by the caller; @op stays with
 *   its task.
 *
 * Returns: (transfer full): the normalised error.
 */
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

/**
 * fail_attempt:
 * @task: (not nullable): the operation's task, whose current attempt failed
 * @err: (not nullable) (transfer full): the failure of the attempt
 *
 * Ends the current attempt with @err: queues a retry when the failure class allows it (network,
 * timeout and HTTP 5xx, at most STK_YAHOO_MAX_RETRIES times per request), otherwise completes @task
 * with the normalised error; a final failure of the second request of a 1D series that is not a
 * cancellation completes @task with the held chart instead (prev_close NAN).
 *
 * Preconditions:
 *  - the attempt is in progress and its callback holds a task reference for the duration of the
 *    call (on_sent() / on_read() hold theirs through g_autoptr), so @task, its Op and the owner
 *    stay alive until this function returns, including across a synchronously invoked callback.
 * Postconditions:
 *  - the per-attempt state is released and on_wire is FALSE. On a retry op->attempt is incremented
 *    and the task is queued at the head (enqueue() pumps); otherwise the task is completed (with
 *    the error, or with the held chart as above) and pump() runs so the next operation can
 *    start.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @err; @task stays with the caller.
 */
static void
fail_attempt(GTask *task, GError *err)
{
  Op *op = g_task_get_task_data(task);
  StkYahoo *self = op->owner;
  GError *final = normalize_error(op, err);
  gboolean retryable = g_error_matches(final, STK_ERROR, STK_ERROR_NETWORK) ||
                       g_error_matches(final, STK_ERROR, STK_ERROR_TIMEOUT) ||
                       (g_error_matches(final, STK_ERROR, STK_ERROR_HTTP) &&
                        op->http_status >= 500 && op->http_status <= 599);
  attempt_cleanup(op);
  self->on_wire = FALSE;
  if (retryable && op->attempt < STK_YAHOO_MAX_RETRIES) {
    g_error_free(final);
    op->attempt++;
    enqueue(task, TRUE); /* pumps */
  } else if (op->series != NULL && !g_error_matches(final, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    /* The previous close of a 1D series is optional: the chart is delivered without it. */
    g_error_free(final);
    g_task_return_pointer(task, g_steal_pointer(&op->series), series_destroy);
    pump(self);
  } else {
    g_task_return_error(task, final);
    pump(self);
  }
}

/**
 * on_user_cancelled:
 * @user: (not nullable): the caller's cancellable that was cancelled
 * @attempt_cancel: (not nullable): the attempt cancellable, a #GCancellable passed as gpointer
 *
 * Cancellable handler: forwards the caller's cancellation to the current attempt.
 *
 * Preconditions:
 *  - @attempt_cancel is the reference taken in attempt_start() and owned by the connection; the
 *    handler is disconnected by attempt_cleanup() before the attempt's own reference is dropped,
 *    and the connection's reference keeps the object alive until the notify runs. @user is unused.
 * Postconditions:
 *  - the attempt cancellable is cancelled; the pending I/O callback then reports the cancellation.
 * Thread affinity: the thread that cancels the caller's cancellable (it only calls
 *   g_cancellable_cancel(), which is thread-safe)
 * Ownership: @user and @attempt_cancel stay owned by their holders; nothing is retained.
 */
static void
on_user_cancelled(GCancellable *user, gpointer attempt_cancel)
{
  (void)user;
  g_cancellable_cancel(G_CANCELLABLE(attempt_cancel));
}

/**
 * on_deadline:
 * @user_data: (not nullable): the operation's GTask (a GTask, passed as gpointer)
 *
 * GSource callback of the per-attempt deadline: marks the attempt as timed out and cancels it.
 *
 * Preconditions:
 *  - @user_data is the task reference held by the source's callback data, so the task and its Op
 *    are alive. attempt_cancel is non-NULL while the source is attached, because attempt_cleanup()
 *    destroys the source before it clears attempt_cancel.
 * Postconditions:
 *  - op->timed_out is TRUE and the attempt cancellable is cancelled (the pending I/O callback then
 *    fails the attempt); the source is removed (the source's destroy notify releases its task
 *    reference).
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @user_data stays owned by the source (released by its destroy notify).
 *
 * Returns: G_SOURCE_REMOVE.
 */
static gboolean
on_deadline(gpointer user_data)
{
  GTask *task = user_data;
  Op *op = g_task_get_task_data(task);
  op->timed_out = TRUE;
  g_cancellable_cancel(op->attempt_cancel); /* the pending I/O callback reports it */
  return G_SOURCE_REMOVE;
}

static void read_next(GTask *task);

/**
 * on_read:
 * @source: (not nullable): the response input stream, a #GInputStream passed as GObject
 * @result: (not nullable): the read result
 * @user_data: (transfer full): the operation's GTask reference taken in read_next(), released here
 *
 * GAsyncReadyCallback of a body read: accumulates the bytes, enforces the body cap and the
 * declared length, and either reads on, fails the attempt or completes the operation.
 *
 * Preconditions:
 *  - @source is op->stream and @result belongs to the read issued by read_next(); op->body is set
 *    and op->body_filled <= STK_YAHOO_MAX_BODY_BYTES (read_next() asked for at most cap + 1 -
 *    body_filled bytes, so body_filled grows to at most cap + 1 and is checked before use).
 * Postconditions:
 *  - exactly one of: another read (body_filled <= cap), a failed attempt (read error, body larger
 *    than the cap -> STK_ERROR_PARSE, or fewer bytes than the declared Content-Length ->
 *    STK_ERROR_NETWORK), or completion: the attempt state is released, on_wire is FALSE, the task
 *    is completed by op_complete() and pump() runs. The guint cast of body_filled is of a value
 *    <= cap (G_STATIC_ASSERT above).
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the task reference in @user_data is released on every path through the g_autoptr; the
 *   body is stolen into a GBytes that is released here.
 */
static void
on_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = user_data;
  Op *op = g_task_get_task_data(task);
  StkYahoo *self = op->owner;
  GError *err = NULL;
  gssize n = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &err);
  if (n < 0) {
    fail_attempt(task, err);
    return;
  }
  op->body_filled += (gsize)n;
  if (op->body_filled > STK_YAHOO_MAX_BODY_BYTES) {
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
  /* End of body: release the wire, then hand the body to the parser. */
  g_byte_array_set_size(op->body, (guint)op->body_filled);
  GBytes *body = g_byte_array_free_to_bytes(g_steal_pointer(&op->body));
  attempt_cleanup(op);
  self->on_wire = FALSE;
  op_complete(task, body);
  g_bytes_unref(body);
  pump(self);
}

/**
 * read_next:
 * @task: (not nullable): the operation's task
 *
 * Issues the next asynchronous body read into op->body, never asking for more than one byte
 * beyond the body cap, so an oversized body is detected without buffering it.
 *
 * Preconditions:
 *  - op->stream and op->body are set and op->body_filled <= STK_YAHOO_MAX_BODY_BYTES (on_sent()
 *    starts with 0; on_read() checks the cap before calling), so the chunk is >= 1 byte and
 *    body_filled + chunk <= cap + 1, which fits a guint (G_STATIC_ASSERT above).
 * Postconditions:
 *  - op->body is grown to body_filled + chunk bytes and a read of @chunk bytes is pending with a
 *    new reference to @task, passed to on_read().
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @task stays with the caller; the pending read owns the new task reference until
 *   on_read() releases it.
 */
static void
read_next(GTask *task)
{
  Op *op = g_task_get_task_data(task);
  /* Never ask for more than one byte beyond the cap, so an oversized body is
   * detected without buffering it. */
  gsize chunk = MIN((gsize)READ_CHUNK_BYTES, (gsize)STK_YAHOO_MAX_BODY_BYTES + 1 - op->body_filled);
  g_byte_array_set_size(op->body, (guint)(op->body_filled + chunk));
  g_input_stream_read_async(op->stream, op->body->data + op->body_filled, chunk,
                            G_PRIORITY_DEFAULT, op->attempt_cancel, on_read,
                            g_object_ref(task));
}

/**
 * on_sent:
 * @source: (not nullable): the SoupSession, passed as GObject
 * @result: (not nullable): the send result
 * @user_data: (transfer full): the operation's GTask reference taken in attempt_start(), released
 *   here
 *
 * GAsyncReadyCallback of soup_session_send_async(): checks the status and the declared length,
 * then starts reading the body.
 *
 * Preconditions:
 *  - @source is the session of the owner and @result belongs to the send issued by
 *    attempt_start(), so op->msg is set.
 * Postconditions:
 *  - a send error, a non-2xx status (mapped by http_status_error()) or a declared Content-Length
 *    above STK_YAHOO_MAX_BODY_BYTES (STK_ERROR_PARSE, before reading) fails the attempt. Otherwise
 *    op->stream is set, op->declared holds the Content-Length when no content coding applies (else
 *    -1), op->body is allocated (declared when it is > 0, which is <= cap and therefore fits a
 *    guint, else READ_CHUNK_BYTES) and the first read is issued.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the stream returned by libsoup becomes owned by the Op; the task reference in
 *   @user_data is released on every path.
 */
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
  if (declared > STK_YAHOO_MAX_BODY_BYTES) {
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

/**
 * attempt_start:
 * @task: (not nullable): the operation's task, just taken out of the queue
 *
 * Starts one attempt of the operation: builds the GET message (no redirects), arms the deadline and
 * the cancellation link and sends it.
 *
 * Preconditions:
 *  - @task has an Op with url set and no attempt state; the caller (pump()) holds a reference to
 *    @task for the duration of the call; nothing is on the wire and the minimum gap has elapsed;
 *    the source object of @task is the StkYahoo.
 * Postconditions:
 *  - cancel_watch is cleared. If libsoup rejects the URL the task is
 *    completed with STK_ERROR_INVALID_ARG
 *    ("malformed request URL"), on_wire stays FALSE, next_start_us is not advanced and nothing is
 *    armed; This branch is reachable: stk_yahoo_new_custom("https://exa%20mple.com", ...) accepts
 *    that origin, and a quote or series request for "^GSPC" then completes with
 *    STK_ERROR_INVALID_ARG "malformed request URL" (probed). this replaces the former g_assert().
 *    Otherwise on_wire is TRUE,
 *    next_start_us = now + min_gap_ms, and msg, attempt_cancel, deadline (attached to the task's
 *    context) and, when the caller supplied a cancellable, the user_cancel_id handler exist and a
 *    send is pending; the references taken for the callbacks are listed in Op.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @task stays with the caller; the new GSource and callbacks take their own task
 *   references; @op owns the new per-attempt objects.
 */
static void
attempt_start(GTask *task)
{
  Op *op = g_task_get_task_data(task);
  StkYahoo *self = op->owner;

  source_clear(&op->cancel_watch);

  /* op->url is an accepted origin plus an escaped symbol; libsoup may still reject the origin. */
  op->msg = soup_message_new(SOUP_METHOD_GET, op->url);
  if (op->msg == NULL) {
    g_task_return_error(task, g_error_new_literal(STK_ERROR, STK_ERROR_INVALID_ARG,
                                                  "malformed request URL"));
    return;
  }
  self->on_wire = TRUE;
  self->next_start_us = g_get_monotonic_time() + (gint64)self->min_gap_ms * 1000;
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

  /* The deadline starts now, at send time. */
  op->deadline = g_timeout_source_new(self->timeout_ms);
  g_source_set_callback(op->deadline, on_deadline, g_object_ref(task), g_object_unref);
  g_source_attach(op->deadline, g_task_get_context(task));

  soup_session_send_async(self->session, op->msg, G_PRIORITY_DEFAULT, op->attempt_cancel, on_sent,
                          g_object_ref(task));
}

/**
 * on_pump_timer:
 * @user_data: (not nullable): the StkYahoo, passed as gpointer, WITHOUT a reference
 *
 * GSource callback of the pacing timer: runs pump().
 *
 * Preconditions:
 *  - the instance is alive: the source is the one stored in self->pump_timer (pump() destroys the
 *    previous timer before arming a new one, so at most one exists), and stk_yahoo_dispose()
 *    destroys it before the instance can be finalised, which in turn needs every Op (each holds an
 *    owner reference) to be gone.
 * Postconditions:
 *  - pump() has run; the source is removed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @user_data is not owned by the source (no destroy notify, no reference).
 *
 * Returns: G_SOURCE_REMOVE.
 */
static gboolean
on_pump_timer(gpointer user_data)
{
  pump(STK_YAHOO(user_data));
  return G_SOURCE_REMOVE;
}

/**
 * pump:
 * @self: (not nullable): the provider whose queue is advanced
 *
 * Starts the queue head if nothing is on the wire and the gap since the last attempt start has
 * elapsed; otherwise arms pump_timer for the remaining time (or does nothing when an attempt is on
 * the wire: its end calls pump()). Idempotent: every state change that could allow a start calls
 * it.
 *
 * Preconditions:
 *  - @self is a live StkYahoo. pump() takes its own reference for the duration of the call: a
 *    completion callback invoked synchronously by g_task_return_*() may drop the last external
 *    reference, and the final unref of the head task (which releases the Op's owner reference)
 *    must not finalise @self while the loop still reads it. Callbacks may re-enter pump(),
 *    enqueue() and the fetch functions; the loop re-reads the state after each of them.
 * Postconditions:
 *  - at most one pacing timer is armed (self->pump_timer; a timer armed by a re-entrant call is
 *    destroyed before a new one is armed). Every queue head that is cancelled is completed with
 *    G_IO_ERROR_CANCELLED without traffic; otherwise the head is started with attempt_start().
 *    The loop ends when an attempt is on the wire, the queue is empty or the timer was armed. The
 *    timer delay is (next_start_us - now + 999) / 1000 milliseconds with now < next_start_us <=
 *    last start + 600000 ms, so it is positive and fits a guint.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the queue's reference to each popped task is dropped here; the new timer's callback
 *   data is @self without a reference (see on_pump_timer()).
 */
static void
pump(StkYahoo *self)
{
  g_autoptr(StkYahoo) keep_alive = g_object_ref(self);
  source_clear(&self->pump_timer);
  while (!self->on_wire && !g_queue_is_empty(&self->queue)) {
    GTask *head = g_queue_peek_head(&self->queue);
    Op *op = g_task_get_task_data(head);
    gboolean cancelled = op->user_cancellable != NULL && g_cancellable_is_cancelled(op->user_cancellable);
    gint64 now = g_get_monotonic_time();
    if (!cancelled && now < self->next_start_us) {
      source_clear(&self->pump_timer); /* a re-entrant pump() may have armed one meanwhile */
      self->pump_timer = g_timeout_source_new((guint)((self->next_start_us - now + 999) / 1000));
      g_source_set_callback(self->pump_timer, on_pump_timer, self, NULL);
      g_source_attach(self->pump_timer, g_task_get_context(head));
      return;
    }
    g_queue_pop_head(&self->queue); /* the queue's reference is now ours */
    if (cancelled) {
      /* The cancel watch has not been dispatched yet: finish here, no traffic. */
      source_clear(&op->cancel_watch);
      g_task_return_error(head, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                    "operation was cancelled"));
    } else {
      attempt_start(head);
    }
    g_object_unref(head);
  }
}

/**
 * on_queued_cancelled:
 * @user: (not nullable): the caller's cancellable that was cancelled
 * @user_data: (not nullable): the operation's GTask (a GTask, passed as gpointer)
 *
 * GCancellableSourceFunc of Op.cancel_watch: completes a queued task with G_IO_ERROR_CANCELLED
 * without sending anything.
 *
 * Preconditions:
 *  - @user_data is the task reference held by the cancel_watch source's callback data; the task
 *    is queued (cancel_watch is cleared by every path that removes it from the queue, so the
 *    source cannot fire for a task that has left the queue). @user is unused.
 * Postconditions:
 *  - the task is removed from the queue (the queue's reference is dropped), cancel_watch is
 *    cleared, the task is completed with G_IO_ERROR_CANCELLED and pump() runs. A local reference
 *    keeps the task, its Op and the owner alive until the function returns.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @user and @user_data stay owned by the source; the function takes and releases its
 *   own task reference.
 *
 * Returns: G_SOURCE_REMOVE.
 */
static gboolean
on_queued_cancelled(GCancellable *user, gpointer user_data)
{
  (void)user;
  g_autoptr(GTask) task = g_object_ref(user_data);
  Op *op = g_task_get_task_data(task);
  StkYahoo *self = op->owner;
  if (g_queue_remove(&self->queue, task))
    g_object_unref(task); /* the queue's reference */
  source_clear(&op->cancel_watch);
  g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                "operation was cancelled"));
  pump(self);
  return G_SOURCE_REMOVE;
}

/**
 * enqueue:
 * @task: (not nullable): the operation's task, which must not be queued
 * @at_head: TRUE to put the task at the head of the queue (a retry), FALSE at the tail
 *
 * Puts @task into the queue and lets the pacing logic start it when its turn has come. While
 * queued, a cancellation of the caller's cancellable completes the task at once.
 *
 * Preconditions:
 *  - @task has an Op, is not in the queue and has no cancel_watch (attempt_start() or the pump()
 *    cancelled branch cleared it); the caller holds a reference to @task for the duration of the
 *    call.
 * Postconditions:
 *  - the queue holds one new reference to @task; if the caller supplied a cancellable, cancel_watch
 *    is attached to the task's context with its own task reference; pump() has run.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @task stays with the caller; the queue and the cancel watch take their own
 *   references.
 */
static void
enqueue(GTask *task, gboolean at_head)
{
  Op *op = g_task_get_task_data(task);
  StkYahoo *self = op->owner;
  if (at_head)
    g_queue_push_head(&self->queue, g_object_ref(task));
  else
    g_queue_push_tail(&self->queue, g_object_ref(task));
  if (op->user_cancellable != NULL) {
    op->cancel_watch = g_cancellable_source_new(op->user_cancellable);
    g_source_set_callback(op->cancel_watch, G_SOURCE_FUNC(on_queued_cancelled), g_object_ref(task),
                          g_object_unref);
    g_source_attach(op->cancel_watch, g_task_get_context(task));
  }
  pump(self);
}

/* ------------------------------------------------------------------------ */
/* StkProvider glue                                                         */
/* ------------------------------------------------------------------------ */

/**
 * op_new:
 * @owner: (not nullable): the provider
 * @type: the operation type
 * @url: (transfer full): the request URL, taken
 * @next_url: (transfer full) (nullable): the URL of a second request (1D series: the quote URL),
 *   taken
 * @symbol: (not nullable): the symbol, copied
 * @kind: the requested kind
 * @range: the requested range
 * @user_cancellable: (nullable): the caller's cancellable
 *
 * Creates the task data of an operation and counts it in owner->inflight.
 *
 * Preconditions:
 *  - @owner is a live StkYahoo; @url is a g_malloc()-allocated string; @symbol is NUL-terminated
 *    (the callers pass a symbol that stk_yahoo_chart_url() accepted).
 * Postconditions:
 *  - the new Op holds references to @owner and @user_cancellable and owns @url, @next_url and the
 *    symbol copy; owner->inflight is incremented once (undone by op_free()).
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @url and @next_url; copies @symbol; takes references on @owner and
 *   @user_cancellable; the returned Op is owned by the caller (op_start() hands it to the task).
 *
 * Returns: (transfer full): the new Op.
 */
static Op *
op_new(StkYahoo *owner, OpType type, char *url, char *next_url, const char *symbol, StkKind kind,
       StkRange range, GCancellable *user_cancellable)
{
  Op *op = g_new0(Op, 1);
  op->type = type;
  op->url = url;
  op->next_url = next_url;
  op->symbol = g_strdup(symbol);
  op->kind = kind;
  op->range = range;
  op->user_cancellable = user_cancellable != NULL ? g_object_ref(user_cancellable) : NULL;
  op->owner = g_object_ref(owner);
  owner->inflight++;
  return op;
}

/**
 * admit:
 * @task: (not nullable): the operation's task
 * @self: (not nullable): the provider
 * @url: (transfer full) (nullable): the request URL, or NULL if the arguments were rejected
 * @url_error: (transfer full) (nullable): the error for a NULL @url
 *
 * Admission control. Tells whether the operation may start. Otherwise @task is completed with the
 * error (@url_error for a rejected argument when @url is NULL; STK_ERROR_RATE_LIMITED when the
 * in-flight cap would be exceeded), @url is freed and no network traffic happens.
 *
 * Preconditions:
 *  - @url is NULL exactly when stk_yahoo_chart_url() failed, in which case it set @url_error
 *    (callers pass the error variable they gave it, initialised to NULL, so @url_error is non-NULL
 *    iff @url is NULL).
 * Postconditions:
 *  - on TRUE nothing is consumed. On FALSE @task has been completed with the error and @url /
 *    @url_error were consumed; the caller then drops its reference to @task.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: consumes @url and @url_error on FALSE; @task and @self stay with the caller.
 *
 * Returns: TRUE if the operation may start, FALSE if it was rejected.
 */
static gboolean
admit(GTask *task, StkYahoo *self, char *url, GError *url_error)
{
  if (url == NULL) {
    g_task_return_error(task, url_error);
    return FALSE;
  }
  if (self->inflight + 1 > STK_YAHOO_MAX_INFLIGHT) {
    g_free(url);
    g_task_return_new_error(task, STK_ERROR, STK_ERROR_RATE_LIMITED,
                            "too many outstanding requests (limit %d)", STK_YAHOO_MAX_INFLIGHT);
    return FALSE;
  }
  return TRUE;
}

/**
 * op_start:
 * @task: (transfer full): the operation's task, the caller's reference is consumed
 * @type: the operation type
 * @url: (transfer full): the admitted request URL
 * @next_url: (transfer full) (nullable): the URL of a second request (see op_new())
 * @symbol: (not nullable): the symbol, copied
 * @kind: the requested kind
 * @range: the requested range
 *
 * Starts one admitted operation as @task: attaches the Op as task data and queues it (a task whose
 * caller's cancellable is already cancelled is completed with G_IO_ERROR_CANCELLED instead).
 *
 * Preconditions:
 *  - admit() returned TRUE for @task; the source object of @task is the StkYahoo; the caller does
 *    not use @task afterwards.
 * Postconditions:
 *  - the Op (see op_new()) is the task data of @task and owner->inflight was incremented; the task
 *    is queued or completed with the cancellation error; the caller's reference to @task is
 *    dropped.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @url, @next_url and of the caller's reference to @task; @symbol
 *   stays with the caller.
 */
static void
op_start(GTask *task, OpType type, char *url, char *next_url, const char *symbol, StkKind kind,
         StkRange range)
{
  StkYahoo *self = STK_YAHOO(g_task_get_source_object(task));
  g_task_set_task_data(
      task, op_new(self, type, url, next_url, symbol, kind, range, g_task_get_cancellable(task)),
      op_free);
  if (!g_task_return_error_if_cancelled(task))
    enqueue(task, FALSE);
  g_object_unref(task);
}

/**
 * yahoo_fetch_quote_async:
 * @provider: (not nullable): the StkYahoo
 * @symbol: (not nullable): the requested symbol
 * @kind: the requested kind
 * @cancellable: (nullable): the caller's cancellable
 * @callback: (scope async): called once with the result
 * @user_data: passed to @callback
 *
 * StkProviderInterface.fetch_quote_async: starts a quote request (see
 * stk_provider_fetch_quote_async()). A quote is derived from the quote URL's response only.
 *
 * Preconditions:
 *  - @provider is a StkYahoo (it is reached only through that class's StkProvider vtable);
 *    @symbol is non-NULL (checked by the dispatcher) and NUL-terminated (caller obligation,
 *    not checked).
 * Postconditions:
 *  - @callback is called once. An invalid symbol/kind completes the task with STK_ERROR_INVALID_ARG
 *    and an over-cap request with STK_ERROR_RATE_LIMITED, with no network traffic; otherwise the
 *    operation is queued.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the task takes references on @provider and @cancellable; @symbol is copied; @user_data
 *   is only passed on.
 */
static void
yahoo_fetch_quote_async(StkProvider *provider, const char *symbol, StkKind kind,
                        GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  StkYahoo *self = STK_YAHOO(provider);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, &tag_quote);
  GError *error = NULL;
  char *url = stk_yahoo_quote_url(self->base_url, symbol, kind, &error);
  if (admit(task, self, url, error))
    op_start(task, OP_QUOTE, url, NULL, symbol, kind, STK_RANGE_1D);
  else
    g_object_unref(task);
}

/**
 * yahoo_fetch_quote_finish:
 * @provider: (not nullable): the StkYahoo the request was started on
 * @result: (not nullable): the result passed to the callback
 * @error: (nullable): return location for a #GError
 *
 * StkProviderInterface.fetch_quote_finish.
 *
 * Preconditions:
 *  - @result is a GTask created by yahoo_fetch_quote_async() for @provider and not yet finished
 *    (checked with g_return_val_if_fail()).
 * Postconditions:
 *  - on failure NULL is returned and *@error is set; the task result is consumed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the quote is returned to the caller; @provider and @result stay with the caller.
 *
 * Returns: (transfer full) (nullable): the quote, or NULL with @error set.
 */
static StkQuote *
yahoo_fetch_quote_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_quote, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/**
 * yahoo_fetch_series_async:
 * @provider: (not nullable): the StkYahoo
 * @symbol: (not nullable): the requested symbol
 * @kind: the requested kind
 * @range: the requested range
 * @cancellable: (nullable): the caller's cancellable
 * @callback: (scope async): called once with the result
 * @user_data: passed to @callback
 *
 * StkProviderInterface.fetch_series_async: starts a series request (see
 * stk_provider_fetch_series_async()). A 1D series is the chart request followed by the quote
 * request, whose daily closes give the chart's previous close (see op_complete()).
 *
 * Preconditions:
 *  - @provider is a StkYahoo (it is reached only through that class's StkProvider vtable);
 *    @symbol is non-NULL (checked by the dispatcher) and NUL-terminated (caller obligation,
 *    not checked).
 * Postconditions:
 *  - @callback is called once. An invalid symbol/kind/range completes the task with
 *    STK_ERROR_INVALID_ARG and an over-cap request with STK_ERROR_RATE_LIMITED, with no network
 *    traffic; otherwise the operation is queued.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the task takes references on @provider and @cancellable; @symbol is copied; @user_data
 *   is only passed on.
 */
static void
yahoo_fetch_series_async(StkProvider *provider, const char *symbol, StkKind kind, StkRange range,
                         GCancellable *cancellable, GAsyncReadyCallback callback,
                         gpointer user_data)
{
  StkYahoo *self = STK_YAHOO(provider);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, &tag_series);
  GError *error = NULL;
  char *url = stk_yahoo_chart_url(self->base_url, symbol, kind, range, &error);
  if (admit(task, self, url, error)) {
    /* The symbol and kind were just accepted, so the quote URL cannot fail. */
    char *next_url = range == STK_RANGE_1D ? stk_yahoo_quote_url(self->base_url, symbol, kind, NULL)
                                           : NULL;
    op_start(task, OP_SERIES, url, next_url, symbol, kind, range);
  }
  else
    g_object_unref(task);
}

/**
 * yahoo_fetch_series_finish:
 * @provider: (not nullable): the StkYahoo the request was started on
 * @result: (not nullable): the result passed to the callback
 * @error: (nullable): return location for a #GError
 *
 * StkProviderInterface.fetch_series_finish.
 *
 * Preconditions:
 *  - @result is a GTask created by yahoo_fetch_series_async() for @provider and not yet finished
 *    (checked with g_return_val_if_fail()).
 * Postconditions:
 *  - on failure NULL is returned and *@error is set; the task result is consumed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the series is returned to the caller; @provider and @result stay with the caller.
 *
 * Returns: (transfer full) (nullable): the series, or NULL with @error set.
 */
static StkSeries *
yahoo_fetch_series_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_series, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/**
 * hits_destroy:
 * @hits: (not nullable) (transfer full): a #GPtrArray of #StkSearchHit, passed as gpointer
 *
 * GDestroyNotify of a search task result that was never propagated.
 *
 * Preconditions:
 *  - @hits is the array created by yahoo_search_async() and owned by the task result.
 * Postconditions:
 *  - the reference is dropped; the array and its hits are freed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of one reference to @hits.
 */
static void
hits_destroy(gpointer hits)
{
  g_ptr_array_unref(hits);
}

/**
 * yahoo_search_async:
 * @provider: (not nullable): the StkYahoo
 * @text: (not nullable): the search text, not inspected
 * @cancellable: (nullable): the caller's cancellable
 * @callback: (scope async): called once with the result
 * @user_data: passed to @callback
 *
 * StkProviderInterface.search_async: completes with an empty array; the search endpoint is not
 * used and the network is never touched (see stk-yahoo.h).
 *
 * Preconditions:
 *  - @provider is a StkYahoo (checked by the interface dispatcher); @text is unused (the
 *    dispatcher requires it to be non-NULL).
 * Postconditions:
 *  - the task is completed once with an empty array of search hits.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the task takes references on @provider and @cancellable; @text is not retained;
 *   @user_data is only passed on.
 */
static void
yahoo_search_async(StkProvider *provider, const char *text, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer user_data)
{
  (void)text; /* no search endpoint is used: see stk-yahoo.h */
  GTask *task = g_task_new(provider, cancellable, callback, user_data);
  g_task_set_source_tag(task, &tag_search);
  g_task_return_pointer(task, g_ptr_array_new_with_free_func((GDestroyNotify)stk_search_hit_free),
                        hits_destroy);
  g_object_unref(task);
}

/**
 * yahoo_search_finish:
 * @provider: (not nullable): the StkYahoo the request was started on
 * @result: (not nullable): the result passed to the callback
 * @error: (nullable): return location for a #GError
 *
 * StkProviderInterface.search_finish.
 *
 * Preconditions:
 *  - @result is a GTask created by yahoo_search_async() for @provider and not yet finished
 *    (checked with g_return_val_if_fail()).
 * Postconditions:
 *  - on failure NULL is returned and *@error is set; the task result is consumed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the array is returned to the caller; @provider and @result stay with the caller.
 *
 * Returns: (transfer full) (nullable) (element-type StkSearchHit): the hits, or NULL with @error
 *   set.
 */
static GPtrArray *
yahoo_search_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_search, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/**
 * stk_yahoo_provider_init:
 * @iface: (not nullable): the interface vtable of StkYahoo
 *
 * GObject interface initialiser: wires the six StkProviderInterface virtual functions.
 *
 * Preconditions:
 *  - called once by the GType system; @iface is writable.
 * Postconditions:
 *  - all six virtual functions are set.
 * Thread affinity: any thread (called by GObject under its type lock).
 * Ownership: @iface is owned by the type system and not retained.
 */
static void
stk_yahoo_provider_init(StkProviderInterface *iface)
{
  iface->fetch_quote_async = yahoo_fetch_quote_async;
  iface->fetch_quote_finish = yahoo_fetch_quote_finish;
  iface->fetch_series_async = yahoo_fetch_series_async;
  iface->fetch_series_finish = yahoo_fetch_series_finish;
  iface->search_async = yahoo_search_async;
  iface->search_finish = yahoo_search_finish;
}
