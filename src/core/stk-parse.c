/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
#include "stk-parse.h"

#include <math.h>
#include <string.h>

#define MAX_TEXT_LEN 63

/**
 * is_space:
 * @c: any byte.
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: TRUE for the ASCII whitespace bytes (space, \t, \n, \r, \f, \v).
 */
static gboolean
is_space(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/**
 * is_digit:
 * @c: any byte.
 *
 * Preconditions:
 *  - none.
 * Postconditions:
 *  - no state is changed.
 * Thread affinity: any thread (pure).
 * Ownership: none (no pointers).
 *
 * Returns: TRUE iff @c is an ASCII digit '0'..'9' (locale independent).
 */
static gboolean
is_digit(char c)
{
  return c >= '0' && c <= '9';
}

/**
 * trim:
 * @text: (nullable): candidate text.
 * @begin: (out): receives the first non-space byte on success.
 * @end: (out): receives one past the last non-space byte on success.
 *
 * Trims ASCII whitespace. The text is walked once up to its terminating NUL
 * (the caller guarantees one). Content is rejected as soon as a non-space byte
 * is found MAX_TEXT_LEN or more bytes after the first non-space byte, so no
 * non-space byte beyond that limit is ever accepted; whitespace (leading,
 * interior and trailing) is skipped without a length limit.
 *
 * Preconditions:
 *  - @text is NULL or NUL-terminated; @begin and @end are non-NULL (the call
 *    sites pass addresses of locals).
 * Postconditions:
 *  - on TRUE: 1 <= *@end - *@begin <= MAX_TEXT_LEN, both pointers lie inside
 *    @text, and *@begin and *@end - 1 are non-space bytes.
 *  - on FALSE: @begin and @end are untouched.
 * Thread affinity: any thread (pure).
 * Ownership: the outputs point into @text; nothing is allocated.
 *
 * Returns: FALSE when @text is NULL, nothing is left after trimming or the
 *   trimmed text is longer than MAX_TEXT_LEN; TRUE otherwise.
 */
static gboolean
trim(const char *text, const char **begin, const char **end)
{
  if (text == NULL)
    return FALSE;

  const char *b = text;
  while (is_space(*b))
    b++;

  /* Walk to the terminating NUL once, remembering the last non-space byte.
   * Content longer than MAX_TEXT_LEN is rejected the moment a non-space byte
   * lies MAX_TEXT_LEN or more bytes after @b. Whitespace (leading, interior or
   * trailing) is walked without a limit. */
  const char *e = b;
  for (const char *c = b; *c != '\0'; c++) {
    if (is_space(*c))
      continue;
    if ((gsize)(c - b) >= MAX_TEXT_LEN)
      return FALSE;
    e = c + 1;
  }

  if (e == b)
    return FALSE;
  *begin = b;
  *end = e;
  return TRUE;
}

/**
 * is_na:
 * @b: start of the trimmed text.
 * @e: one past the end of the trimmed text.
 *
 * Preconditions:
 *  - @b <= @e, both inside the same string (as produced by trim()); at least
 *    @e - @b bytes are readable from @b.
 * Postconditions:
 *  - no state is changed; bytes are only read when @e - @b == 3.
 * Thread affinity: any thread (pure).
 * Ownership: @b and @e are borrowed read-only; nothing is retained.
 *
 * Returns: TRUE iff the range is exactly "N/A" (case-insensitive letters).
 */
static gboolean
is_na(const char *b, const char *e)
{
  return e - b == 3 && (b[0] == 'N' || b[0] == 'n') && b[1] == '/' &&
         (b[2] == 'A' || b[2] == 'a');
}

/**
 * buf_put:
 * @buf: output buffer of capacity MAX_TEXT_LEN + 1 bytes.
 * @n: (inout): number of bytes already stored in @buf.
 * @c: byte to append.
 *
 * Appends one byte, keeping room for the terminating NUL.
 *
 * Preconditions:
 *  - @buf has MAX_TEXT_LEN + 1 writable bytes and *@n <= MAX_TEXT_LEN.
 * Postconditions:
 *  - on TRUE: buf[*@n] was set to @c and *@n incremented (so *@n <=
 *    MAX_TEXT_LEN and buf[*@n] is still in bounds for the NUL).
 *  - on FALSE: nothing written, *@n unchanged.
 * Thread affinity: any thread (pure).
 * Ownership: @buf is caller-owned and written; @n is caller-owned and updated; nothing is
 *   retained.
 *
 * Returns: FALSE if @buf already holds MAX_TEXT_LEN bytes; TRUE otherwise.
 */
static gboolean
buf_put(char *buf, gsize *n, char c)
{
  if (*n >= MAX_TEXT_LEN)
    return FALSE;
  buf[(*n)++] = c;
  return TRUE;
}

/**
 * scan_number:
 * @p: (inout): start of the number; advanced past it on success.
 * @end: one past the last readable byte of the text.
 * @allow_fraction: whether a '.' fraction is part of the grammar.
 * @buf: (out): receives the normalised digits.
 *
 * Scans an unsigned decimal number with strict thousands grouping starting at
 * *p. Copies the digits (commas dropped, '.' kept) into buf (NUL-terminated,
 * capacity MAX_TEXT_LEN + 1) and advances *p. Every store into @buf goes
 * through buf_put(), so the capacity is enforced locally here rather than
 * relying on the caller's 63-byte text limit.
 *
 * Preconditions:
 *  - *@p <= @end, all bytes in [*@p, @end) readable; @buf has MAX_TEXT_LEN + 1
 *    writable bytes.
 * Postconditions:
 *  - on TRUE: @buf holds a NUL-terminated digit string and *@p <= @end.
 *  - on FALSE (no valid number, or it would not fit @buf): *@p is untouched;
 *    @buf contents are unspecified.
 * Thread affinity: any thread (pure).
 * Ownership: *@p is advanced within the caller's text (borrowed); @end is borrowed; @buf is
 *   caller-owned and written; nothing is retained.
 *
 * Returns: FALSE if no valid number starts at *@p, else TRUE.
 */
static gboolean
scan_number(const char **p, const char *end, gboolean allow_fraction, char *buf)
{
  const char *s = *p;
  gsize n = 0;

  const char *int_start = s;
  while (s < end && is_digit(*s)) {
    if (!buf_put(buf, &n, *s))
      return FALSE;
    s++;
  }
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
      for (guint k = 0; k < 3; k++) {
        if (!buf_put(buf, &n, *s))
          return FALSE;
        s++;
      }
    }
  }

  if (allow_fraction && s < end && *s == '.') {
    if (!buf_put(buf, &n, *s))
      return FALSE;
    s++;
    const char *frac_start = s;
    while (s < end && is_digit(*s)) {
      if (!buf_put(buf, &n, *s))
        return FALSE;
      s++;
    }
    if (s == frac_start)
      return FALSE;
  }

  buf[n] = '\0'; /* n <= MAX_TEXT_LEN: in bounds */
  *p = s;
  return TRUE;
}

