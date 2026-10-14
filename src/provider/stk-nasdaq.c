/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-nasdaq.c - StkProvider implementation for the Nasdaq web API.
 * See stk-nasdaq.h for the behaviour contract.
 *
 * Layout: URL builders, JSON helpers, response parsers (pure), then the
 * request layer (one GTask per request, one libsoup message per attempt; a 1D series joins two
 * requests under a third task)
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
/* Longest accepted external company name, bytes (same bound as StkYahoo). */
#define NAME_MAX_BYTES 256
/* Upper bound of the backoff_base_ms argument of stk_nasdaq_new_custom(). */
#define BACKOFF_BASE_MAX_MS 600000

/* Size arithmetic of the request layer: the body buffer is sized with guint
 * (GByteArray), so cap + 1 must fit; schedule_retry() shifts the back-off base
 * left by at most STK_NASDAQ_MAX_RETRIES - 1 and casts the result to guint. */
G_STATIC_ASSERT((guint64)STK_NASDAQ_MAX_BODY_BYTES + 1 <= G_MAXUINT);
G_STATIC_ASSERT(((guint64)BACKOFF_BASE_MAX_MS << STK_NASDAQ_MAX_RETRIES) <= G_MAXUINT);
/* libsoup has no constant for 429. */
#define HTTP_TOO_MANY_REQUESTS 429

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

/**
 * kind_is_valid:
 * @kind: any StkKind value (or an out-of-range integer)
 *
 * Tells whether @kind is served by this provider (stock, ETF or mutual fund; STK_KIND_INDEX and
 * out-of-range values are not).
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure)
 * Ownership: none (no pointers).
 *
 * Returns: TRUE for STK_KIND_STOCK, STK_KIND_ETF and STK_KIND_FUND, FALSE otherwise.
 */
static gboolean
kind_is_valid(StkKind kind)
{
  return kind == STK_KIND_STOCK || kind == STK_KIND_ETF || kind == STK_KIND_FUND;
}

/**
 * range_is_valid:
 * @range: any StkRange value (or an out-of-range integer)
 *
 * Tells whether @range is a StkRange enumerator (relies on the enumerators STK_RANGE_1D ..
 * STK_RANGE_1Y being contiguous and in that order).
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
 * assetclass_of:
 * @kind: a kind accepted by kind_is_valid()
 *
 * Maps a kind to the "assetclass" query value of the Nasdaq API.
 *
 * Preconditions:
 *  - kind_is_valid(@kind): the callers (stk_nasdaq_quote_url(), stk_nasdaq_series_url()) call it
 *    only after check_symbol_kind() succeeded.
 * Postconditions:
 *  - no state is changed. For an invalid @kind (never happens through the callers)
 *    g_return_val_if_reached() logs a critical and returns NULL; this replaces the former
 *    g_assert_not_reached().
 * Thread affinity: any thread (pure)
 * Ownership: the returned string is a static literal; nothing is allocated.
 *
 * Returns: "stocks", "etf" or "mutualfunds"; NULL (with a critical) for an invalid @kind.
 */
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
    g_return_val_if_reached(NULL);
  }
}

/**
 * months_of:
 * @range: one of STK_RANGE_1M, STK_RANGE_3M, STK_RANGE_6M, STK_RANGE_1Y
 *
 * Maps a daily range to the number of calendar months it covers.
 *
 * Preconditions:
 *  - @range is not STK_RANGE_1D and range_is_valid(): the only caller, stk_nasdaq_series_url(),
 *    returns for invalid ranges and for STK_RANGE_1D before calling it.
 * Postconditions:
 *  - no state is changed. For any other value (never happens through the caller)
 *    g_return_val_if_reached() logs a critical and returns 0; this replaces the former
 *    g_assert_not_reached().
 * Thread affinity: any thread (pure)
 * Ownership: none (no pointers).
 *
 * Returns: 1, 3, 6 or 12; 0 (with a critical) for an unsupported @range.
 */
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
    g_return_val_if_reached(0);
  }
}

/**
 * historical_limit_of:
 * @range: one of STK_RANGE_1M, STK_RANGE_3M, STK_RANGE_6M, STK_RANGE_1Y
 *
 * Maps a daily range to the "limit" parameter of the /historical endpoint, which is above the
 * number of trading days in the window.
 *
 * Preconditions:
 *  - @range is not STK_RANGE_1D and range_is_valid(): the only caller, stk_nasdaq_series_url(),
 *    reaches it only after the same checks as for months_of().
 * Postconditions:
 *  - no state is changed. For any other value (never happens through the caller)
 *    g_return_val_if_reached() logs a critical and returns 0; this replaces the former
 *    g_assert_not_reached().
 * Thread affinity: any thread (pure)
 * Ownership: none (no pointers).
 *
 * Returns: 40, 100, 200 or 400; 0 (with a critical) for an unsupported @range.
 */
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
    g_return_val_if_reached(0);
  }
}

/**
 * new_york_tz:
 *
 * Returns the America/New_York time zone, created on first use (falling back to a POSIX rule string
 * if the tz database lacks the name).
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - the zone is created at most once per process (GOnce) and is never freed. If neither identifier
 *    can be resolved NULL is returned on every call; this replaces the former g_assert(tz != NULL)
 *    and the only caller, parse_quote_timestamp(), then reports the timestamp as unparsable.
 * Thread affinity: any thread (initialisation is serialised by g_once_init_enter(); the zone is
 *   only read afterwards).
 * Ownership: the returned GTimeZone is owned by this function (process lifetime); callers must not
 *   unref it.
 *
 * Returns: the shared zone, or NULL if it cannot be created.
 */
static GTimeZone *
new_york_tz(void)
{
  static gsize tz_once = 0;
  static GTimeZone *tz = NULL; /* written once, before g_once_init_leave() */
  if (g_once_init_enter(&tz_once)) {
    tz = g_time_zone_new_identifier("America/New_York");
    if (tz == NULL)
      tz = g_time_zone_new_identifier("EST5EDT,M3.2.0,M11.1.0");
    g_once_init_leave(&tz_once, 1);
  }
  return tz;
}

/* ------------------------------------------------------------------------ */
/* URL builders                                                             */
/* ------------------------------------------------------------------------ */

/**
 * check_symbol_kind:
 * @symbol: (nullable): candidate ticker
 * @kind: candidate kind
 * @error: (nullable): return location for a #GError
 *
 * Validates the symbol and kind of a request and reports STK_ERROR_INVALID_ARG for either.
 *
 * Preconditions:
 *  - @symbol is NULL or NUL-terminated (stk_symbol_is_valid() bounds its reads to 16 bytes); @error
 *    is NULL or points to a NULL GError*.
 * Postconditions:
 *  - on TRUE nothing is set; on FALSE *@error (when non-NULL) is a STK_ERROR /
 *    STK_ERROR_INVALID_ARG error.
 * Thread affinity: any thread (pure)
 * Ownership: @symbol and @error stay with the caller; nothing is retained.
 *
 * Returns: TRUE if stk_symbol_is_valid(@symbol) and kind_is_valid(@kind), else FALSE.
 */
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

/**
 * encode_symbol:
 * @symbol: a symbol that satisfied stk_symbol_is_valid()
 *
 * Upper-cases and percent-encodes a validated symbol for use in a URL path.
 *
 * Preconditions:
 *  - @symbol is non-NULL and NUL-terminated within 16 bytes: both callers invoke it only after
 *    check_symbol_kind() succeeded on the same string.
 * Postconditions:
 *  - nothing is modified; the result contains only unreserved characters and %XX escapes.
 * Thread affinity: any thread (pure)
 * Ownership: @symbol stays with the caller; the returned string is new.
 *
 * Returns: (transfer full): the encoded symbol, to be released with g_free().
 */
static char *
encode_symbol(const char *symbol)
{
  g_autofree char *upper = g_ascii_strup(symbol, -1);
  return g_uri_escape_string(upper, NULL, FALSE);
}

/* Documented in stk-nasdaq.h. */
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

/**
 * date_to_iso:
 * @date: a valid GDate
 *
 * Formats a date as "YYYY-MM-DD".
 *
 * Preconditions:
 *  - @date is non-NULL and g_date_valid(): stk_nasdaq_series_url() passes @today (checked valid and
 *    inside the supported years) and a date derived from it by g_date_subtract_months().
 * Postconditions:
 *  - nothing is modified.
 * Thread affinity: any thread (pure)
 * Ownership: @date stays with the caller; the returned string is new.
 *
 * Returns: (transfer full): the ISO date string, to be released with g_free().
 */
