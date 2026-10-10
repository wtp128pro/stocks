/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * stk-parse.h - strict parsers for the formatted number strings that quote
 * providers return ("$340.52", "+0.24", "-0.04%", "1,234", "N/A").
 *
 * Common rules for every parser:
 *  - Locale independent (always '.' decimal point, ',' thousands separator).
 *  - Leading/trailing ASCII whitespace is ignored; nothing else is.
 *  - Thousands separators are optional but, when present, must be exact:
 *    1-3 leading digits, then groups of exactly 3 ("1,234", "12,345,678").
 *  - A decimal point must have digits on both sides ("1.", ".5" rejected).
 *  - No exponents, hex, "inf"/"nan", embedded spaces or trailing garbage.
 *  - The case-insensitive marker "N/A" is reported as STK_PARSE_NA, not as an
 *    error, so callers can tell "provider has no value" from "malformed".
 *  - NULL or empty text is STK_PARSE_INVALID.
 *  - Numbers longer than 63 characters (after trimming) are rejected.
 *  - Results are always finite. On anything but STK_PARSE_OK the output is
 *    NAN (doubles) or 0 (volume).
 *
 * Pure functions; no allocation, no global state.
 */
#ifndef STK_PARSE_H
#define STK_PARSE_H

#include <glib.h>

G_BEGIN_DECLS

/** Outcome of a stk_parse_* call. */
typedef enum {
  STK_PARSE_OK,     /**< parsed; *out holds the value                   */
  STK_PARSE_NA,     /**< text is the "N/A" marker; no value available   */
  STK_PARSE_INVALID /**< malformed text; callers map this to
                         STK_ERROR_PARSE                                */
} StkParseResult;

/**
 * stk_parse_price:
 * @text: (nullable): e.g. "$340.52", "340.52", "$1,234.50".
 * @out: (out): non-NULL; receives the value (>= 0), else NAN.
 *
 * Parses a price.
 *
 * Grammar: ['$'] number. No sign: negative prices are rejected, as is a
 * "+" sign. Zero is accepted.
 *
 * Preconditions:
 *  - @out is non-NULL and writable (NULL: critical, STK_PARSE_INVALID).
 *  - @text is NULL or a NUL-terminated string. Text whose trimmed content is
 *    longer than 63 bytes is rejected (no non-space byte at offset 63 or
 *    beyond is accepted); the string is walked once up to its NUL and
 *    whitespace of any length is skipped.
 * Postconditions:
 *  - *@out is always written: the value on STK_PARSE_OK, otherwise NAN.
 *    Parsed values are finite.
 * Thread affinity: any thread (pure).
 * Ownership: nothing is retained or allocated; @text stays with the caller.
 *
 * Returns: STK_PARSE_OK if parsed, STK_PARSE_NA for the "N/A" marker,
 *   STK_PARSE_INVALID for NULL, empty, over-long or malformed text.
 */
StkParseResult stk_parse_price(const char *text, double *out);

/**
 * stk_parse_change:
 * @text: (nullable): absolute change, e.g. "+0.24", "-0.04", "0.00",
 *   "-$1.50", "+$1,234.50", "3.10".
 * @out: (out): non-NULL; receives the (possibly negative) value, else NAN.
 *   "-0.00" yields +0.0.
 *
 * Parses an absolute change.
 *
 * Grammar: ['+'|'-'] ['$'] number. One of the two fields that may be
 * negative.
 *
 * Preconditions:
 *  - @out is non-NULL and writable (NULL: critical, STK_PARSE_INVALID).
 *  - @text is NULL or a NUL-terminated string. Text whose trimmed content is
 *    longer than 63 bytes is rejected (no non-space byte at offset 63 or
 *    beyond is accepted); the string is walked once up to its NUL and
 *    whitespace of any length is skipped.
 * Postconditions:
 *  - *@out is always written: the value on STK_PARSE_OK, otherwise NAN.
 *    Parsed values are finite.
 * Thread affinity: any thread (pure).
 * Ownership: nothing is retained or allocated; @text stays with the caller.
 *
 * Returns: STK_PARSE_OK if parsed, STK_PARSE_NA for the "N/A" marker,
 *   STK_PARSE_INVALID for NULL, empty, over-long or malformed text.
 */
StkParseResult stk_parse_change(const char *text, double *out);

/**
 * stk_parse_percent:
 * @text: (nullable): percent change, e.g. "-0.04%", "+0.24%", "1.5", "0%".
 * @out: (out): non-NULL; receives the percentage in percent units
 *   ("-0.04%" -> -0.04), else NAN. "-0.00%" yields +0.0.
 *
 * Parses a percentage.
 *
 * Grammar: ['+'|'-'] number ['%']. The percent sign is optional because
 * providers are inconsistent, but at most one is allowed and only at the end.
 * One of the two fields that may be negative.
 *
 * Preconditions:
 *  - @out is non-NULL and writable (NULL: critical, STK_PARSE_INVALID).
 *  - @text is NULL or a NUL-terminated string. Text whose trimmed content is
 *    longer than 63 bytes is rejected (no non-space byte at offset 63 or
 *    beyond is accepted); the string is walked once up to its NUL and
 *    whitespace of any length is skipped.
 * Postconditions:
 *  - *@out is always written: the value on STK_PARSE_OK, otherwise NAN.
 *    Parsed values are finite.
 * Thread affinity: any thread (pure).
 * Ownership: nothing is retained or allocated; @text stays with the caller.
 *
 * Returns: STK_PARSE_OK if parsed, STK_PARSE_NA for the "N/A" marker,
 *   STK_PARSE_INVALID for NULL, empty, over-long or malformed text.
 */
StkParseResult stk_parse_percent(const char *text, double *out);

/**
 * stk_parse_volume:
 * @text: (nullable): share volume, e.g. "1,234", "0", "48123456".
 * @out: (out): non-NULL; receives the count, else 0.
 *
 * Parses a share volume.
 *
 * Grammar: unsigned integer. No sign, '$' or decimal point. Values above
 * G_MAXUINT64 are rejected.
 *
 * Preconditions:
 *  - @out is non-NULL and writable (NULL: critical, STK_PARSE_INVALID).
 *  - @text is NULL or a NUL-terminated string. Text whose trimmed content is
 *    longer than 63 bytes is rejected (no non-space byte at offset 63 or
 *    beyond is accepted); the string is walked once up to its NUL and
 *    whitespace of any length is skipped.
 * Postconditions:
 *  - *@out is always written: the value on STK_PARSE_OK, otherwise 0.
 * Thread affinity: any thread (pure).
 * Ownership: nothing is retained or allocated; @text stays with the caller.
 *
 * Returns: STK_PARSE_OK if parsed, STK_PARSE_NA for the "N/A" marker,
 *   STK_PARSE_INVALID for NULL, empty, over-long, malformed or overflowing
 *   text.
 */
StkParseResult stk_parse_volume(const char *text, guint64 *out);

G_END_DECLS

#endif /* STK_PARSE_H */