/**
 * finish_double:
 * @p: already-validated grammar start (after any sign/'$').
 * @end: one past the last byte of the trimmed text.
 * @negative: whether to negate the value.
 * @allow_percent: whether one trailing '%' is part of the grammar.
 * @out: (out): receives the value on STK_PARSE_OK; untouched otherwise.
 *
 * Shared tail for the double parsers: scans the number, checks that the whole
 * text was consumed and converts it.
 *
 * Preconditions:
 *  - @p <= @end, all bytes in [@p, @end) readable; @out non-NULL.
 * Postconditions:
 *  - on STK_PARSE_OK: *@out is finite ("-0.00" yields +0.0); otherwise *@out
 *    is untouched (the callers pre-set it to NAN).
 * Thread affinity: any thread (pure).
 * Ownership: @p and @end are borrowed read-only pointers into the caller's text; @out is
 *   written, nothing is retained.
 *
 * Returns: STK_PARSE_OK or STK_PARSE_INVALID.
 */
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

/* Documented in stk-parse.h. */
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

/* Documented in stk-parse.h. */
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

/* Documented in stk-parse.h. */
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

/* Documented in stk-parse.h. */
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
  for (const char *c = buf; *c != '\0'; c++) { /* buf: NUL-terminated by scan_number() */
    guint d = (guint)(*c - '0');
    if (v > (G_MAXUINT64 - d) / 10)
      return STK_PARSE_INVALID;
    v = v * 10 + d;
  }
  *out = v;
  return STK_PARSE_OK;
}