static char *
date_to_iso(const GDate *date)
{
  return g_strdup_printf("%04u-%02u-%02u", (guint)g_date_get_year(date),
                         (guint)g_date_get_month(date), (guint)g_date_get_day(date));
}

/* Documented in stk-nasdaq.h. */
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

/* Documented in stk-nasdaq.h. */
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
  /* One pass to the terminating NUL (a caller precondition) remembering the end of
   * the last non-whitespace byte; it stops early once the trimmed text is already
   * longer than the limit (rejected below either way). */
  gsize len = 0;
  for (gsize i = 0; text[i] != '\0' && len <= SEARCH_TEXT_MAX_BYTES; i++) {
    if (!(text[i] == ' ' || (text[i] >= '\t' && text[i] <= '\r')))
      len = i + 1;
  }
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

/**
 * member_object:
 * @obj: a JsonObject
 * @name: NUL-terminated member name
 *
 * Returns the member @name of @obj if it is a JSON object.
 *
 * Preconditions:
 *  - @obj and @name are non-NULL; every call site passes an object it already obtained from the
 *    parser tree or checked non-NULL.
 * Postconditions:
 *  - nothing is modified.
 * Thread affinity: any thread (pure)
 * Ownership: the result is borrowed from @obj (valid while the owning JsonParser lives).
 *
 * Returns: (nullable) (transfer none): the member object, or NULL if missing, null or of another
 *   type.
 */
static JsonObject *
member_object(JsonObject *obj, const char *name)
{
  JsonNode *node = json_object_get_member(obj, name);
  return (node != NULL && JSON_NODE_HOLDS_OBJECT(node)) ? json_node_get_object(node) : NULL;
}

/**
 * member_string:
 * @obj: a JsonObject
 * @name: NUL-terminated member name
 *
 * Returns the member @name of @obj if it is a JSON string.
 *
 * Preconditions:
 *  - @obj and @name are non-NULL; every call site passes a non-NULL object (blocks are NULL-checked
 *    before use).
 * Postconditions:
 *  - nothing is modified.
 * Thread affinity: any thread (pure)
 * Ownership: the result is borrowed from @obj (valid while the owning JsonParser lives).
 *
 * Returns: (nullable) (transfer none): the NUL-terminated string, or NULL if missing or not a
 *   string. The text is external and its length is not bounded here.
 */
static const char *
member_string(JsonObject *obj, const char *name)
{
  JsonNode *node = json_object_get_member(obj, name);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string(node);
}

/**
 * member_bool:
 * @obj: a JsonObject
 * @name: NUL-terminated member name
 *
 * Returns whether the member @name of @obj is the JSON boolean true.
 *
 * Preconditions:
 *  - @obj and @name are non-NULL; every call site passes a non-NULL block.
 * Postconditions:
 *  - nothing is modified.
 * Thread affinity: any thread (pure)
 * Ownership: none retained.
 *
 * Returns: TRUE only for a boolean member that is true; FALSE if missing, null, mistyped or false.
 */
static gboolean
member_bool(JsonObject *obj, const char *name)
{
  JsonNode *node = json_object_get_member(obj, name);
  return node != NULL && JSON_NODE_HOLDS_VALUE(node) &&
         json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}

/**
 * node_number:
 * @node: (nullable): the node to read
 * @out: (out): receives the value on success
 *
 * Reads a JSON integer or double node as a finite double.
 *
 * Preconditions:
 *  - @out is non-NULL and writable.
 * Postconditions:
 *  - on TRUE *@out holds a finite value; on FALSE *@out is untouched. Integers are converted to
 *    double by json_node_get_double() (no float-to-integer cast is involved).
 * Thread affinity: any thread (pure)
 * Ownership: nothing retained.
 *
 * Returns: TRUE if @node is an int64 or double value that is finite, else FALSE.
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
 * @obj: a JsonObject
 * @name: NUL-terminated member name
 * @out: (out): receives the value on success
 *
 * Reads the member @name of @obj as a finite number (see node_number()).
 *
 * Preconditions:
 *  - @obj, @name and @out are non-NULL; every call site passes a non-NULL object.
 * Postconditions:
 *  - as for node_number().
 * Thread affinity: any thread (pure)
 * Ownership: nothing retained.
 *
 * Returns: TRUE if the member is a finite JSON number, else FALSE.
 */
static gboolean
member_number(JsonObject *obj, const char *name, double *out)
{
  return node_number(json_object_get_member(obj, name), out);
}

/**
 * name_is_usable:
 * @name: (nullable): NUL-terminated text from a JSON string
 *
 * Tells whether an external display name may be stored: non-empty, at most NAME_MAX_BYTES bytes,
 * valid UTF-8 and free of control characters. It has the same semantics as the StkYahoo provider's
 * name_is_usable(); the length is established with a bounded memchr().
 *
 * Preconditions:
 *  - @name is NULL or NUL-terminated (JSON-parsed text); at most NAME_MAX_BYTES + 1 bytes are
 *    examined for the terminator.
 * Postconditions:
 *  - nothing is modified. Callers store NULL instead of an unusable name (an empty quote name, or
 *    "" for a search hit); this filtering is the behavior change approved for external company
 *    names.
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

/**
 * truncated_copy:
 * @text: valid UTF-8, NUL-terminated
 * @max_chars: number of characters to keep; <= 0 yields "".
 *
 * Copies the first @max_chars characters of the valid UTF-8 @text (all of it if shorter), so a
 * server message cannot bloat an error message or split a UTF-8 sequence.
 *
 * Preconditions:
 *  - @text is non-NULL, NUL-terminated valid UTF-8: the only caller passes a JSON string of a body
 *    that parse_envelope() validated with g_utf8_validate().
 * Postconditions:
 *  - the copy is always NUL-terminated and ends on a character boundary; @text is unchanged.
 * Thread affinity: any thread (pure)
 * Ownership: @text stays with the caller; the returned string is new.
 *
 * Returns: (transfer full): the prefix copy, to be released with g_free().
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
 * parse_envelope:
 * @data: (nullable): body bytes
 * @len: number of readable bytes at @data
 * @root: (out): receives the root object on success
 * @error: (nullable): return location for a #GError
 *
 * Validates @data as a UTF-8 JSON envelope {"data":..., "status":{"rCode":N}} and returns the
 * parser, the owner of *@root. rCode != 200 is STK_ERROR_NOT_FOUND; an embedded error message is
 * truncated to 80 characters.
 *
 * Preconditions:
 *  - @data is NULL only with @len == 0, otherwise it points to @len readable bytes (not necessarily
 *    NUL-terminated: reads are bounded by @len).
 *  - @root is non-NULL; @error is NULL or points to a NULL GError*.
 * Postconditions:
 *  - on success *@root is borrowed from the returned parser; on failure NULL is returned, *@root is
 *    untouched and *@error (when non-NULL) is STK_ERROR_PARSE (empty, too large, non-UTF-8,
 *    malformed, wrong shape) or STK_ERROR_NOT_FOUND (rCode != 200). @len above
 *    STK_NASDAQ_MAX_BODY_BYTES is rejected before any parsing.
 * Thread affinity: any thread (pure)
 * Ownership: @data stays with the caller and is not retained (json-glib copies what it needs); the
 *   parser is returned to the caller and *@root must not outlive it.
 *
 * Returns: (transfer full) (nullable): the parser, or NULL with @error set.
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

/**
 * envelope_data_object:
 * @root: the envelope root returned by parse_envelope()
 * @error: (nullable): return location for a #GError
 *
 * Returns the envelope's "data" object or sets STK_ERROR_PARSE.
 *
 * Preconditions:
 *  - @root is non-NULL (as filled in by parse_envelope()); @error is NULL or points to a NULL
 *    GError*.
 * Postconditions:
 *  - on NULL, *@error (when non-NULL) is STK_ERROR_PARSE "response has no data object".
 * Thread affinity: any thread (pure)
 * Ownership: the result is borrowed from @root (valid while its parser lives).
 *
 * Returns: (nullable) (transfer none): the "data" object, or NULL with @error set.
 */
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

