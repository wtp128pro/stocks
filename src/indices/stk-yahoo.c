/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-yahoo.c - StkProvider implementation for the Yahoo Finance chart
 * endpoint (market indices only). See stk-yahoo.h for the behaviour contract.
 *
 * Layout: URL builder, JSON helpers, response parsers (pure), the StkYahoo
 * object, then the request layer: one GTask per operation, one shared FIFO
 * queue that paces the attempts (at most one on the wire, a minimum gap
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

/* ------------------------------------------------------------------------ */
/* URL builder                                                              */
/* ------------------------------------------------------------------------ */

static gboolean
range_is_valid(StkRange range)
{
  return range >= STK_RANGE_1D && range <= STK_RANGE_1Y;
}

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
  g_autofree char *upper = g_ascii_strup(symbol, -1);
  g_autofree char *escaped = g_uri_escape_string(upper, NULL, FALSE);
  return g_strdup_printf("%s/v8/finance/chart/%s?range=%s&interval=%s", base_url, escaped,
                         range_param[range], interval_param[range]);
}

/* ------------------------------------------------------------------------ */
/* JSON helpers (every accessor tolerates missing / null / mistyped nodes)  */
/* ------------------------------------------------------------------------ */

static JsonObject *
member_object(JsonObject *obj, const char *name)
{
  JsonNode *node = obj != NULL ? json_object_get_member(obj, name) : NULL;
  return (node != NULL && JSON_NODE_HOLDS_OBJECT(node)) ? json_node_get_object(node) : NULL;
}

static JsonArray *
member_array(JsonObject *obj, const char *name)
{
  JsonNode *node = obj != NULL ? json_object_get_member(obj, name) : NULL;
  return (node != NULL && JSON_NODE_HOLDS_ARRAY(node)) ? json_node_get_array(node) : NULL;
}

static const char *
member_string(JsonObject *obj, const char *name)
{
  JsonNode *node = obj != NULL ? json_object_get_member(obj, name) : NULL;
  if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string(node);
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
  return obj != NULL && node_number(json_object_get_member(obj, name), out);
}

/* Epoch seconds inside the supported window, else FALSE. */
static gboolean
node_epoch_s(JsonNode *node, gint64 *out)
{
  double d;
  if (!node_number(node, &d) || d < (double)EPOCH_MIN_S || d >= (double)EPOCH_MAX_S)
    return FALSE;
  *out = (gint64)d;
  return TRUE;
}

/* Copy of at most the first @max_chars characters of valid UTF-8 @text, so a
 * server message cannot bloat an error message or split a UTF-8 sequence. */
static char *
truncated_copy(const char *text, glong max_chars)
{
  const char *end = text;
  for (glong i = 0; i < max_chars && *end != '\0'; i++)
    end = g_utf8_next_char(end);
  return g_strndup(text, (gsize)(end - text));
}

/*
 * Validates @data as a UTF-8 JSON chart envelope and returns the parser (owner
 * of the objects). *result is chart.result[0] and *meta its "meta" object.
 * chart.error (any non-null value) is NOT_FOUND.
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

/* meta.previousClose, else meta.chartPreviousClose: finite and > 0, else NAN. */
static double
previous_close_of(JsonObject *meta)
{
  static const char *const names[] = {"previousClose", "chartPreviousClose"};
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    double p;
    if (member_number(meta, names[i], &p) && p > 0.0)
      return p;
  }
  return NAN;
}

/* A display name: valid UTF-8, at most NAME_MAX_BYTES, no control characters. */
static gboolean
name_is_usable(const char *name)
{
  if (name == NULL || name[0] == '\0' || strlen(name) > NAME_MAX_BYTES ||
      !g_utf8_validate(name, -1, NULL))
    return FALSE;
  for (const char *p = name; *p != '\0'; p = g_utf8_next_char(p))
    if (g_unichar_iscntrl(g_utf8_get_char(p)))
      return FALSE;
  return TRUE;
}

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
      !(strlen(currency) == 3 && g_ascii_isupper(currency[0]) && g_ascii_isupper(currency[1]) &&
        g_ascii_isupper(currency[2])))
    currency = NULL;
  const char *name = member_string(meta, "longName");
  if (!name_is_usable(name))
    name = member_string(meta, "shortName");
  if (!name_is_usable(name))
    name = NULL;

  StkQuote *quote = stk_quote_new(symbol, name, kind, currency);
  quote->price = price;
  double prev = previous_close_of(meta);
  if (!isnan(prev)) {
    double change = price - prev;
    double pct = change / prev * 100.0;
    if (isfinite(change) && isfinite(pct)) { /* a tiny previous close can overflow */
      quote->change = change;
      quote->change_pct = pct;
    }
  }
  gint64 as_of_s;
  if (node_epoch_s(json_object_get_member(meta, "regularMarketTime"), &as_of_s))
    quote->as_of_ms = as_of_s * 1000;
  return quote;
}

