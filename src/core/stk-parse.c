#include "stk-parse.h"

#include <math.h>
#include <string.h>

#define MAX_TEXT_LEN 63

static gboolean
is_space(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static gboolean
is_digit(char c)
{
  return c >= '0' && c <= '9';
}

/* Trims ASCII whitespace. Returns FALSE when nothing is left or the result is
 * longer than MAX_TEXT_LEN. */
static gboolean
trim(const char *text, const char **begin, const char **end)
{
  if (text == NULL)
    return FALSE;

  const char *b = text;
  while (is_space(*b))
    b++;
  const char *e = b + strlen(b);
  while (e > b && is_space(e[-1]))
    e--;

  if (e == b || (gsize)(e - b) > MAX_TEXT_LEN)
    return FALSE;
  *begin = b;
  *end = e;
  return TRUE;
}

static gboolean
is_na(const char *b, const char *e)
{
  return e - b == 3 && (b[0] == 'N' || b[0] == 'n') && b[1] == '/' &&
         (b[2] == 'A' || b[2] == 'a');
}

/*
 * Scans an unsigned decimal number with strict thousands grouping starting at
 * *p. Copies the digits (commas dropped, '.' kept) into buf (NUL-terminated,
 * capacity MAX_TEXT_LEN + 1) and advances *p. Returns FALSE if no valid number
 * starts at *p.
 */
static gboolean
scan_number(const char **p, const char *end, gboolean allow_fraction, char *buf)
{
  const char *s = *p;
  gsize n = 0;

  const char *int_start = s;
  while (s < end && is_digit(*s))
    buf[n++] = *s++;
  gsize int_digits = (gsize)(s - int_start);
  if (int_digits == 0)
    return FALSE;

  if (s < end && *s == ',') {
    if (int_digits > 3)
      return FALSE;
    while (s < end && *s == ',') {
      s++;
      if (end - s < 3 || !is_digit(s[0]) || !is_digit(s[1]) || !is_digit(s[2]))
        return FALSE;
      if (end - s > 3 && is_digit(s[3]))
        return FALSE;
      buf[n++] = *s++;
      buf[n++] = *s++;
      buf[n++] = *s++;
    }
  }

  if (allow_fraction && s < end && *s == '.') {
    buf[n++] = *s++;
    const char *frac_start = s;
    while (s < end && is_digit(*s))
      buf[n++] = *s++;
    if (s == frac_start)
      return FALSE;
  }

  buf[n] = '\0';
  *p = s;
  return TRUE;
}

/* Shared tail for the double parsers. @p points at the already-validated
 * grammar start (after any sign/'$'). */
static StkParseResult
finish_double(const char *p, const char *end, gboolean negative, gboolean allow_percent,
              double *out)
{
  char buf[MAX_TEXT_LEN + 1];
  if (!scan_number(&p, end, TRUE, buf))
    return STK_PARSE_INVALID;
  if (allow_percent && p < end && *p == '%')
    p++;
  if (p != end)
    return STK_PARSE_INVALID;

  double v = g_ascii_strtod(buf, NULL);
  if (!isfinite(v))
    return STK_PARSE_INVALID;
  *out = (v == 0.0) ? 0.0 : (negative ? -v : v); /* "-0.00" -> +0.0 */
  return STK_PARSE_OK;
}

StkParseResult
stk_parse_price(const char *text, double *out)
{
  g_return_val_if_fail(out != NULL, STK_PARSE_INVALID);
  *out = NAN;

  const char *b, *e;
  if (!trim(text, &b, &e))
    return STK_PARSE_INVALID;
  if (is_na(b, e))
    return STK_PARSE_NA;

  if (*b == '$')
    b++;
  return finish_double(b, e, FALSE, FALSE, out);
}

StkParseResult
stk_parse_change(const char *text, double *out)
{
  g_return_val_if_fail(out != NULL, STK_PARSE_INVALID);
  *out = NAN;

  const char *b, *e;
  if (!trim(text, &b, &e))
    return STK_PARSE_INVALID;
  if (is_na(b, e))
    return STK_PARSE_NA;

  gboolean negative = FALSE;
  if (*b == '+' || *b == '-') {
    negative = (*b == '-');
    b++;
  }
  if (b < e && *b == '$')
    b++;
  return finish_double(b, e, negative, FALSE, out);
}

StkParseResult
stk_parse_percent(const char *text, double *out)
{
  g_return_val_if_fail(out != NULL, STK_PARSE_INVALID);
  *out = NAN;

  const char *b, *e;
  if (!trim(text, &b, &e))
    return STK_PARSE_INVALID;
  if (is_na(b, e))
    return STK_PARSE_NA;

  gboolean negative = FALSE;
  if (*b == '+' || *b == '-') {
    negative = (*b == '-');
    b++;
  }
  return finish_double(b, e, negative, TRUE, out);
}

StkParseResult
stk_parse_volume(const char *text, guint64 *out)
{
  g_return_val_if_fail(out != NULL, STK_PARSE_INVALID);
  *out = 0;

  const char *b, *e;
  if (!trim(text, &b, &e))
    return STK_PARSE_INVALID;
  if (is_na(b, e))
    return STK_PARSE_NA;

  char buf[MAX_TEXT_LEN + 1];
  const char *p = b;
  if (!scan_number(&p, e, FALSE, buf) || p != e)
    return STK_PARSE_INVALID;

  guint64 v = 0;
  for (const char *c = buf; *c != '\0'; c++) {
    guint d = (guint)(*c - '0');
    if (v > (G_MAXUINT64 - d) / 10)
      return STK_PARSE_INVALID;
    v = v * 10 + d;
  }
  *out = v;
  return STK_PARSE_OK;
}