/**
 * scan_uint:
 * @p: (inout): cursor into a NUL-terminated string
 * @min_digits: minimum number of digits required
 * @max_digits: maximum number of digits consumed; at most 9 so that the value cannot overflow a
 *   guint (all call sites pass 2 or 4)
 * @out: (out): receives the value on success
 *
 * Reads up to @max_digits ASCII digits at *@p as an unsigned decimal number and advances *@p past
 * them.
 *
 * Preconditions:
 *  - @p, *@p and @out are non-NULL and *@p is NUL-terminated (the scan stops at the first
 *    non-digit, in particular at the NUL).
 * Postconditions:
 *  - *@p is advanced past the digits consumed even on failure; *@out is written only on TRUE.
 * Thread affinity: any thread (pure)
 * Ownership: the cursor points into a string owned by the caller; nothing is retained.
 *
 * Returns: TRUE if at least @min_digits digits were read, else FALSE.
 */
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

/**
 * scan_char:
 * @p: (inout): cursor into a NUL-terminated string
 * @c: the expected non-NUL character
 *
 * Consumes the character @c at *@p.
 *
 * Preconditions:
 *  - @p and *@p are non-NULL and *@p is NUL-terminated; @c is not NUL (all call sites pass a
 *    literal punctuation character), so the cursor never moves past the NUL.
 * Postconditions:
 *  - *@p is advanced by one only when it matched.
 * Thread affinity: any thread (pure)
 * Ownership: the cursor points into a string owned by the caller; nothing is retained.
 *
 * Returns: TRUE if **@p == @c (and the cursor advanced), else FALSE.
 */
static gboolean
scan_char(const char **p, char c)
{
  if (**p != c)
    return FALSE;
  (*p)++;
  return TRUE;
}

/**
 * parse_quote_timestamp:
 * @text: (nullable): NUL-terminated timestamp text
 * @out_ms: (out): receives the Unix time in milliseconds on success
 *
 * Parses the timestamp strings of the /info endpoint: "Oct 8, 2026 7:25 PM ET", "Closed at Oct 8,
 * 2026 4:00 PM ET" or "Oct 8, 2026". Times are America/New_York; a bare date is mapped to 12:00
 * that day.
 *
 * Preconditions:
 *  - @text is NULL or NUL-terminated (external JSON text; every comparison used stops at the first
 *    mismatch or NUL); @out_ms is non-NULL.
 * Postconditions:
 *  - *@out_ms is written only on TRUE. FALSE is returned for NULL, any deviation from the formats,
 *    an invalid calendar date, or when the New York time zone is unavailable (new_york_tz()
 *    returned NULL). Digit groups have at most 4 digits, so no integer overflow occurs.
 * Thread affinity: any thread (the shared time zone is read-only).
 * Ownership: @text stays with the caller; nothing is retained.
 *
 * Returns: TRUE if the text was fully parsed and denotes a representable instant, else FALSE.
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

  GTimeZone *tz = new_york_tz();
  if (tz == NULL)
    return FALSE;
  GDateTime *dt = g_date_time_new(tz, (gint)year, (gint)month, (gint)day, (gint)hour,
                                  (gint)minute, 0);
  if (dt == NULL)
    return FALSE;
  *out_ms = g_date_time_to_unix(dt) * 1000;
  g_date_time_unref(dt);
  return TRUE;
}

/**
 * parse_row_date:
 * @text: (nullable): NUL-terminated date text
 * @out_ms: (out): receives the epoch milliseconds on success
 *
 * Parses "MM/DD/YYYY" into 00:00 UTC of that date, as epoch milliseconds.
 *
 * Preconditions:
 *  - @text is NULL or NUL-terminated; @out_ms is non-NULL.
 * Postconditions:
 *  - *@out_ms is written only on TRUE. FALSE is returned for NULL, text not exactly in the format,
 *    an invalid date, or a date outside 2022-01-01..2100-12-31 (EPOCH_MIN_MS <= ms < EPOCH_MAX_MS).
 * Thread affinity: any thread (pure)
 * Ownership: @text stays with the caller; nothing is retained.
 *
 * Returns: TRUE if parsed and inside the accepted window, else FALSE.
 */
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

/**
 * parse_quote_impl:
 * @data: (nullable): body bytes
 * @len: number of readable bytes at @data
 * @symbol: requested symbol
 * @kind: requested kind
 * @regular_block: (out) (optional): receives whether the regular-session block was used; untouched
 *   on failure
 * @error: (nullable): return location for a #GError
 *
 * Implementation of stk_nasdaq_parse_quote(). If @regular_block is not NULL it is set to whether
 * the price was taken from the regular-session block (primaryData while open or after the nightly
 * roll, else secondaryData) rather than from the fallback block. The company name is filtered with
 * name_is_usable().
 *
 * Preconditions:
 *  - as for stk_nasdaq_parse_quote(): @data is NULL only with @len == 0 or points to @len bytes;
 *    @symbol is NULL or NUL-terminated; @error is NULL or points to a NULL GError*.
 * Postconditions:
 *  - as for stk_nasdaq_parse_quote(); *@regular_block is written only on the success paths that
 *    reach the price selection.
 * Thread affinity: any thread (pure)
 * Ownership: nothing retained; the returned quote is new.
 *
 * Returns: (transfer full) (nullable): the quote, or NULL with @error set.
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
  /* Left to right with short-circuit: no byte after the first non-matching one is read. */
  if (currency != NULL &&
      !(g_ascii_isupper(currency[0]) && g_ascii_isupper(currency[1]) &&
        g_ascii_isupper(currency[2]) && currency[3] == '\0'))
    currency = NULL;

  const char *name = member_string(info, "companyName");
  if (!name_is_usable(name))
    name = NULL;
  StkQuote *quote = stk_quote_new(symbol, name, kind, currency);
  quote->price = price;
  quote->market_open = market_open;
  if (block != NULL) {
    /* An unchanged quote (deltaIndicator "unch", e.g. a fund whose NAV did
     * not move) has empty netChange and percentageChange: zero, not unknown. */
    const char *delta = member_string(block, "deltaIndicator");
    gboolean unchanged = delta != NULL && g_ascii_strcasecmp(delta, "unch") == 0;
    const char *net = member_string(block, "netChange");
    const char *pct = member_string(block, "percentageChange");
    double v;
    if (stk_parse_change(net, &v) == STK_PARSE_OK)
      quote->change = v;
    else if (unchanged && (net == NULL || net[0] == '\0'))
      quote->change = 0.0;
    if (stk_parse_percent(pct, &v) == STK_PARSE_OK)
      quote->change_pct = v;
    else if (unchanged && (pct == NULL || pct[0] == '\0'))
      quote->change_pct = 0.0;
    gint64 ms;
    if (parse_quote_timestamp(member_string(block, "lastTradeTimestamp"), &ms))
      quote->as_of_ms = ms;
    quote->is_realtime = member_bool(block, "isRealTime");
  }
  return quote;
}

/* Documented in stk-nasdaq.h. */
StkQuote *
stk_nasdaq_parse_quote(const guint8 *data, gsize len, const char *symbol, StkKind kind,
                       GError **error)
{
  return parse_quote_impl(data, len, symbol, kind, NULL, error);
}

/**
 * chart_array:
 * @info: the "data" object
 * @empty: (out): set to whether "chart" is JSON null
 * @error: (nullable): return location for a #GError
 *
 * Returns data.chart as an array; *@empty is set for an explicit JSON null.
 *
 * Preconditions:
 *  - @info and @empty are non-NULL; @error is NULL or points to a NULL GError*.
 * Postconditions:
 *  - *@empty is always written. NULL with *@empty FALSE means an error: *@error (when non-NULL) is
 *    STK_ERROR_PARSE "response has no chart array".
 * Thread affinity: any thread (pure)
 * Ownership: the array is borrowed from @info (valid while its parser lives).
 *
 * Returns: (nullable) (transfer none): the chart array, or NULL (empty chart or error, see @empty).
 */
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

/* Documented in stk-nasdaq.h. */
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

/**
 * compare_points:
 * @a: (not nullable): const StkPoint *
 * @b: (not nullable): const StkPoint *
 *
 * qsort-style comparison of two StkPoint by t_ms (ascending), written without subtraction so it
 * cannot overflow.
 *
 * Preconditions:
 *  - @a and @b point to valid StkPoint values (GArray elements passed by g_array_sort()).
 * Postconditions:
 *  - nothing is modified.
 * Thread affinity: any thread (pure)
 * Ownership: nothing retained.
 *
 * Returns: negative, zero or positive as @a->t_ms is before, equal to or after @b->t_ms.
 */