StkSeries *
stk_yahoo_parse_series(const guint8 *data, gsize len, StkRange range, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
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
  double prev_close = NAN;
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
    prev_close = previous_close_of(meta);
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

  StkSeries *series = stk_series_new(range, prev_close, points->len);
  for (guint i = 0; i < points->len; i++) {
    const StkPoint *p = &g_array_index(points, StkPoint, i);
    stk_series_append(series, p->t_ms, p->v);
  }
  return series;
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

G_DEFINE_TYPE_WITH_CODE(StkYahoo, stk_yahoo, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(STK_TYPE_PROVIDER, stk_yahoo_provider_init))

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

static void
stk_yahoo_finalize(GObject *object)
{
  StkYahoo *self = STK_YAHOO(object);
  g_free(self->base_url);
  G_OBJECT_CLASS(stk_yahoo_parent_class)->finalize(object);
}

static void
stk_yahoo_class_init(StkYahooClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = stk_yahoo_dispose;
  object_class->finalize = stk_yahoo_finalize;
}

StkYahoo *
stk_yahoo_new(void)
{
  return g_object_new(STK_TYPE_YAHOO, NULL);
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

/* Task data of one operation: what to parse and the state of the request. */
typedef struct {
  OpType type;
  char *url;
  char *symbol;   /* quote only */
  StkKind kind;   /* quote only */
  StkRange range; /* series only */
  GCancellable *user_cancellable; /* the caller's (nullable), strong ref */
  StkYahoo *owner;                /* strong ref; this Op counts in owner->inflight */

  guint attempt;     /* 0 for the first attempt */
  guint http_status; /* HTTP status of the current attempt, 0 until known */

  /* While queued: wakes the task when the caller cancels. */
  GSource *cancel_watch;

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
} Op;

static char tag_quote, tag_series, tag_search;

static void pump(StkYahoo *self);

static void
source_clear(GSource **source)
{
  if (*source != NULL) {
    g_source_destroy(*source);
    g_clear_pointer(source, g_source_unref);
  }
}

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

/* Parses the finished body and completes @task with the result. */
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
  } else {
    StkSeries *series = stk_yahoo_parse_series(data, len, op->range, &error);
    if (series != NULL)
      g_task_return_pointer(task, series, series_destroy);
  }
  if (error != NULL)
    g_task_return_error(task, error);
}

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

/* Puts @task (which must not be queued) into the queue and lets the pacing
 * logic start it when its turn has come. While queued, a cancellation of the
 * caller's cancellable completes the task at once. */
static void
enqueue(GTask *task, gboolean at_head);

/* Ends the current attempt with @err: queues a retry when the failure class
 * allows it, otherwise completes @task with the error. */
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
  } else {
    g_task_return_error(task, final);
    pump(self);
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

/* Sends the next attempt of @task (just taken out of the queue; the caller
 * keeps its reference). Must only be called when nothing is on the wire and
 * the minimum gap has elapsed. */
static void
attempt_start(GTask *task)
{
  Op *op = g_task_get_task_data(task);
  StkYahoo *self = op->owner;

  source_clear(&op->cancel_watch);
  self->on_wire = TRUE;
  self->next_start_us = g_get_monotonic_time() + (gint64)self->min_gap_ms * 1000;

  /* op->url is built from a validated origin and an escaped symbol. */
  op->msg = soup_message_new(SOUP_METHOD_GET, op->url);
  g_assert(op->msg != NULL);
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

static gboolean
on_pump_timer(gpointer user_data)
{
  pump(STK_YAHOO(user_data));
  return G_SOURCE_REMOVE;
}

/*
 * Starts the queue head if nothing is on the wire and the gap since the last
 * attempt start has elapsed; otherwise arms pump_timer for the remaining time
 * (or does nothing when an attempt is on the wire: its end calls pump()).
 * Idempotent: every state change that could allow a start calls it.
 */
static void
pump(StkYahoo *self)
{
  source_clear(&self->pump_timer);
  while (!self->on_wire && !g_queue_is_empty(&self->queue)) {
    GTask *head = g_queue_peek_head(&self->queue);
    Op *op = g_task_get_task_data(head);
    gboolean cancelled = op->user_cancellable != NULL && g_cancellable_is_cancelled(op->user_cancellable);
    gint64 now = g_get_monotonic_time();
    if (!cancelled && now < self->next_start_us) {
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

static Op *
op_new(StkYahoo *owner, OpType type, char *url, const char *symbol, StkKind kind, StkRange range,
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
 * Admission control. Returns TRUE if the operation may start. Otherwise @task
 * is completed with the error (@url_error, taken, for a rejected argument
 * when @url is NULL; STK_ERROR_RATE_LIMITED when the in-flight cap would be
 * exceeded), @url (taken) is freed, no network traffic happens and FALSE is
 * returned; the caller then drops its reference.
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

/* Starts one admitted operation as @task. Takes @url and the caller's
 * reference to @task. */
static void
op_start(GTask *task, OpType type, char *url, const char *symbol, StkKind kind, StkRange range)
{
  StkYahoo *self = STK_YAHOO(g_task_get_source_object(task));
  g_task_set_task_data(task,
                       op_new(self, type, url, symbol, kind, range, g_task_get_cancellable(task)),
                       op_free);
  if (!g_task_return_error_if_cancelled(task))
    enqueue(task, FALSE);
  g_object_unref(task);
}

static void
yahoo_fetch_quote_async(StkProvider *provider, const char *symbol, StkKind kind,
                        GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  StkYahoo *self = STK_YAHOO(provider);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, &tag_quote);
  GError *error = NULL;
  /* A quote is derived from the range=1d chart response only. */
  char *url = stk_yahoo_chart_url(self->base_url, symbol, kind, STK_RANGE_1D, &error);
  if (admit(task, self, url, error))
    op_start(task, OP_QUOTE, url, symbol, kind, STK_RANGE_1D);
  else
    g_object_unref(task);
}

static StkQuote *
yahoo_fetch_quote_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_quote, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

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
  if (admit(task, self, url, error))
    op_start(task, OP_SERIES, url, symbol, kind, range);
  else
    g_object_unref(task);
}

static StkSeries *
yahoo_fetch_series_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_series, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
hits_destroy(gpointer hits)
{
  g_ptr_array_unref(hits);
}

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

static GPtrArray *
yahoo_search_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_search, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

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