static int
compare_points(gconstpointer a, gconstpointer b)
{
  const StkPoint *pa = a, *pb = b;
  return (pa->t_ms > pb->t_ms) - (pa->t_ms < pb->t_ms);
}

/* Documented in stk-nasdaq.h. */
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

/* Documented in stk-nasdaq.h. */
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
    if (!name_is_usable(name))
      name = NULL;
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

/*
 * Instance data; main thread only. session and base_url are owned by the instance (released in
 * dispose / finalize); inflight counts the live Ops, each of which holds a strong reference to
 * the instance, so the instance is never finalised while inflight > 0.
 */
struct _StkNasdaq {
  GObject parent_instance;
  SoupSession *session; /* created once, shared by every request */
  char *base_url;       /* validated origin, no trailing slash */
  guint timeout_ms;
  guint backoff_base_ms;
  guint inflight; /* live Ops (requests not yet released), see STK_NASDAQ_MAX_INFLIGHT */
};

static void stk_nasdaq_provider_init(StkProviderInterface *iface);

/*
 * G_DEFINE_TYPE_WITH_CODE(StkNasdaq, stk_nasdaq, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(
 * STK_TYPE_PROVIDER, stk_nasdaq_provider_init)) generates the following (checked with gcc -E of
 * this file with the project's compile flags):
 *  - GType stk_nasdaq_get_type(void): returns the GType of StkNasdaq and registers it on the first
 *    call, inside a once-initialisation guard, by calling stk_nasdaq_get_type_once().
 *    Thread affinity: any thread. Ownership: none (no pointers).
 *  - static GType stk_nasdaq_get_type_once(void): registers StkNasdaq as a static type derived
 *    from G_TYPE_OBJECT (class structure StkNasdaqClass, instance structure StkNasdaq, class
 *    initialiser stk_nasdaq_class_intern_init(), instance initialiser stk_nasdaq_init()). The
 *    _WITH_CODE argument G_IMPLEMENT_INTERFACE() then adds the StkProvider interface to the new
 *    type with g_type_add_interface_static(), using stk_nasdaq_provider_init() as the interface
 *    initialiser (no interface data), and the new GType is returned.
 *    Thread affinity: only via stk_nasdaq_get_type(). Ownership: none (no pointers).
 *  - static void stk_nasdaq_class_intern_init(gpointer klass): @klass is the class structure
 *    being initialised (borrowed, written; nothing is retained). It stores the parent class in
 *    stk_nasdaq_parent_class, adjusts the private offset only if StkNasdaq_private_offset is
 *    non-zero, then calls stk_nasdaq_class_init(). Called by GObject once per type, from the
 *    thread that first uses the type. Ownership: @klass belongs to GObject.
 *  - static gpointer stk_nasdaq_parent_class: NULL until class_intern_init() runs; used by
 *    dispose() and finalize() to chain up.
 *  - static gint StkNasdaq_private_offset: stays 0 (no G_ADD_PRIVATE is used).
 *  - static inline gpointer stk_nasdaq_get_instance_private(StkNasdaq *self): returns self plus
 *    the private offset; unused here (G_GNUC_UNUSED). @self is borrowed. Thread affinity: any
 *    thread.
 * The interface initialiser stk_nasdaq_provider_init(StkProviderInterface *iface) is not
 * generated; it is the function named in the _WITH_CODE argument and is defined below. GObject
 * calls it with the interface structure of StkNasdaq (@iface is borrowed, written, nothing is
 * retained; owned by GObject) when the interface is set up for the type, and it stores the six
 * nasdaq_*_async / nasdaq_*_finish implementations in the vtable.
 */
G_DEFINE_TYPE_WITH_CODE(StkNasdaq, stk_nasdaq, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(STK_TYPE_PROVIDER, stk_nasdaq_provider_init))

/**
 * stk_nasdaq_init:
 * @self: (not nullable): the new instance
 *
 * GObject instance initialiser: creates the shared HTTP session and sets the production defaults.
 * No cookie jar and no HTTP cache feature are added: nothing is persisted.
 *
 * Preconditions:
 *  - @self is a zero-initialised StkNasdaq instance being constructed by GObject.
 * Postconditions:
 *  - @self->session is a new SoupSession with the app user agent; base_url, timeout_ms and
 *    backoff_base_ms hold the STK_NASDAQ_* defaults; inflight is 0.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @self owns the session and the base_url copy; they are released by dispose and
 *   finalize.
 */
static void
stk_nasdaq_init(StkNasdaq *self)
{
  /* No cookie jar and no HTTP cache feature are added: nothing is persisted. */
  self->session = g_object_new(SOUP_TYPE_SESSION, "user-agent", STK_NASDAQ_USER_AGENT, NULL);
  self->base_url = g_strdup(STK_NASDAQ_BASE_URL);
  self->timeout_ms = STK_NASDAQ_TIMEOUT_MS;
  self->backoff_base_ms = STK_NASDAQ_BACKOFF_BASE_MS;
}

/**
 * stk_nasdaq_dispose:
 * @object: (not nullable): the StkNasdaq being disposed
 *
 * GObject dispose: drops the HTTP session. Pending operations hold their own reference to the
 * instance (see Op.owner), so dispose cannot run while one is outstanding.
 *
 * Preconditions:
 *  - @object is a StkNasdaq; may run more than once (g_clear_object() makes that safe).
 * Postconditions:
 *  - @self->session is NULL; the parent dispose has run.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the session reference owned by the instance is released; @object stays with the
 *   caller.
 */
static void
stk_nasdaq_dispose(GObject *object)
{
  StkNasdaq *self = STK_NASDAQ(object);
  g_clear_object(&self->session);
  G_OBJECT_CLASS(stk_nasdaq_parent_class)->dispose(object);
}

/**
 * stk_nasdaq_finalize:
 * @object: (not nullable): the StkNasdaq being finalized
 *
 * GObject finalize: frees the base URL copy.
 *
 * Preconditions:
 *  - @object is a StkNasdaq whose last reference is gone (runs once).
 * Postconditions:
 *  - base_url is freed; the parent finalize has run.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the instance owns base_url; it is freed here.
 */
static void
stk_nasdaq_finalize(GObject *object)
{
  StkNasdaq *self = STK_NASDAQ(object);
  g_free(self->base_url);
  G_OBJECT_CLASS(stk_nasdaq_parent_class)->finalize(object);
}

/**
 * stk_nasdaq_class_init:
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
stk_nasdaq_class_init(StkNasdaqClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = stk_nasdaq_dispose;
  object_class->finalize = stk_nasdaq_finalize;
}

/* Documented in stk-nasdaq.h. */
StkNasdaq *
stk_nasdaq_new(void)
{
  return g_object_new(STK_TYPE_NASDAQ, NULL);
}

/**
 * validate_base_url:
 * @base_url: NUL-terminated URL text
 *
 * Returns a normalised copy of @base_url if it is an acceptable origin (see
 * stk_nasdaq_new_custom()).
 *
 * Preconditions:
 *  - @base_url is non-NULL and NUL-terminated (the caller, stk_nasdaq_new_custom(), checks
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

/* Documented in stk-nasdaq.h. */
StkNasdaq *
stk_nasdaq_new_custom(const char *base_url, guint timeout_ms, guint backoff_base_ms)
{
  g_return_val_if_fail(base_url != NULL, NULL);
  g_return_val_if_fail(timeout_ms >= 1 && timeout_ms <= 600000, NULL);
  g_return_val_if_fail(backoff_base_ms <= BACKOFF_BASE_MAX_MS, NULL);
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

/*
 * Task data of one operation: what to parse and the state of the request. Created by
 * op_new(), owned by the GTask (task data, freed by op_free() when the task is finalised) and
 * used from the main thread only.
 *
 * Ownership of the members: url, symbol, user_cancellable (a strong reference) and owner (a
 * strong reference; the Op counts in owner->inflight from op_new() to op_free()) are owned by
 * the Op. The per-attempt members are owned by the Op between attempt_start() and
 * attempt_cleanup(); the back-off sources between schedule_retry() and backoff_cleanup().
 *
 * Lifetime: every GSource and GAsyncReadyCallback of an operation holds its own reference to
 * the GTask as callback data (released when the callback has run or the source is finalised;
 * the user-cancel handler holds the attempt cancellable instead), so the task, and with it the
 * Op, outlives every pending callback.
 * That cycle (task -> Op -> source -> task) is broken by attempt_cleanup() and
 * backoff_cleanup(), which every path that ends an attempt or a wait runs before the task is
 * completed or retried.
 */
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
  GCancellable *attempt_cancel; /* cancelled by the deadline or by the user; the user_cancellable
                                 * handler holds a second reference as its data */
  gulong user_cancel_id;        /* handler on user_cancellable, 0 if none */
  GSource *deadline;            /* owned reference; its callback data is a task reference */
  gboolean timed_out;
  GInputStream *stream;
  GByteArray *body;  /* owned until moved into the GBytes handed to op_complete() */
  gsize body_filled; /* bytes of body->data already read; <= STK_NASDAQ_MAX_BODY_BYTES + 1 */
  goffset declared;  /* Content-Length of an undecoded body, else -1 */

  /* Back-off wait state, released by backoff_cleanup(). */
  GSource *backoff_timer;  /* owned reference; its callback data is a task reference */
  GSource *backoff_cancel; /* owned reference (nullable); callback data is a task reference */
} Op;

static char tag_quote, tag_series, tag_search;

/**
 * attempt_cleanup:
 * @op: (not nullable): the operation
 *
 * Releases the per-attempt state of @op: the deadline source, the user-cancellation handler, the
 * attempt cancellable, the stream (closed asynchronously, which hands the connection back to the
 * pool), the message and the partial body.
 *
 * Preconditions:
 *  - @op is live (owned by its GTask). It may be called with no attempt running (everything NULL/0)
 *    and more than once.
 *  - It is not called from inside on_user_cancelled() (g_cancellable_disconnect() would wait for
 *    that handler): its callers are fail_attempt(), on_read() and op_free(), reached from I/O
 *    callbacks and from task finalisation. This relies on a cancelled read/send being completed
 *    from the main loop and not from inside g_cancellable_cancel(): the pre-existing design,
 *    exercised by the cancel tests, not something checked here.
 * Postconditions:
 *  - deadline, user_cancel_id, attempt_cancel, stream, msg and body are NULL/0 and body_filled is
 *    0. Dropping the deadline source also drops the task reference held by its callback data, and
 *    disconnecting the handler drops the attempt_cancel reference held by it.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @op stays with its task; every per-attempt resource owned by @op is released here.
 */
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

/**
 * backoff_cleanup:
 * @op: (not nullable): the operation
 *
 * Cancels and releases the back-off wait of @op (the timer and the cancellation watch). Dropping
 * each source also drops the task reference held by its callback data.
 *
 * Preconditions:
 *  - @op is live (owned by its GTask); may be called with no wait pending and more than once.
 * Postconditions:
 *  - backoff_timer and backoff_cancel are NULL and their sources are destroyed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @op stays with its task; the two source references owned by @op are released.
 */
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

/**
 * op_free:
 * @data: (not nullable): the Op created by op_new()
 *
 * GDestroyNotify of the task data: releases an Op and everything it owns, then drops the in-flight
 * count and the owner reference.
 *
 * Preconditions:
 *  - @data is the Op installed with g_task_set_task_data(); called exactly once, when the GTask is
 *    finalised (so no source or callback can still reference the task, because each of them holds a
 *    task reference that attempt_cleanup()/backoff_cleanup() release first).
 *  - op->owner->inflight is > 0: op_new() incremented it once for this Op.
 * Postconditions:
 *  - all members are released; owner->inflight is decremented once; the owner reference taken in
 *    op_new() is dropped (which may finalise the provider). @data is invalid afterwards.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @data and of every member of the Op (url, symbol, user_cancellable
 *   ref, owner ref, per-attempt and back-off state).
 */
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

/**
 * quote_destroy:
 * @quote: (nullable): a StkQuote returned by the parser
 *
 * GDestroyNotify for a StkQuote handed to g_task_return_pointer().
 *
 * Preconditions:
 *  - @quote is NULL or a quote owned by the GTask result.
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
 * @series: (nullable): a StkSeries returned by a parser
 *
 * GDestroyNotify for a StkSeries handed to g_task_return_pointer().
 *
 * Preconditions:
 *  - @series is NULL or a series reference owned by the GTask result.
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
 * hits_destroy:
 * @hits: (not nullable): a GPtrArray returned by stk_nasdaq_parse_search()
 *
 * GDestroyNotify for the hit array handed to g_task_return_pointer().
 *
 * Preconditions:
 *  - @hits is a non-NULL array owned by the GTask result.
 * Postconditions:
 *  - the array and its hits are freed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of one reference to @hits.
 */
static void
hits_destroy(gpointer hits)
{
  g_ptr_array_unref(hits);
}

/**
 * op_complete:
 * @task: (not nullable): the operation's task; its task data is an Op
 * @body: (not nullable): the complete response body
 *
 * Parses the finished body and completes @task with the result (or the parse error).
 *
 * Preconditions:
 *  - @task has an Op as task data that has not been returned yet; @body is a GBytes of at most
 *    STK_NASDAQ_MAX_BODY_BYTES bytes (on_read() fails the attempt instead of completing when the
 *    body exceeds the cap; parse_envelope() re-checks the length).
 * Postconditions:
 *  - @task is completed exactly once: with a quote (OP_QUOTE), a heap double holding the previous
 *    close or NAN (OP_PREV_CLOSE, only the regular-session block counts), a series (OP_SERIES: fund
 *    historical or stock/ETF chart) or a hit array (OP_SEARCH); or with the parser's error. An
 *    unknown op->type (cannot happen: the type is set by op_new() from the OpType enumeration)
 *    completes the task with STK_ERROR_INVALID_ARG instead of aborting.
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
    g_set_error_literal(&error, STK_ERROR, STK_ERROR_INVALID_ARG, "unknown operation type");
    break;
  }
  if (error != NULL)
    g_task_return_error(task, error);
}

/**
 * http_status_error:
 * @status: HTTP status code of a non-2xx response
 *
 * Maps an HTTP status to the error reported to the caller: 429 is STK_ERROR_RATE_LIMITED, 400 and
 * 404 are STK_ERROR_NOT_FOUND, everything else STK_ERROR_HTTP. The message formats are fixed; only
 * the numeric status and libsoup's own phrase are embedded.
 *
 * Preconditions:
 *  - none (any value is accepted).
 * Postconditions:
 *  - a new GError is returned; nothing else changes.
 * Thread affinity: any thread (pure)
 * Ownership: the returned error is owned by the caller.
 *
 * Returns: (transfer full): the error.
 */
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

/**
 * normalize_error:
 * @op: (not nullable): the operation
 * @err: (transfer full): the failure of the attempt
 *
 * Turns a libsoup/GIO failure of the current attempt into the error the caller sees: cancellation
 * of the user's cancellable wins, STK_ERROR errors pass through, timeouts become STK_ERROR_TIMEOUT,
 * anything else STK_ERROR_NETWORK ("network error: %s" with the library's message; the format is
 * fixed).
 *
 * Preconditions:
 *  - @op is live; @err is non-NULL and owned by the caller, who must not use it afterwards.
 * Postconditions:
 *  - @err is either returned (STK_ERROR domain) or freed; the result is never NULL.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @err; the returned error is owned by the caller.
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

/* Forward declaration; documented at the definition below. */
static void schedule_retry(GTask *task);

/**
 * fail_attempt:
 * @task: (not nullable): the operation's task
 * @err: (transfer full): the failure of the attempt
 *
 * Ends the current attempt with @err: retries after a back-off when the failure class allows it
 * (network, timeout, HTTP 5xx, fewer than STK_NASDAQ_MAX_RETRIES retries so far), otherwise
 * completes @task with the error.
 *
 * Preconditions:
 *  - @task has an Op as task data and the caller holds a reference to @task for the duration of the
 *    call (so op stays valid even if the task completes synchronously); @err is non-NULL.
 * Postconditions:
 *  - the attempt state is released; either schedule_retry() has queued the next attempt or @task
 *    has been completed with the normalised error.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @err; @task stays with the caller.
 */
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

/**
 * on_user_cancelled:
 * @user: (unused): the caller's cancellable
 * @attempt_cancel: (not nullable): the attempt's GCancellable (a GCancellable, passed as gpointer)
 *
 * "cancelled" handler of the caller's GCancellable: cancels the current attempt. It may run in
 * whatever thread cancels the caller's cancellable (g_cancellable_cancel() is thread-safe).
 *
 * Preconditions:
 *  - @attempt_cancel is the reference taken in attempt_start() and owned by the connection; the
 *    handler is disconnected by attempt_cleanup() before the attempt's own reference is dropped,
 *    and the connection's reference keeps the object alive until the notify runs.
 * Postconditions:
 *  - the attempt cancellable is cancelled; the pending I/O callback then reports the cancellation.
 * Thread affinity: any thread (the one that cancels the caller's cancellable); it only calls
 *   g_cancellable_cancel(), which is thread-safe.
 * Ownership: @attempt_cancel stays owned by the connection; @user is not retained.
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
 * GSource callback of the per-attempt deadline: marks the attempt as timed out and cancels its I/O
 * (the pending I/O callback reports the failure).
 *
 * Preconditions:
 *  - @user_data is the task reference held by the source's callback data, so the task and its Op
 *    are alive. attempt_cancel is non-NULL while the source is attached, because attempt_cleanup()
 *    destroys the source before it clears attempt_cancel (g_cancellable_cancel(NULL) would be a
 *    no-op anyway).
 * Postconditions:
 *  - op->timed_out is TRUE and the attempt cancellable is cancelled; the source is removed (and
 *    releases its task reference).
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

/* Forward declaration; documented at the definition below. */
static void read_next(GTask *task);

/**
 * on_read:
 * @source: (not nullable): the response GInputStream (a GObject, passed as GObject*)
 * @result: (not nullable): the read result
 * @user_data: (transfer full): the operation's GTask reference taken in read_next(), released here
 *
 * GAsyncReadyCallback of one body read: accumulates the bytes, enforces the body cap and the
 * declared length, and either reads on, fails the attempt or hands the body to op_complete().
 *
 * Preconditions:
 *  - @source is op->stream and @result belongs to the read started by read_next(), whose buffer
 *    lies inside op->body (not reallocated while a read is pending). op->body_filled <=
 *    STK_NASDAQ_MAX_BODY_BYTES on entry (read_next() is only called then).
 * Postconditions:
 *  - body_filled is incremented by the bytes read; if it exceeds STK_NASDAQ_MAX_BODY_BYTES the
 *    attempt fails with STK_ERROR_PARSE "response too large" (so body_filled <= cap + 1 and (guint)
 *    casts of it cannot truncate); a declared Content-Length that disagrees with the body is
 *    STK_ERROR_NETWORK; at end of stream the body (trimmed to body_filled, <= the cap) is passed to
 *    op_complete(). Exactly one of: another read, a failed attempt, or completion.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the task reference in @user_data is released on every path through the g_autoptr; the
 *   body is stolen into a GBytes that is released here.
 */
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

/**
 * read_next:
 * @task: (not nullable): the operation's task
 *
 * Issues the next asynchronous read into the body buffer. It never asks for more than one byte
 * beyond the cap, so an oversized body is detected without buffering it.
 *
 * Preconditions:
 *  - @task has an Op with a live stream and body, and op->body_filled <= STK_NASDAQ_MAX_BODY_BYTES
 *    (on_sent() starts with 0; on_read() checks the cap before calling), so the chunk is >= 1 byte
 *    and body_filled + chunk <= cap + 1, which fits a guint (G_STATIC_ASSERT above).
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
  gsize chunk = MIN((gsize)READ_CHUNK_BYTES, (gsize)STK_NASDAQ_MAX_BODY_BYTES + 1 - op->body_filled);
  g_byte_array_set_size(op->body, (guint)(op->body_filled + chunk));
  g_input_stream_read_async(op->stream, op->body->data + op->body_filled, chunk,
                            G_PRIORITY_DEFAULT, op->attempt_cancel, on_read,
                            g_object_ref(task));
}

/**
 * on_sent:
 * @source: (not nullable): the SoupSession (a GObject, passed as GObject*)
 * @result: (not nullable): the send result
 * @user_data: (transfer full): the operation's GTask reference taken in attempt_start(), released
 *   here
 *
 * GAsyncReadyCallback of soup_session_send_async(): checks the status and the declared length, then
 * starts reading the body.
 *
 * Preconditions:
 *  - @user_data is a task whose Op has a live message (op->msg) of the attempt that was sent.
 * Postconditions:
 *  - on a transport error or a non-2xx status the attempt fails (fail_attempt()); a declared
 *    Content-Length above STK_NASDAQ_MAX_BODY_BYTES fails with STK_ERROR_PARSE "response too large"
 *    before any body byte is read. Otherwise op->stream holds the stream, op->declared is the
 *    Content-Length when no content coding applies (else -1), op->body is allocated with the
 *    declared length (<= the cap, so the (guint) cast is safe) or READ_CHUNK_BYTES, and the first
 *    read is issued.
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

/**
 * attempt_start:
 * @task: (not nullable): the operation's task
 *
 * Starts one attempt of the operation: builds the GET message (no redirects), arms the deadline and
 * the cancellation link and sends it.
 *
 * Preconditions:
 *  - @task has an Op with url set and no attempt state; the caller holds a reference to @task for
 *    the duration of the call; the source object of @task is the StkNasdaq.
 * Postconditions:
 *  - on a malformed URL the task is completed with STK_ERROR_INVALID_ARG and nothing is armed.
 *    Otherwise msg, attempt_cancel, deadline (attached to the task's context), and, when the caller
 *    supplied a cancellable, the user_cancel_id handler exist, and a send is pending; the
 *    references taken for the callbacks are listed in Op.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @task stays with the caller; the new GSource and callbacks take their own task
 *   references; @op owns the new per-attempt objects.
 */
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

/**
 * on_backoff_elapsed:
 * @user_data: (not nullable): the operation's GTask (a GTask, passed as gpointer)
 *
 * GSource callback of the back-off timer: starts the next attempt.
 *
 * Preconditions:
 *  - @user_data is the task reference owned by the timer source's callback data.
 * Postconditions:
 *  - the back-off sources are released and attempt_start() has run; the source is removed. A local
 *    reference keeps the task alive across backoff_cleanup() (which drops the source's own
 *    reference).
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @user_data stays owned by the source; the callback takes and releases its own
 *   reference.
 *
 * Returns: G_SOURCE_REMOVE.
 */
static gboolean
on_backoff_elapsed(gpointer user_data)
{
  g_autoptr(GTask) task = g_object_ref(user_data);
  backoff_cleanup(g_task_get_task_data(task));
  attempt_start(task);
  return G_SOURCE_REMOVE;
}

/**
 * on_backoff_cancelled:
 * @user: (unused): the caller's cancellable
 * @user_data: (not nullable): the operation's GTask (a GTask, passed as gpointer)
 *
 * GCancellableSource callback: the caller cancelled during the back-off wait, so the task completes
 * immediately with G_IO_ERROR_CANCELLED.
 *
 * Preconditions:
 *  - @user_data is the task reference owned by the cancellation source's callback data; a back-off
 *    is pending.
 * Postconditions:
 *  - the back-off sources are released and @task is completed with a G_IO_ERROR /
 *    G_IO_ERROR_CANCELLED error; the source is removed. A local reference keeps the task alive
 *    across backoff_cleanup().
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @user_data stays owned by the source; the callback takes and releases its own
 *   reference.
 *
 * Returns: G_SOURCE_REMOVE.
 */
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

/**
 * schedule_retry:
 * @task: (not nullable): the operation's task
 *
 * Waits backoff_base_ms * 2^(retries so far), then starts the next attempt; a cancellation during
 * the wait completes the task immediately.
 *
 * Preconditions:
 *  - @task has an Op whose attempt state was released and op->attempt < STK_NASDAQ_MAX_RETRIES
 *    (fail_attempt() checks it), so the shift is at most STK_NASDAQ_MAX_RETRIES - 1 and
 *    backoff_base_ms <= BACKOFF_BASE_MAX_MS (stk_nasdaq_new_custom() enforces it), hence the delay
 *    fits a guint (G_STATIC_ASSERT above).
 * Postconditions:
 *  - op->attempt is incremented; a timer source (and, if the caller supplied a cancellable, a
 *    cancellation source) is attached to the task's context, each holding a new task reference
 *    released when the source is finalised.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @task stays with the caller; @op owns the two new sources.
 */
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

/**
 * op_new:
 * @owner: (not nullable): the provider
 * @type: what to parse
 * @url: (transfer full): the request URL
 * @symbol: (nullable): requested symbol (quote operations); copied
 * @kind: requested kind
 * @range: requested range
 * @user_cancellable: (nullable): the caller's cancellable; referenced
 *
 * Creates the Op of one request and counts it in the owner's in-flight total.
 *
 * Preconditions:
 *  - @owner satisfies STK_IS_NASDAQ(); @url is non-NULL and heap-allocated (taken); @symbol is NULL
 *    or NUL-terminated.
 * Postconditions:
 *  - owner->inflight is incremented (admit() has checked the cap); the Op holds a reference to
 *    @owner and to @user_cancellable and owns @url and the symbol copy.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @url; copies @symbol; takes references on @owner and
 *   @user_cancellable; the returned Op is owned by the caller (op_start() hands it to the task).
 *
 * Returns: (transfer full): the new Op.
 */
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

/**
 * admit:
 * @task: (not nullable): the operation's task
 * @self: (not nullable): the provider
 * @url: (transfer full) (nullable): the built request URL, NULL if building failed
 * @url_error: (transfer full) (nullable): the builder's error; non-NULL exactly when @url is NULL
 *   (the URL builders set it on every failure path)
 * @requests: number of GETs the operation needs (1 or 2)
 *
 * Admission control for an operation of @requests GETs. Returns TRUE if it may start. Otherwise
 * @task is completed with the error (@url_error, taken, when @url is NULL; STK_ERROR_RATE_LIMITED
 * when the in-flight cap would be exceeded), @url (taken) is freed, no network traffic happens and
 * FALSE is returned; the caller then drops its reference.
 *
 * Preconditions:
 *  - @task and @self are valid; @requests is 1 or 2; self->inflight is at most
 *    STK_NASDAQ_MAX_INFLIGHT (it only grows through op_new() after this check), so the sum cannot
 *    overflow a guint.
 * Postconditions:
 *  - on TRUE nothing is consumed (the caller keeps @url and @url_error is NULL); on FALSE @task has
 *    been completed with the error and @url / @url_error were consumed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: consumes @url and @url_error on FALSE; @task and @self stay with the caller.
 *
 * Returns: TRUE if the operation may start, FALSE if it was rejected.
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

/**
 * op_start:
 * @task: (transfer full): the operation's task
 * @type: what to parse
 * @url: (transfer full): the request URL
 * @symbol: (nullable): requested symbol; copied
 * @kind: requested kind
 * @range: requested range
 *
 * Starts one admitted request as @task. Takes @url and the caller's reference to @task.
 *
 * Preconditions:
 *  - @task has a StkNasdaq source object and no task data; @url is non-NULL (admit() returned
 *    TRUE).
 * Postconditions:
 *  - the task owns a new Op (freed by op_free()); if the cancellable is already cancelled the task
 *    is completed with G_IO_ERROR_CANCELLED, otherwise the first attempt has started. The caller's
 *    task reference is dropped before returning, the task living on through the references of its
 *    callbacks.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @url and of the caller's reference to @task; @symbol stays with the
 *   caller.
 */
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

/**
 * nasdaq_fetch_quote_async:
 * @provider: (not nullable): the StkNasdaq
 * @symbol: (not nullable): ticker; copied
 * @kind: instrument kind
 * @cancellable: (nullable): cancels the operation
 * @callback: (scope async): called once with the result
 * @user_data: passed to @callback
 *
 * StkProviderInterface.fetch_quote_async: starts a quote request (see
 * stk_provider_fetch_quote_async()).
 *
 * Preconditions:
 *  - @provider is a StkNasdaq (it is reached only through that class's StkProvider vtable);
 *    @symbol is non-NULL (checked by the dispatcher) and NUL-terminated (caller obligation,
 *    not checked).
 * Postconditions:
 *  - @callback is called once. An invalid symbol/kind completes the task with STK_ERROR_INVALID_ARG
 *    and an over-cap request with STK_ERROR_RATE_LIMITED, with no network traffic; otherwise one
 *    GET is in flight.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the task takes references on @provider and @cancellable; @symbol is copied; @user_data
 *   is only passed on.
 */
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

/**
 * nasdaq_fetch_quote_finish:
 * @provider: (not nullable): the StkNasdaq the request was started on
 * @result: (not nullable): the result passed to the callback
 * @error: (nullable): return location for a #GError
 *
 * StkProviderInterface.fetch_quote_finish.
 *
 * Preconditions:
 *  - @result is a GTask created by nasdaq_fetch_quote_async() for @provider and not yet finished
 *    (checked with g_return_val_if_fail()).
 * Postconditions:
 *  - on failure NULL is returned and *@error is set; the task result is consumed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the quote is returned to the caller; @provider and @result stay with the caller.
 *
 * Returns: (transfer full) (nullable): the quote, or NULL with @error set.
 */
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

/*
 * Task data of the series task of a 1D operation. The chart and the quote run as two inner
 * tasks on @child, a cancellable of their own, so that a failed chart can cancel the quote and
 * a caller's cancellation reaches both. Created by series_1d_start(), owned by the series task
 * (freed by join_free()) and used from the main thread only.
 *
 * Ownership: child is a strong reference owned by the Join (the inner tasks and the watch
 * source take their own); user_cancel is a GSource reference owned by the Join whose callback
 * data is a child reference; chart is owned by the Join until join_try_complete() moves it into
 * the series task's result. Both inner tasks hold a reference to the series task as callback
 * data (on_chart_done() / on_prev_close_done() release it), so the series task and this Join
 * outlive both callbacks; `finished` makes a late callback discard its result.
 */
typedef struct {
  GCancellable *child;
  GSource *user_cancel; /* watches the caller's cancellable (nullable) */
  StkSeries *chart;     /* parsed chart, once chart_done; owned until moved to the result */
  double prev_close;    /* NAN until the quote supplied one */
  gboolean chart_done;
  gboolean quote_done;
  gboolean finished; /* the series task has been given its result */
} Join;

/**
 * join_unwatch:
 * @join: (not nullable): the series task's Join
 *
 * Stops watching the caller's cancellable.
 *
 * Preconditions:
 *  - @join is live (owned by its series task); may be called repeatedly.
 * Postconditions:
 *  - join->user_cancel is NULL; its source is destroyed, which releases the child reference held by
 *    its callback data.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the GSource reference owned by @join is released.
 */
static void
join_unwatch(Join *join)
{
  if (join->user_cancel != NULL) {
    g_source_destroy(join->user_cancel);
    g_clear_pointer(&join->user_cancel, g_source_unref);
  }
}

/**
 * join_free:
 * @data: (not nullable): the Join created in series_1d_start()
 *
 * GDestroyNotify of the series task data: releases the Join.
 *
 * Preconditions:
 *  - @data is the Join installed with g_task_set_task_data(); called once, when the series task is
 *    finalised.
 * Postconditions:
 *  - the watch is removed, the child cancellable and an unclaimed chart are released and @data is
 *    freed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @data and of its child, user_cancel and chart members.
 */
static void
join_free(gpointer data)
{
  Join *join = data;
  join_unwatch(join);
  g_clear_object(&join->child);
  g_clear_pointer(&join->chart, stk_series_unref);
  g_free(join);
}

/**
 * join_fail:
 * @series_task: (not nullable): the series task
 * @error: (transfer full): the error to report
 *
 * Fails the series with @error (taken) and cancels whatever is still running.
 *
 * Preconditions:
 *  - @series_task has a Join as task data, is not yet returned (join->finished is FALSE) and the
 *    caller holds a reference to it.
 * Postconditions:
 *  - join->finished is TRUE (later completions of the chart/quote tasks are discarded), the watch
 *    is removed, the child cancellable is cancelled and @series_task is completed with @error.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @error; @series_task stays with the caller.
 */
static void
join_fail(GTask *series_task, GError *error)
{
  Join *join = g_task_get_task_data(series_task);
  join->finished = TRUE;
  join_unwatch(join);
  g_cancellable_cancel(join->child);
  g_task_return_error(series_task, error);
}

/**
 * join_try_complete:
 * @series_task: (not nullable): the series task
 *
 * Completes the series once both the chart and the quote have ended: attaches the previous close to
 * the chart series and returns it.
 *
 * Preconditions:
 *  - @series_task has a Join as task data and is not yet returned. join->chart is non-NULL whenever
 *    join->chart_done is TRUE (on_chart_done() sets both together), and that series is solely owned
 *    by the Join (freshly parsed, never shared), so writing series->prev_close directly is allowed.
 * Postconditions:
 *  - if either part is still pending nothing happens; otherwise join->finished is TRUE, the watch
 *    is removed, the chart series (prev_close set to join->prev_close, NAN when the quote gave
 *    none) is moved out of the Join and @series_task is completed with it.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: ownership of the chart series moves from the Join to the task result.
 */
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

/**
 * on_chart_done:
 * @source: (unused): the StkNasdaq
 * @result: (not nullable): the chart task's result
 * @user_data: (transfer full): the series task reference taken in series_1d_start(), released here
 *
 * GAsyncReadyCallback of the chart sub-task of a 1D series: a chart failure fails the series;
 * success is stored in the Join.
 *
 * Preconditions:
 *  - @user_data is a series task with a Join; @result is a task created in series_1d_start().
 * Postconditions:
 *  - the chart result is always consumed. If the series has already finished (failure or
 *    cancellation) the result is discarded; on failure join_fail() completes the series with the
 *    chart's error; on success the series is stored in join->chart, chart_done is set and
 *    join_try_complete() runs.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the chart series moves into the Join (or is freed); @user_data is released through the
 *   g_autoptr on every path.
 */
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

/**
 * on_prev_close_done:
 * @source: (unused): the StkNasdaq
 * @result: (not nullable): the quote task's result (a heap double or an error)
 * @user_data: (transfer full): the series task reference taken in series_1d_start(), released here
 *
 * GAsyncReadyCallback of the quote sub-task of a 1D series: stores the previous close, if any. Any
 * quote failure, a cancellation included, only leaves prev_close NAN: a caller's cancellation is
 * reported by the series task itself (GTask checks its cancellable when the result is collected).
 *
 * Preconditions:
 *  - @user_data is a series task with a Join; @result is a task created in series_1d_start().
 * Postconditions:
 *  - the result is consumed and freed; unless the series has already finished, join->prev_close is
 *    the value (when one was returned), quote_done is TRUE and join_try_complete() runs.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the heap double is freed here; @user_data is released through the g_autoptr on every
 *   path.
 */
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

/**
 * on_join_user_cancelled:
 * @user: (unused): the caller's cancellable
 * @child: (not nullable): the Join's child GCancellable (passed as gpointer)
 *
 * GCancellableSource callback: the caller cancelled a 1D series, so both sub-requests are cancelled
 * through the child cancellable (from the main loop, never inside the caller's
 * g_cancellable_cancel()).
 *
 * Preconditions:
 *  - @child is the reference taken in series_1d_start() and owned by the source's callback data
 *    (released when the source is destroyed by join_unwatch()).
 * Postconditions:
 *  - the child cancellable is cancelled; the source is removed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: @child stays owned by the source.
 *
 * Returns: G_SOURCE_REMOVE.
 */
static gboolean
on_join_user_cancelled(GCancellable *user, gpointer child)
{
  (void)user;
  g_cancellable_cancel(G_CANCELLABLE(child));
  return G_SOURCE_REMOVE;
}

/**
 * series_1d_start:
 * @series_task: (transfer full): the series task
 * @self: (not nullable): the provider
 * @symbol: the requested symbol; copied
 * @kind: requested kind
 * @chart_url: (transfer full): the chart URL
 *
 * Starts the two requests of a 1D series for @series_task, whose chart URL is @chart_url (taken).
 * Takes the caller's reference to @series_task.
 *
 * Preconditions:
 *  - admit() succeeded for two requests; @chart_url is non-NULL; @symbol and @kind already passed
 *    stk_nasdaq_series_url(), so the quote URL can be built. The caller has checked that the task
 *    is not cancelled.
 * Postconditions:
 *  - normally the series task owns a Join, a chart task (OP_SERIES) and a quote task
 *    (OP_PREV_CLOSE) are running on the Join's child cancellable and each holds a reference to the
 *    series task. If the quote URL could not be built (cannot happen after the chart URL succeeded)
 *    the series task is completed with that builder's error, @chart_url is freed and no request
 *    starts; this replaces the former g_assert().
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: takes ownership of @chart_url and of the caller's reference to @series_task; @symbol
 *   stays with the caller.
 */
static void
series_1d_start(GTask *series_task, StkNasdaq *self, const char *symbol, StkKind kind,
                char *chart_url)
{
  /* The chart URL was built, so symbol and kind are valid and this cannot fail;
   * a failure would still be reported as the series' error. */
  GError *quote_error = NULL;
  char *quote_url = stk_nasdaq_quote_url(self->base_url, symbol, kind, &quote_error);
  if (quote_url == NULL) {
    g_free(chart_url);
    g_task_return_error(series_task, quote_error);
    g_object_unref(series_task);
    return;
  }

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

/**
 * nasdaq_fetch_series_async:
 * @provider: (not nullable): the StkNasdaq
 * @symbol: (not nullable): ticker; copied
 * @kind: instrument kind
 * @range: chart range
 * @cancellable: (nullable): cancels the operation
 * @callback: (scope async): called once with the result
 * @user_data: passed to @callback
 *
 * StkProviderInterface.fetch_series_async: starts a series request (see
 * stk_provider_fetch_series_async()). A 1D series runs a chart and a quote request joined; other
 * ranges one request.
 *
 * Preconditions:
 *  - @provider is a StkNasdaq and @symbol is non-NULL (checked by the dispatcher) and
 *    NUL-terminated (caller obligation, not checked).
 * Postconditions:
 *  - @callback is called once: an invalid argument completes the task with STK_ERROR_INVALID_ARG,
 *    an over-cap request with STK_ERROR_RATE_LIMITED (a 1D series needs two slots), an already
 *    cancelled caller with G_IO_ERROR_CANCELLED, all without network traffic.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the task takes references on @provider and @cancellable; @symbol is copied; @user_data
 *   is only passed on.
 */
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

/**
 * nasdaq_fetch_series_finish:
 * @provider: (not nullable): the StkNasdaq the request was started on
 * @result: (not nullable): the result passed to the callback
 * @error: (nullable): return location for a #GError
 *
 * StkProviderInterface.fetch_series_finish.
 *
 * Preconditions:
 *  - @result is a GTask created by nasdaq_fetch_series_async() for @provider and not yet finished
 *    (checked with g_return_val_if_fail()).
 * Postconditions:
 *  - on failure NULL is returned and *@error is set; the task result is consumed.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the series is returned to the caller; @provider and @result stay with the caller.
 *
 * Returns: (transfer full) (nullable): the series, or NULL with @error set.
 */
static StkSeries *
nasdaq_fetch_series_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_series, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/**
 * nasdaq_search_async:
 * @provider: (not nullable): the StkNasdaq
 * @text: (not nullable): user text; copied into the URL
 * @cancellable: (nullable): cancels the operation
 * @callback: (scope async): called once with the result
 * @user_data: passed to @callback
 *
 * StkProviderInterface.search_async: starts a search request (see stk_provider_search_async()).
 *
 * Preconditions:
 *  - @provider is a StkNasdaq and @text is non-NULL (checked by the dispatcher) and
 *    NUL-terminated (caller obligation, not checked).
 * Postconditions:
 *  - @callback is called once. Unacceptable text completes the task with STK_ERROR_INVALID_ARG and
 *    an over-cap request with STK_ERROR_RATE_LIMITED, with no network traffic; otherwise one GET is
 *    in flight.
 * Thread affinity: main thread only (the thread running the GLib main context)
 * Ownership: the task takes references on @provider and @cancellable; @text is not retained;
 *   @user_data is only passed on.
 */
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

/**
 * nasdaq_search_finish:
 * @provider: (not nullable): the StkNasdaq the request was started on
 * @result: (not nullable): the result passed to the callback
 * @error: (nullable): return location for a #GError
 *
 * StkProviderInterface.search_finish.
 *
 * Preconditions:
 *  - @result is a GTask created by nasdaq_search_async() for @provider and not yet finished
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
nasdaq_search_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == &tag_search, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/**
 * stk_nasdaq_provider_init:
 * @iface: (not nullable): the interface vtable of StkNasdaq
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
stk_nasdaq_provider_init(StkProviderInterface *iface)
{
  iface->fetch_quote_async = nasdaq_fetch_quote_async;
  iface->fetch_quote_finish = nasdaq_fetch_quote_finish;
  iface->fetch_series_async = nasdaq_fetch_series_async;
  iface->fetch_series_finish = nasdaq_fetch_series_finish;
  iface->search_async = nasdaq_search_async;
  iface->search_finish = nasdaq_search_finish;
}
