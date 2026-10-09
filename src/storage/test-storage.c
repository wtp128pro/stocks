/* Offline tests for the storage module: fileio, watchlist and cache. */
#define _XOPEN_SOURCE 700 /* mkfifo(), truncate(), fchmod() under -std=c11 */

#include <fcntl.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "stk-cache.h"
#include "stk-fileio.h"
#include "stk-watchlist.h"

/* ------------------------------------------------------------------------ */
/* Fixture: a private temp directory                                        */
/* ------------------------------------------------------------------------ */

typedef struct {
  char *dir;
} Fixture;

static void
fixture_set_up(Fixture *f, gconstpointer unused)
{
  (void)unused;
  GError *err = NULL;
  f->dir = g_dir_make_tmp("stk-storage-XXXXXX", &err);
  g_assert_no_error(err);
}

static void
rm_rf(const char *path)
{
  GStatBuf st;
  if (g_lstat(path, &st) != 0)
    return;
  if (S_ISDIR(st.st_mode)) {
    (void)g_chmod(path, 0700);
    GDir *d = g_dir_open(path, 0, NULL);
    if (d != NULL) {
      const char *n;
      while ((n = g_dir_read_name(d)) != NULL) {
        char *child = g_build_filename(path, n, NULL);
        rm_rf(child);
        g_free(child);
      }
      g_dir_close(d);
    }
  }
  (void)g_remove(path);
}

static void
fixture_tear_down(Fixture *f, gconstpointer unused)
{
  (void)unused;
  rm_rf(f->dir);
  g_free(f->dir);
}

static char *
path_in(Fixture *f, const char *name)
{
  return g_build_filename(f->dir, name, NULL);
}

static void
put_file(const char *path, const char *data, gsize len)
{
  GError *err = NULL;
  g_assert_true(g_file_set_contents(path, data, (gssize)len, &err));
  g_assert_no_error(err);
}

static void
put_text(const char *path, const char *text)
{
  put_file(path, text, strlen(text));
}

/* Number of directory entries; used to prove no temp file is left behind. */
static guint
count_files(const char *dir)
{
  guint n = 0;
  GDir *d = g_dir_open(dir, 0, NULL);
  g_assert_nonnull(d);
  while (g_dir_read_name(d) != NULL)
    n++;
  g_dir_close(d);
  return n;
}

/* chmod 0500 the directory; returns FALSE (and skips) when that cannot block writes (root). */
static gboolean
make_read_only(const char *dir)
{
  g_assert_cmpint(g_chmod(dir, 0500), ==, 0);
  if (g_access(dir, W_OK) == 0) {
    (void)g_chmod(dir, 0700);
    g_test_skip("directory permissions are not enforced for this user");
    return FALSE;
  }
  return TRUE;
}

static unsigned
file_mode(const char *path)
{
  GStatBuf st;
  g_assert_cmpint(g_stat(path, &st), ==, 0);
  return (unsigned)(st.st_mode & 07777);
}

/* ------------------------------------------------------------------------ */
/* fileio                                                                   */
/* ------------------------------------------------------------------------ */

static void
test_fileio_roundtrip(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "a/b/c/data.bin"); /* parents are created */
  const char data[] = {'h', 'i', '\0', 'x', '\n'};      /* embedded NUL */
  GError *err = NULL;
  g_assert_true(stk_file_write_atomic(path, data, sizeof data, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(file_mode(path), ==, 0600);

  g_autofree char *got = NULL;
  gsize len = 99;
  g_assert_true(stk_file_read_bounded(path, 1024, &got, &len, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(len, ==, sizeof data);
  g_assert_cmpmem(got, len, data, sizeof data);
  g_assert_cmpint(got[len], ==, '\0');

  /* Overwrite with different (shorter) content: the old bytes must not remain. */
  g_assert_true(stk_file_write_atomic(path, "z", 1, &err));
  g_assert_no_error(err);
  g_free(got);
  g_assert_true(stk_file_read_bounded(path, 1024, &got, &len, &err));
  g_assert_cmpuint(len, ==, 1);
  g_assert_cmpstr(got, ==, "z");

  g_autofree char *dir = g_path_get_dirname(path);
  g_assert_cmpuint(count_files(dir), ==, 1); /* no temp file left */

  /* Empty content is valid. */
  g_assert_true(stk_file_write_atomic(path, NULL, 0, &err));
  g_free(got);
  g_assert_true(stk_file_read_bounded(path, 0, &got, &len, &err));
  g_assert_cmpuint(len, ==, 0);
}

static void
test_fileio_read_limits(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "r.txt");
  put_text(path, "0123456789");

  g_autofree char *got = NULL;
  gsize len = 0;
  GError *err = NULL;
  g_assert_true(stk_file_read_bounded(path, 10, &got, &len, &err)); /* exactly at the limit */
  g_assert_cmpuint(len, ==, 10);
  g_free(got);

  g_assert_false(stk_file_read_bounded(path, 9, &got, &len, &err));
  g_assert_error(err, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
  g_assert_null(got);
  g_assert_cmpuint(len, ==, 0);
  g_clear_error(&err);

  g_autofree char *missing = path_in(f, "missing.txt");
  g_assert_false(stk_file_read_bounded(missing, 10, &got, &len, &err));
  g_assert_error(err, G_FILE_ERROR, G_FILE_ERROR_NOENT);
  g_clear_error(&err);

  /* A directory and a FIFO are not regular files; the FIFO must not block. */
  g_assert_false(stk_file_read_bounded(f->dir, 10, &got, &len, &err));
  g_assert_error(err, G_FILE_ERROR, G_FILE_ERROR_INVAL);
  g_clear_error(&err);
  g_autofree char *fifo = path_in(f, "fifo");
  g_assert_cmpint(mkfifo(fifo, 0600), ==, 0);
  g_assert_false(stk_file_read_bounded(fifo, 10, &got, &len, &err));
  g_assert_error(err, G_FILE_ERROR, G_FILE_ERROR_INVAL);
  g_clear_error(&err);

  /* A sparse file far beyond the limit is rejected without reading it. */
  g_autofree char *big = path_in(f, "big");
  put_text(big, "x");
  g_assert_cmpint(truncate(big, (off_t)1 << 40), ==, 0);
  g_assert_false(stk_file_read_bounded(big, 1024, &got, &len, &err));
  g_assert_error(err, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
  g_clear_error(&err);
}

static void
test_fileio_write_failures(Fixture *f, gconstpointer unused)
{
  (void)unused;
  GError *err = NULL;

  /* Target is a non-empty directory: rename() fails after the temp file was
   * fully written; the temp file must be cleaned up and the target untouched. */
  g_autofree char *dirtarget = path_in(f, "target");
  g_autofree char *inner = g_build_filename(dirtarget, "keep", NULL);
  g_assert_cmpint(g_mkdir_with_parents(dirtarget, 0700), ==, 0);
  put_text(inner, "keep");
  g_assert_false(stk_file_write_atomic(dirtarget, "new", 3, &err));
  g_assert_error(err, G_FILE_ERROR, G_FILE_ERROR_ISDIR);
  g_clear_error(&err);
  g_assert_cmpuint(count_files(f->dir), ==, 1);
  g_assert_cmpuint(count_files(dirtarget), ==, 1);

  /* A parent component that is a regular file. */
  g_autofree char *file = path_in(f, "plain");
  put_text(file, "x");
  g_autofree char *below = g_build_filename(file, "sub", "x.json", NULL);
  g_assert_false(stk_file_write_atomic(below, "new", 3, &err));
  g_assert_nonnull(err);
  g_assert_error(err, G_FILE_ERROR, G_FILE_ERROR_NOTDIR);
  g_clear_error(&err);

  /* Read-only target directory: the previous content survives, no stray temp. */
  g_autofree char *rodir = path_in(f, "ro");
  g_assert_cmpint(g_mkdir(rodir, 0700), ==, 0);
  g_autofree char *rofile = g_build_filename(rodir, "data.json", NULL);
  put_text(rofile, "old");
  if (!make_read_only(rodir))
    return;
  g_assert_false(stk_file_write_atomic(rofile, "new", 3, &err));
  g_assert_error(err, G_FILE_ERROR, G_FILE_ERROR_ACCES);
  g_clear_error(&err);
  g_autofree char *got = NULL;
  g_assert_true(g_file_get_contents(rofile, &got, NULL, NULL));
  g_assert_cmpstr(got, ==, "old");
  g_assert_cmpuint(count_files(rodir), ==, 1);
}


static void
set_mtime_ago(const char *path, glong seconds)
{
  struct timeval tv[2];
  gettimeofday(&tv[0], NULL);
  tv[0].tv_sec -= seconds;
  tv[1] = tv[0];
  g_assert_cmpint(utimes(path, tv), ==, 0);
}

static void
test_fileio_stale_temps(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "cache.json");
  put_text(path, "{}");
  g_autofree char *old_tmp = path_in(f, "cache.json.stk-tmp-AbC123");
  g_autofree char *new_tmp = path_in(f, "cache.json.stk-tmp-zzZZ99");
  g_autofree char *other = path_in(f, "cache.json.bak");
  g_autofree char *other2 = path_in(f, "cache.json.stk-tmp-ab-123"); /* not alphanumeric */
  g_autofree char *foreign = path_in(f, "other.json.stk-tmp-AbC123"); /* another file's temp */
  g_autofree char *dir_tmp = path_in(f, "cache.json.stk-tmp-DIR123"); /* not a regular file */
  /* User copies a 6-character suffix could collide with: never removed. */
  const char *user_names[] = { "cache.json.backup", "cache.json.2026Q3", "cache.json.old123",
                               "cache.json.before", "cache.json.AbC123", "cache.json.stk-tmp-AbC1234",
                               "cache.json.stk-tmp-AbC12", "cache.json.stk-tmp-", "cache.json.stk-tmpAbC123" };
  for (guint i = 0; i < G_N_ELEMENTS(user_names); i++) {
    g_autofree char *u = path_in(f, user_names[i]);
    put_text(u, "u");
    set_mtime_ago(u, 3600);
  }
  put_text(old_tmp, "x");
  put_text(new_tmp, "y");
  put_text(other, "z");
  put_text(other2, "z");
  put_text(foreign, "z");
  g_assert_cmpint(g_mkdir(dir_tmp, 0700), ==, 0);
  set_mtime_ago(old_tmp, 3600);
  set_mtime_ago(other, 3600);
  set_mtime_ago(other2, 3600);
  set_mtime_ago(foreign, 3600);
  set_mtime_ago(dir_tmp, 3600);

  stk_file_remove_stale_temps(path);
  g_assert_false(g_file_test(old_tmp, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(new_tmp, G_FILE_TEST_EXISTS)); /* a write may be running */
  g_assert_true(g_file_test(other, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(other2, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(foreign, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(dir_tmp, G_FILE_TEST_IS_DIR));
  g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));

  /* A missing directory is not an error. */
  g_autofree char *nodir = path_in(f, "nodir/x.json");
  stk_file_remove_stale_temps(nodir);

  /* Both loaders clean up as well. */
  put_text(old_tmp, "x");
  set_mtime_ago(old_tmp, 3600);
  g_autoptr(StkCache) c = stk_cache_new(path);
  g_assert_false(stk_cache_load(c, NULL));
  g_assert_false(g_file_test(old_tmp, G_FILE_TEST_EXISTS));
  g_autofree char *wpath = path_in(f, "w.json");
  g_autofree char *wtmp = path_in(f, "w.json.stk-tmp-QqQq00");
  put_text(wtmp, "x");
  set_mtime_ago(wtmp, 3600);
  g_autoptr(StkWatchlist) wl = stk_watchlist_new(wpath);
  g_assert_false(stk_watchlist_load(wl, NULL, NULL));
  g_assert_false(g_file_test(wtmp, G_FILE_TEST_EXISTS));
}

static char *
nested(gsize depth, char open, char close)
{
  GString *g = g_string_new(NULL);
  for (gsize i = 0; i < depth; i++)
    g_string_append_c(g, open);
  for (gsize i = 0; i < depth; i++)
    g_string_append_c(g, close);
  return g_string_free(g, FALSE);
}

static void
test_json_strict(void)
{
  g_autofree char *ok = nested(32, '[', ']');
  g_autofree char *deep = nested(33, '[', ']');
  g_autofree char *deep_obj = g_strconcat("{\"a\":", deep, "}", NULL);
  g_assert_true(stk_json_check_strict(ok, strlen(ok), 32));
  g_assert_false(stk_json_check_strict(deep, strlen(deep), 32));
  g_assert_false(stk_json_check_strict(deep_obj, strlen(deep_obj), 32));
  /* Brackets in strings (with escaped quotes) do not count. */
  const char *str = "[\"[[[[[[\\\"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[\"]";
  g_assert_true(stk_json_check_strict(str, strlen(str), 1));
  g_assert_true(stk_json_check_strict(NULL, 0, 0));
  g_assert_false(stk_json_check_strict("[", 1, 0));

  /* Structure: closers underflow, mismatch, unclosed containers. */
  const char *bad_struct[] = { "]]]][", "[}", "{]", "[[]", "]", "[]]", "{\"a\":[}]", "[{]}" };
  for (guint i = 0; i < G_N_ELEMENTS(bad_struct); i++)
    g_assert_false(stk_json_check_strict(bad_struct[i], strlen(bad_struct[i]), 32));

  /* Lenient json-glib syntax and stray bytes outside strings. */
  const char *bad_lex[] = {
    "['\"',[[[", "/*\"*/[[[", "// \"\n[", "['a']", "{'a':1}", "[1] # x", "[/**/]", "[b]", "[x]",
    "[tru e,\xc3\xa9]", "[\f1]", "[\v1]", "[\x01]", "[1]\x7f", "[\"a\"]\xff", "\0[]",
  };
  for (guint i = 0; i < G_N_ELEMENTS(bad_lex); i++) {
    gsize l = i == G_N_ELEMENTS(bad_lex) - 1 ? 3 : strlen(bad_lex[i]);
    g_assert_false(stk_json_check_strict(bad_lex[i], l, 32));
  }

  /* Strings: raw control bytes, bad/truncated escapes, unterminated. */
  const char *bad_str[] = { "[\"a\nb\"]", "[\"a\tb\"]", "[\"\\x\"]", "[\"\\u12\"]", "[\"\\u12G4\"]",
                            "[\"\\", "[\"abc", "[\"\\\"]", "[\"\\u123" };
  for (guint i = 0; i < G_N_ELEMENTS(bad_str); i++)
    g_assert_false(stk_json_check_strict(bad_str[i], strlen(bad_str[i]), 32));
  g_assert_false(stk_json_check_strict("[\"a\0b\"]", 7, 32)); /* NUL inside a string */

  /* Valid text, including every escape, non-ASCII bytes and ' / # inside strings. */
  const char *good =
    "{\"a\": [1, -2.5e+10, true, false, null, \"x\\\"\\\\\\/\\b\\f\\n\\r\\t\\u00e9\\uD83D\", \"'/#\\\\\", "
    "\"\xc3\xa9\xff\"], \"b\":{}, \"c\":[]}\n\t\r ";
  g_assert_true(stk_json_check_strict(good, strlen(good), 2));
  g_assert_false(stk_json_check_strict(good, strlen(good), 1));

  /* Siblings do not accumulate. */
  GString *many = g_string_new("[");
  for (guint i = 0; i < 5000; i++)
    g_string_append(many, "[],");
  g_string_append(many, "[]]");
  g_assert_true(stk_json_check_strict(many->str, many->len, 2));
  g_string_free(many, TRUE);
}

/* Appends a random JSON value of nesting at most @budget to @out. */
static void
random_value(GString *out, GRand *r, guint budget)
{
  static const char *const ws[] = { "", " ", "\n", "\t ", "\r\n" };
  g_string_append(out, ws[g_rand_int_range(r, 0, 5)]);
  gint kind = g_rand_int_range(r, 0, budget > 0 ? 8 : 5);
  switch (kind) {
  case 0: g_string_append(out, "null"); break;
  case 1: g_string_append(out, g_rand_boolean(r) ? "true" : "false"); break;
  case 2: g_string_append_printf(out, "%d", g_rand_int_range(r, -100000, 100000)); break;
  case 3: g_string_append_printf(out, "%.3e", g_rand_double_range(r, -1e9, 1e9)); break;
  case 4: {
    static const char *const frag[] = { "a", "'", "/", "#", "[", "{", "\\\"", "\\\\", "\\u00e9", "\xc3\xa9",
                                        "/*", "\\n", " ", "]" };
    g_string_append_c(out, '"');
    for (gint i = g_rand_int_range(r, 0, 8); i > 0; i--)
      g_string_append(out, frag[g_rand_int_range(r, 0, 14)]);
    g_string_append_c(out, '"');
    break;
  }
  case 5: case 6: {
    g_string_append_c(out, '[');
    for (gint i = g_rand_int_range(r, 0, 4); i > 0; i--) {
      random_value(out, r, budget - 1);
      g_string_append_c(out, i > 1 ? ',' : ' ');
    }
    g_string_append_c(out, ']');
    break;
  }
  default: {
    g_string_append_c(out, '{');
    for (gint i = g_rand_int_range(r, 0, 4); i > 0; i--) {
      g_string_append_printf(out, "\"k%d\":", i);
      random_value(out, r, budget - 1);
      g_string_append_c(out, i > 1 ? ',' : ' ');
    }
    g_string_append_c(out, '}');
    break;
  }
  }
  g_string_append(out, ws[g_rand_int_range(r, 0, 5)]);
}

static void
test_json_strict_random(void)
{
  GRand *r = g_rand_new_with_seed(12345);
  for (guint n = 0; n < 3000; n++) {
    g_autoptr(GString) doc = g_string_new(NULL);
    guint budget = (guint)g_rand_int_range(r, 0, 12);
    random_value(doc, r, budget);
    g_assert_true(stk_json_check_strict(doc->str, doc->len, 32));
    /* A valid document of depth <= 12 is accepted at exactly its depth and refused one below. */
    guint depth = 0, maxd = 0;
    gboolean in_s = FALSE;
    for (gsize i = 0; i < doc->len; i++) {
      char c = doc->str[i];
      if (in_s) {
        if (c == '\\') i++;
        else if (c == '"') in_s = FALSE;
      } else if (c == '"') in_s = TRUE;
      else if (c == '[' || c == '{') {
        depth++;
        maxd = MAX(maxd, depth);
      }
      else if (c == ']' || c == '}') depth--;
    }
    g_assert_true(stk_json_check_strict(doc->str, doc->len, maxd));
    if (maxd > 0)
      g_assert_false(stk_json_check_strict(doc->str, doc->len, maxd - 1));
    /* Truncation always yields an unclosed container or string: rejected. */
    if (doc->len > 0 && maxd > 0) {
      gsize cut = (gsize)g_rand_int_range(r, 0, (gint)doc->len);
      gboolean complete_prefix = stk_json_check_strict(doc->str, cut, 32);
      /* A strict prefix passes only when it is itself balanced (e.g. cut inside trailing whitespace). */
      if (complete_prefix) {
        guint d2 = 0;
        in_s = FALSE;
        for (gsize i = 0; i < cut; i++) {
          char c = doc->str[i];
          if (in_s) { if (c == '\\') i++; else if (c == '"') in_s = FALSE; }
          else if (c == '"') in_s = TRUE;
          else if (c == '[' || c == '{') d2++;
          else if (c == ']' || c == '}') d2--;
        }
        g_assert_cmpuint(d2, ==, 0);
        g_assert_false(in_s);
      }
    }
    /* A forbidden byte placed between tokens (outside any string) is refused. */
    g_string_prepend_c(doc, "'/#\x01\xff"[g_rand_int_range(r, 0, 5)]);
    g_assert_false(stk_json_check_strict(doc->str, doc->len, 32));
  }
  g_rand_free(r);
}

/* ------------------------------------------------------------------------ */
/* watchlist                                                                */
/* ------------------------------------------------------------------------ */

static void
on_changed(StkWatchlist *wl, gpointer data)
{
  (void)wl;
  (*(guint *)data)++;
}

static void
assert_symbols(StkWatchlist *wl, const char *const *expected)
{
  guint n = 0;
  while (expected[n] != NULL)
    n++;
  g_assert_cmpuint(stk_watchlist_get_n_entries(wl), ==, n);
  for (guint i = 0; i < n; i++)
    g_assert_cmpstr(stk_watchlist_get_entry(wl, i)->symbol, ==, expected[i]);
}

static void
test_watchlist_basic(void)
{
  g_autoptr(StkWatchlist) wl = stk_watchlist_new("/nonexistent/never-used.json");
  guint changed = 0;
  g_signal_connect(wl, "changed", G_CALLBACK(on_changed), &changed);

  g_assert_true(stk_watchlist_add(wl, "aapl", "Apple Inc.", STK_KIND_STOCK));
  g_assert_true(stk_watchlist_add(wl, "FXAIX", NULL, STK_KIND_FUND));
  g_assert_true(stk_watchlist_add(wl, "BRK.B", "Berkshire", STK_KIND_STOCK));
  g_assert_true(stk_watchlist_add(wl, "FBCG", "Fidelity Blue Chip Growth ETF", STK_KIND_ETF));
  g_assert_cmpuint(changed, ==, 4);
  const char *const order1[] = {"AAPL", "FXAIX", "BRK.B", "FBCG", NULL};
  assert_symbols(wl, order1);
  g_assert_cmpstr(stk_watchlist_get_entry(wl, 0)->name, ==, "Apple Inc.");
  g_assert_cmpstr(stk_watchlist_get_entry(wl, 1)->name, ==, "");
  g_assert_cmpint(stk_watchlist_get_entry(wl, 3)->kind, ==, STK_KIND_ETF);
  g_assert_null(stk_watchlist_get_entry(wl, 4));

  /* Duplicates, in any case, are rejected without a signal. */
  g_assert_false(stk_watchlist_add(wl, "AAPL", "x", STK_KIND_STOCK));
  g_assert_false(stk_watchlist_add(wl, "Aapl", "x", STK_KIND_STOCK));
  g_assert_false(stk_watchlist_add(wl, "brk.b", "x", STK_KIND_STOCK));
  g_assert_cmpuint(changed, ==, 4);
  g_assert_true(stk_watchlist_contains(wl, "fxaix"));
  g_assert_cmpint(stk_watchlist_index_of(wl, "fbcg"), ==, 3);
  g_assert_cmpint(stk_watchlist_index_of(wl, "NOPE"), ==, -1);
  g_assert_cmpint(stk_watchlist_index_of(wl, NULL), ==, -1);
  g_assert_cmpint(stk_watchlist_index_of(wl, "../etc"), ==, -1);

  /* Invalid input. */
  g_assert_false(stk_watchlist_add(wl, NULL, "x", STK_KIND_STOCK));
  g_assert_false(stk_watchlist_add(wl, "", "x", STK_KIND_STOCK));
  g_assert_false(stk_watchlist_add(wl, "../../etc/passwd", "x", STK_KIND_STOCK));
  g_assert_false(stk_watchlist_add(wl, "A B", "x", STK_KIND_STOCK));
  g_assert_false(stk_watchlist_add(wl, "TOOLONGSYMBOL12345", "x", STK_KIND_STOCK));
  g_assert_false(stk_watchlist_add(wl, "MSFT", "bad \xff utf8", STK_KIND_STOCK));
  g_autofree char *longname = g_strnfill(STK_WATCHLIST_MAX_NAME_BYTES + 1, 'n');
  g_assert_false(stk_watchlist_add(wl, "MSFT", longname, STK_KIND_STOCK));
  g_assert_false(stk_watchlist_add(wl, "MSFT", "x", (StkKind)99));
  g_assert_false(stk_watchlist_add(wl, "MSFT", "x", (StkKind)(STK_KIND_INDEX + 1)));
  g_autofree char *okname = g_strnfill(STK_WATCHLIST_MAX_NAME_BYTES, 'n');
  g_assert_true(stk_watchlist_add(wl, "MSFT", okname, STK_KIND_STOCK));
  g_assert_cmpuint(changed, ==, 5);
  g_assert_true(stk_watchlist_remove(wl, "msft"));
  g_assert_cmpuint(changed, ==, 6);

  /* Move forwards, backwards, and the no-op / invalid cases. */
  g_assert_true(stk_watchlist_move(wl, 0, 2));
  const char *const order2[] = {"FXAIX", "BRK.B", "AAPL", "FBCG", NULL};
  assert_symbols(wl, order2);
  g_assert_true(stk_watchlist_move(wl, 3, 0));
  const char *const order3[] = {"FBCG", "FXAIX", "BRK.B", "AAPL", NULL};
  assert_symbols(wl, order3);
  g_assert_true(stk_watchlist_move(wl, 1, 2));
  const char *const order4[] = {"FBCG", "BRK.B", "FXAIX", "AAPL", NULL};
  assert_symbols(wl, order4);
  guint before = changed;
  g_assert_false(stk_watchlist_move(wl, 2, 2));
  g_assert_false(stk_watchlist_move(wl, 4, 0));
  g_assert_false(stk_watchlist_move(wl, 0, 4));
  g_assert_cmpuint(changed, ==, before);

  g_assert_true(stk_watchlist_remove(wl, "brk.b"));
  g_assert_false(stk_watchlist_remove(wl, "brk.b"));
  g_assert_false(stk_watchlist_remove(wl, NULL));
  const char *const order5[] = {"FBCG", "FXAIX", "AAPL", NULL};
  assert_symbols(wl, order5);
}

static void
test_watchlist_capacity(void)
{
  g_autoptr(StkWatchlist) wl = stk_watchlist_new("/nonexistent/never-used.json");
  for (guint i = 0; i < STK_WATCHLIST_MAX_ENTRIES; i++) {
    g_autofree char *sym = g_strdup_printf("S%u", i);
    g_assert_true(stk_watchlist_add(wl, sym, NULL, STK_KIND_STOCK));
  }
  g_assert_false(stk_watchlist_add(wl, "ONEMORE", NULL, STK_KIND_STOCK));
  g_assert_cmpuint(stk_watchlist_get_n_entries(wl), ==, STK_WATCHLIST_MAX_ENTRIES);
}

static void
test_watchlist_default_path(void)
{
  g_autoptr(StkWatchlist) wl = stk_watchlist_new(NULL);
  g_autofree char *want = g_build_filename(g_get_user_config_dir(), "stocks", "watchlist.json", NULL);
  g_assert_cmpstr(stk_watchlist_get_path(wl), ==, want);
}

static void
test_watchlist_roundtrip_unicode(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "cfg/stocks/watchlist.json");
  g_autoptr(StkWatchlist) wl = stk_watchlist_new(path);
  const char *names[] = {"Zürich Straße 日本株 ETF 🚀", "quote \" backslash \\ tab \t nl \n", "Fidelity® Fund™", ""};
  const char *syms[] = {"^GSPC", "EURUSD=X", "fxaix", "BF-B"};
  StkKind kinds[] = {STK_KIND_INDEX, STK_KIND_STOCK, STK_KIND_FUND, STK_KIND_STOCK};
  for (guint i = 0; i < 4; i++)
    g_assert_true(stk_watchlist_add(wl, syms[i], names[i], kinds[i]));

  GError *err = NULL;
  g_assert_true(stk_watchlist_save(wl, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(file_mode(path), ==, 0600);

  /* The file is plain UTF-8 JSON with a version. */
  g_autoptr(JsonParser) p = json_parser_new();
  g_assert_true(json_parser_load_from_file(p, path, &err));
  g_assert_no_error(err);
  JsonObject *root = json_node_get_object(json_parser_get_root(p));
  g_assert_cmpint(json_object_get_int_member(root, "version"), ==, 1);

  g_autoptr(StkWatchlist) wl2 = stk_watchlist_new(path);
  guint dropped = 99;
  g_assert_true(stk_watchlist_load(wl2, &dropped, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(dropped, ==, 0);
  g_assert_cmpuint(stk_watchlist_get_n_entries(wl2), ==, 4);
  const char *const upper[] = {"^GSPC", "EURUSD=X", "FXAIX", "BF-B"};
  for (guint i = 0; i < 4; i++) {
    const StkWatchlistEntry *e = stk_watchlist_get_entry(wl2, i);
    g_assert_cmpstr(e->symbol, ==, upper[i]);
    g_assert_cmpstr(e->name, ==, names[i]);
    g_assert_cmpint(e->kind, ==, kinds[i]);
  }
}

static void
test_watchlist_load_errors(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "wl.json");
  g_autoptr(StkWatchlist) wl = stk_watchlist_new(path);
  guint changed = 0;
  g_signal_connect(wl, "changed", G_CALLBACK(on_changed), &changed);
  GError *err = NULL;
  guint dropped = 7;

  /* Missing file: empty list + NOENT, nothing emitted for an empty -> empty load. */
  g_assert_false(stk_watchlist_load(wl, &dropped, &err));
  g_assert_error(err, G_FILE_ERROR, G_FILE_ERROR_NOENT);
  g_assert_cmpuint(dropped, ==, 0);
  g_assert_cmpuint(changed, ==, 0);
  g_clear_error(&err);

  const char *corrupt[] = {
    "",
    "not json at all",
    "{\"version\":1,\"entries\":[{\"symbol\":\"AAPL\"",    /* truncated */
    "[]",
    "null",
    "{}",
    "{\"version\":2,\"entries\":[]}",                       /* unknown version */
    "{\"version\":\"1\",\"entries\":[]}",                   /* version of the wrong type */
    "{\"version\":1.5,\"entries\":[]}",
    "{\"version\":1}",
    "{\"version\":1,\"entries\":{}}",
    "{\"version\":1,\"entries\":\"AAPL\"}",
    "\xff\xfe{\"version\":1,\"entries\":[]}",               /* not UTF-8 */
  };
  for (guint i = 0; i < G_N_ELEMENTS(corrupt); i++) {
    g_assert_true(stk_watchlist_add(wl, "SEED", NULL, STK_KIND_STOCK) || stk_watchlist_contains(wl, "SEED"));
    put_text(path, corrupt[i]);
    dropped = 7;
    g_assert_false(stk_watchlist_load(wl, &dropped, &err));
    g_assert_error(err, STK_ERROR, STK_ERROR_PARSE);
    g_assert_cmpuint(dropped, ==, 0);
    g_assert_cmpuint(stk_watchlist_get_n_entries(wl), ==, 0); /* emptied, never a crash */
    g_clear_error(&err);
  }

  /* Pathological nesting must be rejected, not overflow the stack. */
  g_autofree char *deep = g_strnfill(1000000, '[');
  put_text(path, deep);
  g_assert_false(stk_watchlist_load(wl, NULL, &err));
  g_assert_nonnull(err);
  g_clear_error(&err);

  /* A file bigger than the limit is refused. */
  g_autofree char *huge = g_strnfill(STK_WATCHLIST_MAX_FILE_BYTES + 1, ' ');
  put_file(path, huge, STK_WATCHLIST_MAX_FILE_BYTES + 1);
  g_assert_false(stk_watchlist_load(wl, NULL, &err));
  g_assert_error(err, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
  g_clear_error(&err);

  /* The error argument is optional. */
  put_text(path, "garbage");
  g_assert_false(stk_watchlist_load(wl, NULL, NULL));
}

static void
test_watchlist_load_partial(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "wl.json");
  put_text(path,
           "{\"version\":1,\"entries\":["
           "{\"symbol\":\"aapl\",\"name\":\"Apple\",\"kind\":\"stock\"},"
           "{\"symbol\":\"../../etc/passwd\",\"name\":\"evil\",\"kind\":\"stock\"},"
           "{\"symbol\":\"AAPL\",\"name\":\"dup\",\"kind\":\"stock\"},"
           "{\"symbol\":\"MSFT\",\"name\":\"Microsoft\",\"kind\":\"bond\"},"
           "{\"symbol\":\"A%2fB\",\"name\":\"pct\",\"kind\":\"etf\"},"
           "{\"symbol\":42,\"name\":\"num\",\"kind\":\"etf\"},"
           "{\"symbol\":\"NONAME\",\"kind\":\"etf\"},"
           "\"just a string\","
           "null,"
           "{\"symbol\":\"FXAIX\",\"name\":\"Fidelity 500\",\"kind\":\"fund\"},"
           "{\"symbol\":\"^DJI\",\"name\":\"Dow\",\"kind\":\"index\"},"
           "{\"symbol\":\"SPY\",\"name\":\"ünï\",\"kind\":\"etf\"}"
           "]}");
  g_autoptr(StkWatchlist) wl = stk_watchlist_new(path);
  guint changed = 0;
  g_signal_connect(wl, "changed", G_CALLBACK(on_changed), &changed);
  guint dropped = 0;
  GError *err = NULL;
  g_assert_true(stk_watchlist_load(wl, &dropped, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(dropped, ==, 8);
  g_assert_cmpuint(changed, ==, 1);
  const char *const want[] = {"AAPL", "FXAIX", "^DJI", "SPY", NULL};
  assert_symbols(wl, want);
  g_assert_cmpstr(stk_watchlist_get_entry(wl, 0)->name, ==, "Apple");
  g_assert_cmpint(stk_watchlist_get_entry(wl, 2)->kind, ==, STK_KIND_INDEX);
}

static void
test_watchlist_save_failure(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *dir = path_in(f, "ro");
  g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
  g_autofree char *path = g_build_filename(dir, "watchlist.json", NULL);
  g_autoptr(StkWatchlist) wl = stk_watchlist_new(path);
  g_assert_true(stk_watchlist_add(wl, "AAPL", "Apple", STK_KIND_STOCK));
  GError *err = NULL;
  g_assert_true(stk_watchlist_save(wl, &err));
  g_assert_true(stk_watchlist_add(wl, "MSFT", "Microsoft", STK_KIND_STOCK));
  if (!make_read_only(dir))
    return;
  g_assert_false(stk_watchlist_save(wl, &err));
  g_assert_error(err, G_FILE_ERROR, G_FILE_ERROR_ACCES);
  g_clear_error(&err);

  (void)g_chmod(dir, 0700);
  g_autoptr(StkWatchlist) again = stk_watchlist_new(path);
  g_assert_true(stk_watchlist_load(again, NULL, &err));
  g_assert_cmpuint(stk_watchlist_get_n_entries(again), ==, 1); /* old file intact */
  g_assert_cmpuint(count_files(dir), ==, 1);
}


/* A hostile file of 4 MiB of '[' must fail with a parse error, not crash. */
static void
put_open_brackets(const char *path, gsize n)
{
  g_autofree char *buf = g_malloc(n);
  memset(buf, '[', n);
  put_file(path, buf, n);
}

/* Payloads that hid every later bracket from the old depth scan (a quote
 * inside a comment or a single-quoted string) while json-glib's default
 * scanner still recursed into them. */
static const char *const bypass_prefixes[] = {
  "['\"',", "/*\"*/", "// \"\n", "/* ' */ ['\"',", "'\"'", "# \"\n", "\"\\", "\"\\u12\"",
};

static void
put_prefixed_brackets(const char *path, const char *prefix, gsize total)
{
  gsize plen = strlen(prefix);
  g_assert_cmpuint(plen, <, total);
  g_autofree char *buf = g_malloc(total);
  memcpy(buf, prefix, plen);
  memset(buf + plen, '[', total - plen);
  put_file(path, buf, total);
}

static void
test_watchlist_hostile_nesting(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "wl.json");
  g_autoptr(StkWatchlist) wl = stk_watchlist_new(path);
  GError *err = NULL;
  put_open_brackets(path, STK_WATCHLIST_MAX_FILE_BYTES);
  g_assert_false(stk_watchlist_load(wl, NULL, &err));
  g_assert_error(err, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&err);
  g_assert_cmpuint(stk_watchlist_get_n_entries(wl), ==, 0);

  /* Deep nesting inside an otherwise valid document is rejected too. */
  g_autofree char *deep = nested(40, '[', ']');
  g_autofree char *doc = g_strconcat("{\"version\":1,\"entries\":[], \"x\":", deep, "}", NULL);
  put_text(path, doc);
  g_assert_false(stk_watchlist_load(wl, NULL, &err));
  g_assert_error(err, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&err);
}

static void
test_watchlist_hostile_nesting_bypass(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "wl.json");
  g_autoptr(StkWatchlist) wl = stk_watchlist_new(path);
  for (guint i = 0; i < G_N_ELEMENTS(bypass_prefixes); i++) {
    GError *err = NULL;
    put_prefixed_brackets(path, bypass_prefixes[i], STK_WATCHLIST_MAX_FILE_BYTES - 64);
    g_assert_false(stk_watchlist_load(wl, NULL, &err));
    g_assert_error(err, STK_ERROR, STK_ERROR_PARSE);
    g_clear_error(&err);
    g_assert_cmpuint(stk_watchlist_get_n_entries(wl), ==, 0);
  }
  GError *err = NULL;
  put_text(path, "// c\n{\"version\":1,\"entries\":[]}");
  g_assert_false(stk_watchlist_load(wl, NULL, &err));
  g_assert_error(err, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&err);
  put_text(path, "{'version':1,\"entries\":[]}");
  g_assert_false(stk_watchlist_load(wl, NULL, &err));
  g_assert_error(err, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&err);
}

typedef struct {
  GError *error;
  gboolean ok;
  guint calls;
  gboolean dirty_in_callback;
  StkWatchlist *wl;
} WlAsync;

static void
on_wl_saved(GObject *source, GAsyncResult *res, gpointer data)
{
  WlAsync *st = data;
  st->calls++;
  st->ok = stk_watchlist_save_finish(STK_WATCHLIST(source), res, &st->error);
  st->dirty_in_callback = stk_watchlist_is_dirty(st->wl);
}

static void
wl_wait(WlAsync *st, guint calls)
{
  while (st->calls < calls)
    g_main_context_iteration(NULL, TRUE);
}

static void
test_watchlist_dirty_tracking(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "wl.json");
  g_autoptr(StkWatchlist) wl = stk_watchlist_new(path);
  g_assert_false(stk_watchlist_is_dirty(wl));
  g_assert_false(stk_watchlist_add(wl, "bad symbol", "x", STK_KIND_STOCK));
  g_assert_false(stk_watchlist_is_dirty(wl));
  g_assert_true(stk_watchlist_add(wl, "AAPL", "Apple", STK_KIND_STOCK));
  g_assert_true(stk_watchlist_is_dirty(wl));
  g_assert_true(stk_watchlist_save(wl, NULL));
  g_assert_false(stk_watchlist_is_dirty(wl));
  g_assert_true(stk_watchlist_add(wl, "MSFT", "Microsoft", STK_KIND_STOCK));
  g_assert_true(stk_watchlist_save(wl, NULL));
  g_assert_true(stk_watchlist_move(wl, 0, 1));
  g_assert_true(stk_watchlist_is_dirty(wl));
  g_assert_true(stk_watchlist_save(wl, NULL));
  g_assert_true(stk_watchlist_remove(wl, "AAPL"));
  g_assert_true(stk_watchlist_is_dirty(wl));
  /* A failed or missing-file load leaves the list clean. */
  g_autoptr(StkWatchlist) w2 = stk_watchlist_new(path);
  g_assert_true(stk_watchlist_load(w2, NULL, NULL));
  g_assert_false(stk_watchlist_is_dirty(w2));
  g_autofree char *missing = path_in(f, "missing.json");
  g_autoptr(StkWatchlist) w3 = stk_watchlist_new(missing);
  g_assert_true(stk_watchlist_add(w3, "AAPL", "", STK_KIND_STOCK));
  g_assert_false(stk_watchlist_load(w3, NULL, NULL));
  g_assert_false(stk_watchlist_is_dirty(w3));
}

static void
test_watchlist_save_async(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "cfg/wl.json");
  g_autoptr(StkWatchlist) wl = stk_watchlist_new(path);
  g_assert_true(stk_watchlist_add(wl, "AAPL", "Apple", STK_KIND_STOCK));
  g_assert_true(stk_watchlist_add(wl, "MSFT", "Zürich 日本", STK_KIND_STOCK));

  WlAsync st = {NULL, FALSE, 0, TRUE, wl};
  WlAsync st2 = {NULL, FALSE, 0, TRUE, wl};
  stk_watchlist_save_async(wl, NULL, on_wl_saved, &st);
  /* While one runs, a second save and a load are refused; nothing is invoked synchronously. */
  stk_watchlist_save_async(wl, NULL, on_wl_saved, &st2);
  g_assert_cmpuint(st.calls + st2.calls, ==, 0);
  GError *err = NULL;
  g_assert_false(stk_watchlist_save(wl, &err));
  g_assert_error(err, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error(&err);
  guint dropped = 77;
  g_assert_true(stk_watchlist_is_dirty(wl));
  g_assert_false(stk_watchlist_load(wl, &dropped, &err));
  g_assert_error(err, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error(&err);
  g_assert_cmpuint(dropped, ==, 0); /* out-parameter is set even on the early return */
  g_assert_true(stk_watchlist_is_dirty(wl)); /* refused load: dirty flag unchanged ... */
  g_assert_cmpuint(stk_watchlist_get_n_entries(wl), ==, 2); /* ... and the list untouched */
  /* Editing after the snapshot is allowed and keeps the list dirty. */
  g_assert_true(stk_watchlist_add(wl, "GOOG", "Alphabet", STK_KIND_STOCK));

  wl_wait(&st, 1);
  wl_wait(&st2, 1);
  g_assert_true(st.ok);
  g_assert_no_error(st.error);
  g_assert_true(st.dirty_in_callback);
  g_assert_false(st2.ok);
  g_assert_error(st2.error, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error(&st2.error);
  g_assert_true(stk_watchlist_is_dirty(wl));

  /* The file holds the snapshot (2 entries), not the later edit. */
  g_autoptr(StkWatchlist) wl2 = stk_watchlist_new(path);
  g_assert_true(stk_watchlist_load(wl2, NULL, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(stk_watchlist_get_n_entries(wl2), ==, 2);
  g_assert_cmpstr(stk_watchlist_get_entry(wl2, 1)->name, ==, "Zürich 日本");
  g_assert_false(stk_watchlist_contains(wl2, "GOOG"));

  /* A new save after completion is accepted and clears the dirty flag. */
  st.calls = 0;
  st.ok = FALSE;
  stk_watchlist_save_async(wl, NULL, on_wl_saved, &st);
  wl_wait(&st, 1);
  g_assert_true(st.ok);
  g_assert_false(st.dirty_in_callback);
  g_assert_false(stk_watchlist_is_dirty(wl));
  g_assert_cmpuint(count_files(f->dir), ==, 1);
  g_autofree char *cfg = path_in(f, "cfg");
  g_assert_cmpuint(count_files(cfg), ==, 1);
  g_assert_cmpuint(file_mode(path), ==, 0600);
}

static void
test_watchlist_save_async_failure_and_cancel(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *dir = path_in(f, "ro");
  g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
  g_autofree char *path = g_build_filename(dir, "watchlist.json", NULL);
  g_autoptr(StkWatchlist) wl = stk_watchlist_new(path);
  g_assert_true(stk_watchlist_add(wl, "AAPL", "Apple", STK_KIND_STOCK));

  /* Cancelled before it starts: nothing is written, the list stays dirty. */
  WlAsync st = {NULL, FALSE, 0, FALSE, wl};
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  g_cancellable_cancel(cancel);
  stk_watchlist_save_async(wl, cancel, on_wl_saved, &st);
  wl_wait(&st, 1);
  g_assert_false(st.ok);
  g_assert_error(st.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&st.error);
  g_assert_true(stk_watchlist_is_dirty(wl));
  g_assert_cmpuint(count_files(dir), ==, 0);

  /* Not cancelled, directory not writable: the error is reported, the old file survives. */
  g_assert_true(stk_watchlist_save(wl, NULL));
  g_assert_true(stk_watchlist_add(wl, "MSFT", "Microsoft", STK_KIND_STOCK));
  if (make_read_only(dir)) {
    st.calls = 0;
    stk_watchlist_save_async(wl, NULL, on_wl_saved, &st);
    wl_wait(&st, 1);
    g_assert_false(st.ok);
    g_assert_error(st.error, G_FILE_ERROR, G_FILE_ERROR_ACCES);
    g_clear_error(&st.error);
    g_assert_true(stk_watchlist_is_dirty(wl));
    (void)g_chmod(dir, 0700);
    g_assert_cmpuint(count_files(dir), ==, 1);
  }

  /* After a failure a new save is accepted (the in-flight flag was reset). */
  st.calls = 0;
  stk_watchlist_save_async(wl, NULL, on_wl_saved, &st);
  wl_wait(&st, 1);
  g_assert_true(st.ok);
  g_assert_no_error(st.error);
  g_assert_false(stk_watchlist_is_dirty(wl));
  g_autoptr(StkWatchlist) again = stk_watchlist_new(path);
  g_assert_true(stk_watchlist_load(again, NULL, NULL));
  g_assert_cmpuint(stk_watchlist_get_n_entries(again), ==, 2);
}

/* The list may be released by the caller while a save runs: the task keeps it alive. */
static void
test_watchlist_save_async_unref_while_running(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "wl.json");
  StkWatchlist *wl = stk_watchlist_new(path);
  g_assert_true(stk_watchlist_add(wl, "AAPL", "Apple", STK_KIND_STOCK));
  WlAsync st = {NULL, FALSE, 0, TRUE, wl};
  stk_watchlist_save_async(wl, NULL, on_wl_saved, &st);
  g_object_unref(wl);
  wl_wait(&st, 1);
  g_assert_true(st.ok);
  g_autoptr(StkWatchlist) again = stk_watchlist_new(path);
  g_assert_true(stk_watchlist_load(again, NULL, NULL));
  g_assert_cmpuint(stk_watchlist_get_n_entries(again), ==, 1);
}

/* ------------------------------------------------------------------------ */
/* cache                                                                    */
/* ------------------------------------------------------------------------ */

static gint64
now_ms(void)
{
  return g_get_real_time() / 1000;
}

static StkSeries *
make_series(StkRange range, guint n, double prev_close, guint seed)
{
  StkSeries *s = stk_series_new(range, prev_close, n);
  gint64 t = G_GINT64_CONSTANT(1760000000000) + (gint64)seed * 1000;
  for (guint i = 0; i < n; i++) {
    /* Awkward doubles: not representable in few digits, wide dynamic range. */
    double v = 100.0 + sin((double)(i + seed) * 0.37) * 13.3 + (double)i / 7.0 + 0.1 + 0.2;
    if (i % 50 == 7)
      v = 1e-7 * (double)(i + 1);
    if (i % 50 == 9)
      v = 123456789.123456789;
    g_assert_true(stk_series_append(s, t, v));
    t += (i % 10 == 9) ? 60001 : 60000; /* includes equal-to-neighbour-free but irregular steps */
  }
  return s;
}

/* Round-trip checks compare bit patterns: -0.0 == 0.0 would hide a lost sign. */
static gboolean
same_bits(double a, double b)
{
  return memcmp(&a, &b, sizeof a) == 0;
}

static void
assert_series_equal(const StkSeries *a, const StkSeries *b)
{
  g_assert_cmpint(a->range, ==, b->range);
  if (isnan(a->prev_close))
    g_assert_true(isnan(b->prev_close));
  else
    g_assert_true(same_bits(a->prev_close, b->prev_close));
  g_assert_cmpuint(a->n_points, ==, b->n_points);
  for (gsize i = 0; i < a->n_points; i++) {
    g_assert_true(a->points[i].t_ms == b->points[i].t_ms);
    g_assert_true(same_bits(a->points[i].v, b->points[i].v));
  }
}

static StkQuote *
make_quote(const char *sym, const char *name, StkKind kind)
{
  StkQuote *q = stk_quote_new(sym, name, kind, "USD");
  q->price = 187.31;
  q->change = -1.07;
  q->change_pct = -0.5680000000000001;
  q->as_of_ms = G_GINT64_CONSTANT(1760000123456);
  q->is_realtime = TRUE;
  q->market_open = TRUE;
  return q;
}

static void
assert_quote_equal(const StkQuote *a, const StkQuote *b)
{
  g_assert_cmpstr(a->symbol, ==, b->symbol);
  g_assert_cmpstr(a->name, ==, b->name);
  g_assert_cmpstr(a->currency, ==, b->currency);
  g_assert_cmpint(a->kind, ==, b->kind);
  g_assert_true(a->as_of_ms == b->as_of_ms);
  g_assert_true(a->is_realtime == b->is_realtime);
  g_assert_true(a->market_open == b->market_open);
  const double x[] = {a->price, a->change, a->change_pct};
  const double y[] = {b->price, b->change, b->change_pct};
  for (guint i = 0; i < 3; i++) {
    if (isnan(x[i]))
      g_assert_true(isnan(y[i]));
    else
      g_assert_true(same_bits(x[i], y[i]));
  }
}

static void
put_n_symbols(StkCache *c, guint from, guint to)
{
  for (guint i = from; i < to; i++) {
    g_autofree char *sym = g_strdup_printf("S%04u", i);
    g_autoptr(StkQuote) q = make_quote(sym, "n", STK_KIND_STOCK);
    g_assert_true(stk_cache_put_quote(c, q, now_ms()));
  }
}

static gboolean
has_quote(StkCache *c, const char *sym)
{
  StkQuote *q = stk_cache_get_quote(c, sym, NULL);
  stk_quote_free(q);
  return q != NULL;
}

static void
test_cache_default_path(void)
{
  g_autoptr(StkCache) c = stk_cache_new(NULL);
  g_autofree char *want = g_build_filename(g_get_user_cache_dir(), "stocks", "cache.json", NULL);
  g_assert_cmpstr(stk_cache_get_path(c), ==, want);
}

static void
test_cache_quote_and_age(void)
{
  g_autoptr(StkCache) c = stk_cache_new("/nonexistent/never-used.json");
  g_assert_false(stk_cache_is_dirty(c));
  g_assert_null(stk_cache_get_quote(c, "AAPL", NULL));

  g_autoptr(StkQuote) q = make_quote("aapl", "Äpple Inc. 日本", STK_KIND_STOCK);
  g_assert_true(stk_cache_put_quote(c, q, now_ms() - 5000));
  g_assert_true(stk_cache_is_dirty(c));
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, 1);

  gint64 age = -1;
  g_autoptr(StkQuote) got = stk_cache_get_quote(c, "AaPl", &age);
  g_assert_nonnull(got);
  g_assert_cmpint(age, >=, 5000);
  g_assert_cmpint(age, <, 5000 + 5000);
  assert_quote_equal(q, got);
  g_assert_true(got != q); /* a copy */

  /* A fetch time in the future yields age 0, never negative. */
  g_assert_true(stk_cache_put_quote(c, q, now_ms() + 60000));
  g_autoptr(StkQuote) got2 = stk_cache_get_quote(c, "AAPL", &age);
  g_assert_nonnull(got2);
  g_assert_cmpint(age, ==, 0);

  /* Rejected inputs leave the cache unchanged. */
  g_assert_false(stk_cache_put_quote(c, NULL, 1));
  g_assert_false(stk_cache_put_quote(c, q, -1));
  StkQuote bad = *q;
  char evil[] = "../x";
  bad.symbol = evil;
  g_assert_false(stk_cache_put_quote(c, &bad, 1));
  bad = *q;
  bad.price = INFINITY;
  g_assert_false(stk_cache_put_quote(c, &bad, 1));
  bad = *q;
  bad.as_of_ms = -5;
  g_assert_false(stk_cache_put_quote(c, &bad, 1));
  bad = *q;
  bad.name = (char *)"bad \xff";
  g_assert_false(stk_cache_put_quote(c, &bad, 1));
  bad = *q;
  bad.currency = (char *)"US D";
  g_assert_false(stk_cache_put_quote(c, &bad, 1));
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, 1);
  g_assert_null(stk_cache_get_quote(c, NULL, NULL));
  g_assert_null(stk_cache_get_quote(c, "../x", NULL));
  g_assert_null(stk_cache_get_series(c, "AAPL", STK_RANGE_1D, NULL));
  g_assert_null(stk_cache_get_series(c, "AAPL", (StkRange)42, NULL));
}

static void
test_cache_series_limits(void)
{
  g_autoptr(StkCache) c = stk_cache_new("/nonexistent/never-used.json");
  g_autoptr(StkSeries) ok = make_series(STK_RANGE_1D, STK_CACHE_MAX_SERIES_POINTS, NAN, 0);
  g_autoptr(StkSeries) big = make_series(STK_RANGE_1D, STK_CACHE_MAX_SERIES_POINTS + 1, NAN, 0);
  g_assert_true(stk_cache_put_series(c, "AAPL", ok, 10));
  g_assert_false(stk_cache_put_series(c, "AAPL", big, 10));
  g_assert_false(stk_cache_put_series(c, "../x", ok, 10));
  g_assert_false(stk_cache_put_series(c, "AAPL", ok, -1));
  g_assert_false(stk_cache_put_series(c, "AAPL", NULL, 10));
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, 1);

  /* The series is shared, not copied. */
  g_autoptr(StkSeries) got = stk_cache_get_series(c, "aapl", STK_RANGE_1D, NULL);
  g_assert_true(got == ok);
  g_assert_null(stk_cache_get_series(c, "AAPL", STK_RANGE_1M, NULL));

  /* Replacing the same (symbol, range) repeatedly must not consume the budget. */
  for (guint i = 0; i < 300; i++)
    g_assert_true(stk_cache_put_series(c, "AAPL", ok, 10));
  for (guint i = 0; i < 100; i++) {
    g_autofree char *sym = g_strdup_printf("X%u", i);
    g_assert_true(stk_cache_put_series(c, sym, ok, 10));
  }
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, 101); /* 101 * 2000 < 250000: no eviction */
}

static void
test_cache_lru_symbols(void)
{
  g_autoptr(StkCache) c = stk_cache_new("/nonexistent/never-used.json");
  put_n_symbols(c, 0, STK_CACHE_MAX_SYMBOLS);
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, STK_CACHE_MAX_SYMBOLS);

  /* Reading S0000 makes it the most recently used; S0001 becomes the oldest. */
  g_assert_true(has_quote(c, "S0000"));
  put_n_symbols(c, 1000, 1001);
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, STK_CACHE_MAX_SYMBOLS);
  g_assert_true(has_quote(c, "S0000"));
  g_assert_false(has_quote(c, "S0001"));
  g_assert_true(has_quote(c, "S0002"));
  g_assert_true(has_quote(c, "S1000"));

  /* Updating S0002 also refreshes it: next victim is S0003, then S0004. */
  put_n_symbols(c, 2, 3);
  put_n_symbols(c, 1001, 1003);
  g_assert_false(has_quote(c, "S0003"));
  g_assert_false(has_quote(c, "S0004"));
  g_assert_true(has_quote(c, "S0002"));
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, STK_CACHE_MAX_SYMBOLS);

  /* Eviction drops the symbol's series with it. */
  g_autoptr(StkSeries) s = make_series(STK_RANGE_1M, 10, 1.0, 0);
  g_assert_true(stk_cache_put_series(c, "S0005", s, 1));
  put_n_symbols(c, 2000, 2000 + STK_CACHE_MAX_SYMBOLS);
  g_assert_false(has_quote(c, "S0005"));
  g_assert_null(stk_cache_get_series(c, "S0005", STK_RANGE_1M, NULL));
}

static void
test_cache_lru_points_budget(void)
{
  g_autoptr(StkCache) c = stk_cache_new("/nonexistent/never-used.json");
  g_autoptr(StkSeries) s = make_series(STK_RANGE_1D, STK_CACHE_MAX_SERIES_POINTS, NAN, 0);
  guint per = STK_CACHE_MAX_TOTAL_POINTS / STK_CACHE_MAX_SERIES_POINTS; /* 125 fit exactly */
  for (guint i = 0; i < per; i++) {
    g_autofree char *sym = g_strdup_printf("P%u", i);
    g_assert_true(stk_cache_put_series(c, sym, s, 1));
  }
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, per);
  g_assert_true(stk_cache_put_series(c, "NEWEST", s, 1)); /* over budget: oldest goes */
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, per);
  g_autoptr(StkSeries) gone = stk_cache_get_series(c, "P0", STK_RANGE_1D, NULL);
  g_assert_null(gone);
  g_autoptr(StkSeries) kept = stk_cache_get_series(c, "P1", STK_RANGE_1D, NULL);
  g_assert_nonnull(kept);
  g_autoptr(StkSeries) newest = stk_cache_get_series(c, "NEWEST", STK_RANGE_1D, NULL);
  g_assert_nonnull(newest);
}

static void
test_cache_roundtrip(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "cache/stocks/cache.json");
  g_autoptr(StkCache) c = stk_cache_new(path);

  gint64 fetched = now_ms() - 123456;
  g_autoptr(StkQuote) q = make_quote("AAPL", "Apple \"Inc\" \\ \n 日本 🚀 \x01", STK_KIND_STOCK);
  g_autoptr(StkQuote) qf = make_quote("FXAIX", "Fidelity 500 Index", STK_KIND_FUND);
  g_autoptr(StkQuote) qi = make_quote("^DJI", "Dow Jones Industrial Average", STK_KIND_INDEX);
  qi->price = 44123.45;
  g_assert_cmpint(qi->kind, ==, STK_KIND_INDEX);
  qf->price = NAN; /* unknown values survive as NAN */
  qf->change = NAN;
  qf->change_pct = NAN;
  qf->is_realtime = FALSE;
  qf->market_open = FALSE;
  g_assert_true(stk_cache_put_quote(c, q, fetched));
  g_assert_true(stk_cache_put_quote(c, qf, fetched));
  g_assert_true(stk_cache_put_quote(c, qi, fetched));
  q->kind = (StkKind)(STK_KIND_INDEX + 1); /* out of range: never stored */
  g_assert_false(stk_cache_put_quote(c, q, fetched));
  q->kind = STK_KIND_STOCK;

  g_autoptr(StkSeries) s1d = make_series(STK_RANGE_1D, 400, 186.25, 1);
  g_autoptr(StkSeries) s1m = make_series(STK_RANGE_1M, 22, NAN, 2);
  g_autoptr(StkSeries) s1y = make_series(STK_RANGE_1Y, 252, 0.1 + 0.2, 3);
  g_autoptr(StkSeries) sempty = make_series(STK_RANGE_3M, 0, NAN, 4);
  g_assert_true(stk_cache_put_series(c, "AAPL", s1d, fetched + 1));
  g_assert_true(stk_cache_put_series(c, "AAPL", s1m, fetched + 2));
  g_assert_true(stk_cache_put_series(c, "FXAIX", s1y, fetched + 3));
  g_assert_true(stk_cache_put_series(c, "FXAIX", sempty, fetched + 4));
  g_assert_true(stk_cache_put_series(c, "^GSPC", s1m, fetched + 5)); /* series without a quote */
  g_assert_true(stk_cache_is_dirty(c));

  GError *err = NULL;
  g_assert_true(stk_cache_save(c, &err));
  g_assert_no_error(err);
  g_assert_false(stk_cache_is_dirty(c));
  g_assert_cmpuint(file_mode(path), ==, 0600);

  /* The file is valid JSON, versioned, and compact (arrays, not per-point objects). */
  g_autofree char *text = NULL;
  gsize len = 0;
  g_assert_true(g_file_get_contents(path, &text, &len, &err));
  g_assert_cmpuint(len, <, 400 * 40 + 22 * 40 * 2 + 252 * 40 + 4000);
  g_autoptr(JsonParser) p = json_parser_new();
  g_assert_true(json_parser_load_from_data(p, text, (gssize)len, &err));
  g_assert_no_error(err);
  g_assert_cmpint(json_object_get_int_member(json_node_get_object(json_parser_get_root(p)), "version"), ==, 1);
  g_assert_nonnull(strstr(text, "\"t\":["));

  g_autoptr(StkCache) c2 = stk_cache_new(path);
  g_assert_true(stk_cache_load(c2, &err));
  g_assert_no_error(err);
  g_assert_false(stk_cache_is_dirty(c2));
  g_assert_cmpuint(stk_cache_get_n_symbols(c2), ==, 4);

  gint64 age = 0;
  g_autoptr(StkQuote) lq = stk_cache_get_quote(c2, "AAPL", &age);
  g_assert_nonnull(lq);
  assert_quote_equal(q, lq);
  g_assert_cmpint(age, >=, 123456);
  g_assert_cmpint(age, <, 123456 + 10000);
  g_autoptr(StkQuote) lqf = stk_cache_get_quote(c2, "FXAIX", NULL);
  g_assert_nonnull(lqf);
  assert_quote_equal(qf, lqf);
  g_autoptr(StkQuote) lqi = stk_cache_get_quote(c2, "^DJI", NULL);
  g_assert_nonnull(lqi);
  g_assert_cmpint(lqi->kind, ==, STK_KIND_INDEX);
  assert_quote_equal(qi, lqi);
  g_assert_null(stk_cache_get_quote(c2, "^GSPC", NULL));

  g_autoptr(StkSeries) l1d = stk_cache_get_series(c2, "AAPL", STK_RANGE_1D, &age);
  g_assert_nonnull(l1d);
  g_assert_cmpuint(l1d->n_points, ==, 400);
  assert_series_equal(s1d, l1d);
  g_assert_cmpint(age, >=, 123456 - 1); /* fetched + 1 */
  g_autoptr(StkSeries) l1m = stk_cache_get_series(c2, "AAPL", STK_RANGE_1M, NULL);
  g_assert_nonnull(l1m);
  assert_series_equal(s1m, l1m);
  g_autoptr(StkSeries) l1y = stk_cache_get_series(c2, "FXAIX", STK_RANGE_1Y, NULL);
  g_assert_nonnull(l1y);
  assert_series_equal(s1y, l1y);
  g_autoptr(StkSeries) lempty = stk_cache_get_series(c2, "FXAIX", STK_RANGE_3M, NULL);
  g_assert_nonnull(lempty);
  g_assert_cmpuint(lempty->n_points, ==, 0);
  g_autoptr(StkSeries) lg = stk_cache_get_series(c2, "^GSPC", STK_RANGE_1M, NULL);
  g_assert_nonnull(lg);
  assert_series_equal(s1m, lg);
  g_assert_null(stk_cache_get_series(c2, "AAPL", STK_RANGE_6M, NULL));

  /* Reading does not make the cache dirty. */
  g_assert_false(stk_cache_is_dirty(c2));
  g_assert_cmpuint(count_files(f->dir), ==, 1);
}

static void
test_cache_double_precision(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "c.json");
  g_autoptr(StkCache) c = stk_cache_new(path);
  const double vals[] = {0.1, 0.1 + 0.2, 1.0 / 3.0, 100.0, -5.5, 1e-300, 1.7976931348623157e308, 4.9e-324,
                         123456789012345.6, 0.30000000000000004, 2.2250738585072014e-308, 9007199254740993.0,
                         5e-324, 1e21, 1e22, 123.45, 187.31, 0.0};
  g_autoptr(StkSeries) s = stk_series_new(STK_RANGE_1D, 1.0 / 7.0, 0);
  gint64 t = G_MAXINT64 - 1000; /* extreme timestamps survive as integers */
  for (guint i = 0; i < G_N_ELEMENTS(vals); i++)
    g_assert_true(stk_series_append(s, t + (gint64)i, vals[i]));
  g_assert_true(stk_cache_put_series(c, "EDGE", s, 1));
  g_assert_true(stk_cache_save(c, NULL));

  g_autoptr(StkCache) c2 = stk_cache_new(path);
  GError *err = NULL;
  g_assert_true(stk_cache_load(c2, &err));
  g_assert_no_error(err);
  g_autoptr(StkSeries) got = stk_cache_get_series(c2, "EDGE", STK_RANGE_1D, NULL);
  g_assert_nonnull(got);
  assert_series_equal(s, got);
}

static void
test_cache_lru_order_persists(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "c.json");
  g_autoptr(StkCache) c = stk_cache_new(path);
  put_n_symbols(c, 0, STK_CACHE_MAX_SYMBOLS);
  g_assert_true(has_quote(c, "S0000")); /* S0001 is now the oldest */
  g_assert_true(stk_cache_save(c, NULL));

  g_autoptr(StkCache) c2 = stk_cache_new(path);
  g_assert_true(stk_cache_load(c2, NULL));
  g_assert_cmpuint(stk_cache_get_n_symbols(c2), ==, STK_CACHE_MAX_SYMBOLS);
  put_n_symbols(c2, 5000, 5001);
  g_assert_false(has_quote(c2, "S0001"));
  g_assert_true(has_quote(c2, "S0000"));
  g_assert_true(has_quote(c2, "S0002"));
}

static void
test_cache_load_errors(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "c.json");
  g_autoptr(StkCache) c = stk_cache_new(path);
  GError *err = NULL;

  g_assert_false(stk_cache_load(c, &err));
  g_assert_error(err, G_FILE_ERROR, G_FILE_ERROR_NOENT);
  g_clear_error(&err);
  g_assert_false(stk_cache_is_dirty(c));

  const char *corrupt[] = {
    "",
    "garbage",
    "{\"version\":1,\"entries\":[{\"symbol\":\"AAPL\"",
    "[]",
    "null",
    "{\"entries\":[]}",
    "{\"version\":2,\"entries\":[]}",
    "{\"version\":0,\"entries\":[]}",
    "{\"version\":1,\"entries\":7}",
    "{\"version\":1}",
    "\xff\xfe\xfd",
  };
  for (guint i = 0; i < G_N_ELEMENTS(corrupt); i++) {
    put_n_symbols(c, 0, 3);
    put_text(path, corrupt[i]);
    g_assert_false(stk_cache_load(c, &err));
    g_assert_error(err, STK_ERROR, STK_ERROR_PARSE);
    g_clear_error(&err);
    g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, 0);
    g_assert_false(stk_cache_is_dirty(c));
  }

  g_autofree char *deep = g_strnfill(1000000, '[');
  put_text(path, deep);
  g_assert_false(stk_cache_load(c, &err));
  g_assert_nonnull(err);
  g_clear_error(&err);

  /* Oversized: one byte over the limit, produced sparsely. */
  put_text(path, "x");
  g_assert_cmpint(truncate(path, STK_CACHE_MAX_FILE_BYTES + 1), ==, 0);
  g_assert_false(stk_cache_load(c, &err));
  g_assert_error(err, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
  g_clear_error(&err);
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, 0);

  /* A cache must still be usable after a failed load. */
  put_n_symbols(c, 0, 1);
  g_assert_true(stk_cache_save(c, &err));
  g_assert_no_error(err);
  g_assert_true(stk_cache_load(c, &err));
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, 1);
}

static void
test_cache_load_partial(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "c.json");
  const char *good_q =
    "{\"fetched_at_ms\":1000,\"name\":\"Good\",\"kind\":\"etf\",\"price\":1.5,\"change\":null,"
    "\"change_pct\":2,\"as_of_ms\":5,\"is_realtime\":false,\"market_open\":true,\"currency\":\"USD\"}";
  const char *good_idx_q =
    "{\"fetched_at_ms\":1000,\"name\":\"Idx\",\"kind\":\"index\",\"price\":5000.5,\"change\":1,"
    "\"change_pct\":0.5,\"as_of_ms\":5,\"is_realtime\":false,\"market_open\":true,\"currency\":\"USD\"}";
  g_autofree char *doc = g_strdup_printf(
    "{\"version\":1,\"entries\":["
    "{\"symbol\":\"../evil\",\"quote\":%s},"                                   /* bad symbol */
    "{\"symbol\":42,\"quote\":%s},"
    "{\"symbol\":\"good\",\"quote\":%s,\"series\":{"
    "  \"1D\":{\"fetched_at_ms\":7,\"prev_close\":3.5,\"t\":[1,2,3],\"v\":[1.0,2,3.25]},"
    "  \"1M\":{\"fetched_at_ms\":7,\"prev_close\":null,\"t\":[1,2],\"v\":[1.0]},"            /* length mismatch */
    "  \"3M\":{\"fetched_at_ms\":7,\"prev_close\":null,\"t\":[3,2],\"v\":[1.0,2.0]},"        /* t decreasing */
    "  \"6M\":{\"fetched_at_ms\":7,\"prev_close\":null,\"t\":[1,2],\"v\":[1.0,\"x\"]},"      /* v not a number */
    "  \"1Y\":{\"fetched_at_ms\":-7,\"prev_close\":null,\"t\":[],\"v\":[]},"                 /* negative fetch time */
    "  \"5Y\":{\"fetched_at_ms\":7,\"prev_close\":null,\"t\":[],\"v\":[]}}},"                /* unknown range */
    "{\"symbol\":\"GOOD\",\"quote\":%s},"                                      /* duplicate */
    "{\"symbol\":\"BADKIND\",\"quote\":{\"fetched_at_ms\":1,\"name\":\"n\",\"kind\":\"bond\",\"price\":1,"
    "\"change\":1,\"change_pct\":1,\"as_of_ms\":1,\"is_realtime\":true,\"market_open\":true,\"currency\":\"USD\"}},"
    "{\"symbol\":\"EMPTY\"},"
    "{\"symbol\":\"OKQ\",\"quote\":%s,\"series\":[1,2]},"                      /* series wrong type: quote kept */
    "{\"symbol\":\"^IDX\",\"quote\":%s},"                                      /* index kind is valid */
    "[1,2,3],\"str\",null"
    "]}",
    good_q, good_q, good_q, good_q, good_q, good_idx_q);
  put_text(path, doc);

  g_autoptr(StkCache) c = stk_cache_new(path);
  GError *err = NULL;
  g_assert_true(stk_cache_load(c, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, 3);

  g_autoptr(StkQuote) q = stk_cache_get_quote(c, "GOOD", NULL);
  g_assert_nonnull(q);
  g_assert_cmpstr(q->name, ==, "Good");
  g_assert_cmpint(q->kind, ==, STK_KIND_ETF);
  g_assert_true(isnan(q->change));
  g_assert_true(q->change_pct == 2.0);
  g_assert_true(q->market_open && !q->is_realtime);
  g_autoptr(StkSeries) s = stk_cache_get_series(c, "GOOD", STK_RANGE_1D, NULL);
  g_assert_nonnull(s);
  g_assert_cmpuint(s->n_points, ==, 3);
  g_assert_true(s->prev_close == 3.5);
  g_assert_true(s->points[2].v == 3.25 && s->points[2].t_ms == 3);
  for (int r = STK_RANGE_1M; r <= STK_RANGE_1Y; r++)
    g_assert_null(stk_cache_get_series(c, "GOOD", (StkRange)r, NULL));
  g_assert_true(has_quote(c, "OKQ"));
  g_autoptr(StkQuote) iq = stk_cache_get_quote(c, "^IDX", NULL);
  g_assert_nonnull(iq);
  g_assert_cmpint(iq->kind, ==, STK_KIND_INDEX);
  g_assert_false(has_quote(c, "BADKIND"));
}

static void
test_cache_load_many_entries(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "c.json");
  GString *doc = g_string_new("{\"version\":1,\"entries\":[");
  for (guint i = 0; i < 260; i++)
    g_string_append_printf(doc,
                           "%s{\"symbol\":\"E%u\",\"quote\":{\"fetched_at_ms\":1,\"name\":\"n\",\"kind\":\"stock\","
                           "\"price\":1,\"change\":1,\"change_pct\":1,\"as_of_ms\":1,\"is_realtime\":true,"
                           "\"market_open\":true,\"currency\":\"USD\"}}",
                           i > 0 ? "," : "", i);
  g_string_append(doc, "]}");
  put_file(path, doc->str, doc->len);
  g_string_free(doc, TRUE);

  g_autoptr(StkCache) c = stk_cache_new(path);
  g_assert_true(stk_cache_load(c, NULL));
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, STK_CACHE_MAX_SYMBOLS);
  g_assert_false(has_quote(c, "E59"));
  g_assert_true(has_quote(c, "E60")); /* the 200 most recently used survive */
  g_assert_true(has_quote(c, "E259"));
}

static void
test_cache_negative_zero(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "c.json");
  g_autoptr(StkCache) c = stk_cache_new(path);
  g_autoptr(StkSeries) s = stk_series_new(STK_RANGE_1D, -0.0, 3);
  g_assert_true(stk_series_append(s, 1000, -0.0));
  g_assert_true(stk_series_append(s, 2000, 0.0));
  g_assert_true(stk_series_append(s, 3000, -1e-320));
  g_assert_true(stk_cache_put_series(c, "NZ", s, 1));
  g_autoptr(StkQuote) q = make_quote("NZ", "n", STK_KIND_STOCK);
  q->price = 0.0;
  q->change = -0.0;
  q->change_pct = -0.0;
  g_assert_true(stk_cache_put_quote(c, q, 1));
  g_assert_true(stk_cache_save(c, NULL));

  g_autoptr(StkCache) c2 = stk_cache_new(path);
  g_assert_true(stk_cache_load(c2, NULL));
  g_autoptr(StkSeries) got = stk_cache_get_series(c2, "NZ", STK_RANGE_1D, NULL);
  g_assert_nonnull(got);
  g_assert_true(signbit(got->prev_close));
  g_assert_true(signbit(got->points[0].v));
  g_assert_false(signbit(got->points[1].v));
  assert_series_equal(s, got);
  g_autoptr(StkQuote) gq = stk_cache_get_quote(c2, "NZ", NULL);
  g_assert_nonnull(gq);
  g_assert_false(signbit(gq->price));
  g_assert_true(signbit(gq->change));
  g_assert_true(signbit(gq->change_pct));
  assert_quote_equal(q, gq);
}

static void
test_cache_load_invalid_elements_do_not_count(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "c.json");
  GString *doc = g_string_new("{\"version\":1,\"entries\":[");
  for (guint i = 0; i < 200; i++)
    g_string_append_printf(doc,
                           "%s{\"symbol\":\"E%u\",\"quote\":{\"fetched_at_ms\":1,\"name\":\"n\",\"kind\":\"stock\","
                           "\"price\":1,\"change\":1,\"change_pct\":1,\"as_of_ms\":1,\"is_realtime\":true,"
                           "\"market_open\":true,\"currency\":\"USD\"}}",
                           i > 0 ? "," : "", i);
  /* Junk after the 200 valid ones: null, a bad symbol, a duplicate, an entry without data. */
  g_string_append(doc, ",null,{\"symbol\":\"bad symbol\"},{\"symbol\":\"E5\"},{\"symbol\":\"E6\",\"quote\":3},5]}");
  put_file(path, doc->str, doc->len);
  g_string_free(doc, TRUE);

  g_autoptr(StkCache) c = stk_cache_new(path);
  g_assert_true(stk_cache_load(c, NULL));
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, 200);
  g_assert_true(has_quote(c, "E0"));
  g_assert_true(has_quote(c, "E199"));
}

static void
test_cache_hostile_nesting(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "c.json");
  g_autoptr(StkCache) c = stk_cache_new(path);
  GError *err = NULL;
  put_open_brackets(path, STK_CACHE_MAX_FILE_BYTES);
  g_assert_false(stk_cache_load(c, &err));
  g_assert_error(err, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&err);
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, 0);

  put_open_brackets(path, 4 * 1024 * 1024);
  g_assert_false(stk_cache_load(c, &err));
  g_assert_error(err, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&err);

  g_autofree char *deep = nested(40, '{', '}');
  g_autofree char *doc = g_strconcat("{\"version\":1,\"entries\":[], \"x\":[", deep, "]}", NULL);
  put_text(path, doc);
  g_assert_false(stk_cache_load(c, &err));
  g_assert_error(err, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&err);
}

static void
test_cache_hostile_nesting_bypass(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "c.json");
  g_autoptr(StkCache) c = stk_cache_new(path);
  for (guint i = 0; i < G_N_ELEMENTS(bypass_prefixes); i++) {
    GError *err = NULL;
    put_prefixed_brackets(path, bypass_prefixes[i], STK_CACHE_MAX_FILE_BYTES - 64);
    g_assert_false(stk_cache_load(c, &err));
    g_assert_error(err, STK_ERROR, STK_ERROR_PARSE);
    g_clear_error(&err);
    g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, 0);
  }
  /* Lenient json-glib syntax is refused even in an otherwise valid document. */
  GError *err = NULL;
  put_text(path, "/* c */{\"version\":1,\"entries\":[]}");
  g_assert_false(stk_cache_load(c, &err));
  g_assert_error(err, STK_ERROR, STK_ERROR_PARSE);
  g_clear_error(&err);
  put_text(path, "{\"version\":1,\"entries\":[]}");
  g_assert_true(stk_cache_load(c, NULL));
}

/* A duplicate of a symbol whose first occurrence was already evicted must not
 * be re-admitted (it would displace a valid symbol). */
static void
test_cache_duplicate_of_evicted(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "c.json");
  GString *doc = g_string_new("{\"version\":1,\"entries\":[");
  for (guint i = 0; i <= 200; i++)
    g_string_append_printf(doc,
                           "%s{\"symbol\":\"E%u\",\"quote\":{\"fetched_at_ms\":1,\"name\":\"n\",\"kind\":\"stock\","
                           "\"price\":%u,\"change\":1,\"change_pct\":1,\"as_of_ms\":1,\"is_realtime\":true,"
                           "\"market_open\":true,\"currency\":\"USD\"}}",
                           i > 0 ? "," : "", i, i + 1);
  g_string_append(doc,
                  ",{\"symbol\":\"e0\",\"quote\":{\"fetched_at_ms\":1,\"name\":\"n\",\"kind\":\"stock\","
                  "\"price\":999,\"change\":1,\"change_pct\":1,\"as_of_ms\":1,\"is_realtime\":true,"
                  "\"market_open\":true,\"currency\":\"USD\"}}]}");
  put_file(path, doc->str, doc->len);
  g_string_free(doc, TRUE);

  g_autoptr(StkCache) c = stk_cache_new(path);
  g_assert_true(stk_cache_load(c, NULL));
  g_assert_cmpuint(stk_cache_get_n_symbols(c), ==, 200);
  g_assert_false(has_quote(c, "E0")); /* the LRU valid symbol is ignored ... */
  g_assert_true(has_quote(c, "E1"));  /* ... and the duplicate displaced nobody */
  g_assert_true(has_quote(c, "E200"));
  g_autoptr(StkQuote) q = stk_cache_get_quote(c, "E200", NULL);
  g_assert_nonnull(q);
  g_assert_cmpfloat(q->price, ==, 201.0);
}

static void
test_cache_save_failure(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *dir = path_in(f, "ro");
  g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
  g_autofree char *path = g_build_filename(dir, "cache.json", NULL);
  g_autoptr(StkCache) c = stk_cache_new(path);
  put_n_symbols(c, 0, 2);
  GError *err = NULL;
  g_assert_true(stk_cache_save(c, &err));
  put_n_symbols(c, 2, 3);
  g_assert_true(stk_cache_is_dirty(c));
  if (!make_read_only(dir))
    return;
  g_assert_false(stk_cache_save(c, &err));
  g_assert_error(err, G_FILE_ERROR, G_FILE_ERROR_ACCES);
  g_clear_error(&err);
  g_assert_true(stk_cache_is_dirty(c)); /* a failed save keeps the cache dirty */

  (void)g_chmod(dir, 0700);
  g_autoptr(StkCache) again = stk_cache_new(path);
  g_assert_true(stk_cache_load(again, &err));
  g_assert_cmpuint(stk_cache_get_n_symbols(again), ==, 2); /* previous file intact */
  g_assert_cmpuint(count_files(dir), ==, 1);
  g_assert_true(stk_cache_save(c, &err)); /* and saving works again afterwards */
  g_assert_false(stk_cache_is_dirty(c));
}

/* --- async save --------------------------------------------------------- */

typedef struct {
  GMainLoop *loop;
  StkCache *cache;
  GError *error;
  gboolean ok;
  guint calls;
  gboolean dirty_in_callback;
} AsyncState;

static void
on_saved(GObject *source, GAsyncResult *res, gpointer data)
{
  AsyncState *st = data;
  st->calls++;
  st->ok = stk_cache_save_finish(STK_CACHE(source), res, &st->error);
  st->dirty_in_callback = stk_cache_is_dirty(st->cache);
  g_main_loop_quit(st->loop);
}

static void
test_cache_save_async(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "c.json");
  g_autoptr(StkCache) c = stk_cache_new(path);
  g_autoptr(StkSeries) s = make_series(STK_RANGE_1D, 400, 10.5, 9);
  put_n_symbols(c, 0, 3);
  g_assert_true(stk_cache_put_series(c, "S0000", s, 1));

  AsyncState st = {g_main_loop_new(NULL, FALSE), c, NULL, FALSE, 0, TRUE};
  stk_cache_save_async(c, NULL, on_saved, &st);
  /* A second save, or a load, while one runs is refused; nothing is invoked synchronously. */
  AsyncState st2 = {st.loop, c, NULL, FALSE, 0, TRUE};
  stk_cache_save_async(c, NULL, on_saved, &st2);
  g_assert_cmpuint(st.calls + st2.calls, ==, 0);
  GError *err = NULL;
  g_assert_false(stk_cache_save(c, &err));
  g_assert_error(err, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error(&err);
  g_assert_false(stk_cache_load(c, &err));
  g_assert_error(err, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error(&err);
  /* Mutating after the snapshot is allowed and keeps the cache dirty. */
  put_n_symbols(c, 10, 11);

  while (st.calls == 0 || st2.calls == 0)
    g_main_context_iteration(NULL, TRUE);
  g_assert_true(st.ok);
  g_assert_no_error(st.error);
  g_assert_true(st.dirty_in_callback);
  g_assert_false(st2.ok);
  g_assert_error(st2.error, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error(&st2.error);
  g_assert_true(stk_cache_is_dirty(c));

  /* The file holds the snapshot (3 symbols), not the later put. */
  g_autoptr(StkCache) c2 = stk_cache_new(path);
  g_assert_true(stk_cache_load(c2, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(stk_cache_get_n_symbols(c2), ==, 3);
  g_assert_false(has_quote(c2, "S0010"));
  g_autoptr(StkSeries) got = stk_cache_get_series(c2, "S0000", STK_RANGE_1D, NULL);
  g_assert_nonnull(got);
  assert_series_equal(s, got);

  /* A new save after completion is accepted and clears the dirty flag. */
  st.calls = 0;
  st.ok = FALSE;
  stk_cache_save_async(c, NULL, on_saved, &st);
  while (st.calls == 0)
    g_main_context_iteration(NULL, TRUE);
  g_assert_true(st.ok);
  g_assert_false(st.dirty_in_callback);
  g_assert_false(stk_cache_is_dirty(c));
  g_assert_cmpuint(count_files(f->dir), ==, 1);
  g_main_loop_unref(st.loop);
}

static void
test_cache_save_async_failure_and_cancel(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *dir = path_in(f, "ro");
  g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
  g_autofree char *path = g_build_filename(dir, "cache.json", NULL);
  g_autoptr(StkCache) c = stk_cache_new(path);
  put_n_symbols(c, 0, 2);

  /* Cancelled before it starts: nothing is written, the cache stays dirty. */
  AsyncState st = {g_main_loop_new(NULL, FALSE), c, NULL, FALSE, 0, FALSE};
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  g_cancellable_cancel(cancel);
  stk_cache_save_async(c, cancel, on_saved, &st);
  while (st.calls == 0)
    g_main_context_iteration(NULL, TRUE);
  g_assert_false(st.ok);
  g_assert_error(st.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&st.error);
  g_assert_true(stk_cache_is_dirty(c));
  g_assert_cmpuint(count_files(dir), ==, 0);

  /* Not cancelled, directory not writable: the error is reported. */
  if (make_read_only(dir)) {
    st.calls = 0;
    stk_cache_save_async(c, NULL, on_saved, &st);
    while (st.calls == 0)
      g_main_context_iteration(NULL, TRUE);
    g_assert_false(st.ok);
    g_assert_error(st.error, G_FILE_ERROR, G_FILE_ERROR_ACCES);
    g_clear_error(&st.error);
    g_assert_true(stk_cache_is_dirty(c));
    (void)g_chmod(dir, 0700);
  }

  /* After a failure a new save is accepted (the in-flight flag was reset). */
  st.calls = 0;
  stk_cache_save_async(c, NULL, on_saved, &st);
  while (st.calls == 0)
    g_main_context_iteration(NULL, TRUE);
  g_assert_true(st.ok);
  g_assert_no_error(st.error);
  g_assert_false(stk_cache_is_dirty(c));
  g_main_loop_unref(st.loop);
}

/* ------------------------------------------------------------------------ */
/* Writers never produce what the loaders' strict gate rejects              */
/* ------------------------------------------------------------------------ */

/* Every byte that needs escaping or is a control character: 0x01-0x1f, 0x7f,
 * '"' and '\\'. */
static guint
special_bytes(char out[34])
{
  guint n = 0;
  for (char c = 0x01; c <= 0x1f; c++)
    out[n++] = c;
  out[n++] = 0x7f;
  out[n++] = '"';
  out[n++] = '\\';
  return n;
}

static void
test_json_append_string(void)
{
  char bytes[34];
  guint n = special_bytes(bytes);
  for (guint i = 0; i < n; i++) {
    char text[] = {'a', bytes[i], 'b', '\0'};
    g_autoptr(GString) out = g_string_new(NULL);
    stk_json_append_string(out, text);
    g_assert_true(stk_json_check_strict(out->str, out->len, 1));
    guchar c = (guchar)bytes[i];
    g_autofree char *want = NULL;
    if (c == '"' || c == '\\')
      want = g_strdup_printf("\"a\\%cb\"", c);
    else if (c < 0x20)
      want = g_strdup_printf("\"a\\u%04xb\"", c);
    else
      want = g_strdup_printf("\"a%cb\"", c);
    g_assert_cmpstr(out->str, ==, want);
  }
  g_autoptr(GString) out = g_string_new("x");
  stk_json_append_string(out, "");
  g_assert_cmpstr(out->str, ==, "x\"\"");
}

static void
test_watchlist_control_bytes_roundtrip(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "wl.json");
  g_autoptr(StkWatchlist) wl = stk_watchlist_new(path);
  char bytes[34];
  guint n = special_bytes(bytes);
  g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < n; i++) {
    char *name = g_strdup_printf("Unit%cSeparator Corp", bytes[i]);
    g_ptr_array_add(names, name);
    g_autofree char *sym = g_strdup_printf("B%02X", (guchar)bytes[i]);
    g_assert_true(stk_watchlist_add(wl, sym, name, STK_KIND_STOCK));
  }
  char all[35];
  memcpy(all, bytes, n);
  all[n] = '\0';
  g_ptr_array_add(names, g_strdup(all));
  g_assert_true(stk_watchlist_add(wl, "ALLBYTES", all, STK_KIND_FUND));

  GError *err = NULL;
  g_assert_true(stk_watchlist_save(wl, &err));
  g_assert_no_error(err);

  /* The file passes the loader's own gate, whatever json-glib version wrote... */
  g_autofree char *text = NULL;
  gsize len = 0;
  g_assert_true(g_file_get_contents(path, &text, &len, &err));
  g_assert_no_error(err);
  g_assert_true(stk_json_check_strict(text, len, STK_JSON_MAX_DEPTH));

  g_autoptr(StkWatchlist) wl2 = stk_watchlist_new(path);
  guint dropped = 99;
  g_assert_true(stk_watchlist_load(wl2, &dropped, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(dropped, ==, 0);
  g_assert_cmpuint(stk_watchlist_get_n_entries(wl2), ==, names->len);
  for (guint i = 0; i < names->len; i++)
    g_assert_cmpstr(stk_watchlist_get_entry(wl2, i)->name, ==, g_ptr_array_index(names, i));

  /* The async path serialises the same way. */
  g_autofree char *path2 = path_in(f, "wl-async.json");
  g_autoptr(StkWatchlist) wl3 = stk_watchlist_new(path2);
  g_assert_true(stk_watchlist_add(wl3, "AAPL", "Unit\x1fSeparator Corp", STK_KIND_STOCK));
  WlAsync st = {NULL, FALSE, 0, TRUE, wl3};
  stk_watchlist_save_async(wl3, NULL, on_wl_saved, &st);
  wl_wait(&st, 1);
  g_assert_no_error(st.error);
  g_assert_true(st.ok);
  g_autoptr(StkWatchlist) wl4 = stk_watchlist_new(path2);
  g_assert_true(stk_watchlist_load(wl4, NULL, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(stk_watchlist_get_n_entries(wl4), ==, 1);
  g_assert_cmpstr(stk_watchlist_get_entry(wl4, 0)->name, ==, "Unit\x1fSeparator Corp");
}

static void
test_cache_control_bytes_roundtrip(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "cache.json");
  g_autoptr(StkCache) c = stk_cache_new(path);
  char bytes[34];
  guint n = special_bytes(bytes);
  g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
  gint64 fetched = now_ms();
  for (guint i = 0; i < n; i++) {
    char *name = g_strdup_printf("Unit%cSeparator Corp", bytes[i]);
    g_ptr_array_add(names, name);
    g_autofree char *sym = g_strdup_printf("B%02X", (guchar)bytes[i]);
    g_autoptr(StkQuote) q = make_quote(sym, name, STK_KIND_STOCK);
    g_assert_true(stk_cache_put_quote(c, q, fetched));
  }
  char all[35];
  memcpy(all, bytes, n);
  all[n] = '\0';
  g_ptr_array_add(names, g_strdup(all));
  g_autoptr(StkQuote) qa = make_quote("ALLBYTES", all, STK_KIND_FUND);
  g_assert_true(stk_cache_put_quote(c, qa, fetched));

  GError *err = NULL;
  g_assert_true(stk_cache_save(c, &err));
  g_assert_no_error(err);
  g_autofree char *text = NULL;
  gsize len = 0;
  g_assert_true(g_file_get_contents(path, &text, &len, &err));
  g_assert_no_error(err);
  g_assert_true(stk_json_check_strict(text, len, STK_JSON_MAX_DEPTH));

  g_autoptr(StkCache) c2 = stk_cache_new(path);
  g_assert_true(stk_cache_load(c2, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(stk_cache_get_n_symbols(c2), ==, names->len);
  for (guint i = 0; i < names->len; i++) {
    g_autofree char *sym = i < n ? g_strdup_printf("B%02X", (guchar)bytes[i]) : g_strdup("ALLBYTES");
    g_autoptr(StkQuote) got = stk_cache_get_quote(c2, sym, NULL);
    g_assert_nonnull(got);
    g_assert_cmpstr(got->name, ==, g_ptr_array_index(names, i));
  }
}

static void
test_fileio_stale_temps_symlinks(Fixture *f, gconstpointer unused)
{
  (void)unused;
  g_autofree char *path = path_in(f, "watchlist.json");
  put_text(path, "{}");
  set_mtime_ago(path, 3600);
  g_autofree char *good_link = path_in(f, "watchlist.json.stk-tmp-LINK01");
  g_autofree char *dead_link = path_in(f, "watchlist.json.stk-tmp-LINK02");
  g_autofree char *dir_link = path_in(f, "watchlist.json.stk-tmp-LINK03");
  g_assert_cmpint(symlink("watchlist.json", good_link), ==, 0);
  g_assert_cmpint(symlink("does-not-exist", dead_link), ==, 0);
  g_assert_cmpint(symlink(".", dir_link), ==, 0);
  /* Age the links themselves (not their targets). */
  struct timespec ts[2];
  clock_gettime(CLOCK_REALTIME, &ts[0]);
  ts[0].tv_sec -= 3600;
  ts[1] = ts[0];
  g_assert_cmpint(utimensat(AT_FDCWD, good_link, ts, AT_SYMLINK_NOFOLLOW), ==, 0);
  g_assert_cmpint(utimensat(AT_FDCWD, dead_link, ts, AT_SYMLINK_NOFOLLOW), ==, 0);
  g_assert_cmpint(utimensat(AT_FDCWD, dir_link, ts, AT_SYMLINK_NOFOLLOW), ==, 0);

  stk_file_remove_stale_temps(path);
  /* Only regular files are removed: a symlink is never judged by, or
   * followed to, its target. */
  GStatBuf st;
  g_assert_cmpint(g_lstat(good_link, &st), ==, 0);
  g_assert_true(S_ISLNK(st.st_mode));
  g_assert_cmpint(g_lstat(dead_link, &st), ==, 0);
  g_assert_true(S_ISLNK(st.st_mode));
  g_assert_cmpint(g_lstat(dir_link, &st), ==, 0);
  g_assert_true(S_ISLNK(st.st_mode));
  g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));
}

/* ------------------------------------------------------------------------ */

#define ADD_FIXTURE(name, fn) \
  g_test_add(name, Fixture, NULL, fixture_set_up, fn, fixture_tear_down)

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  ADD_FIXTURE("/fileio/roundtrip", test_fileio_roundtrip);
  ADD_FIXTURE("/fileio/read-limits", test_fileio_read_limits);
  ADD_FIXTURE("/fileio/write-failures", test_fileio_write_failures);
  ADD_FIXTURE("/fileio/stale-temps", test_fileio_stale_temps);
  g_test_add_func("/fileio/json-depth", test_json_strict);
  g_test_add_func("/fileio/json-strict-random", test_json_strict_random);

  g_test_add_func("/watchlist/basic", test_watchlist_basic);
  g_test_add_func("/watchlist/capacity", test_watchlist_capacity);
  g_test_add_func("/watchlist/default-path", test_watchlist_default_path);
  ADD_FIXTURE("/watchlist/roundtrip-unicode", test_watchlist_roundtrip_unicode);
  ADD_FIXTURE("/watchlist/load-errors", test_watchlist_load_errors);
  ADD_FIXTURE("/watchlist/load-partial", test_watchlist_load_partial);
  ADD_FIXTURE("/watchlist/save-failure", test_watchlist_save_failure);
  ADD_FIXTURE("/watchlist/hostile-nesting", test_watchlist_hostile_nesting);
  ADD_FIXTURE("/watchlist/hostile-nesting-bypass", test_watchlist_hostile_nesting_bypass);
  ADD_FIXTURE("/watchlist/dirty-tracking", test_watchlist_dirty_tracking);
  ADD_FIXTURE("/watchlist/save-async", test_watchlist_save_async);
  ADD_FIXTURE("/watchlist/save-async-failure-and-cancel", test_watchlist_save_async_failure_and_cancel);
  ADD_FIXTURE("/watchlist/save-async-unref-while-running", test_watchlist_save_async_unref_while_running);

  g_test_add_func("/cache/default-path", test_cache_default_path);
  g_test_add_func("/cache/quote-and-age", test_cache_quote_and_age);
  g_test_add_func("/cache/series-limits", test_cache_series_limits);
  g_test_add_func("/cache/lru-symbols", test_cache_lru_symbols);
  g_test_add_func("/cache/lru-points-budget", test_cache_lru_points_budget);
  ADD_FIXTURE("/cache/roundtrip", test_cache_roundtrip);
  ADD_FIXTURE("/cache/double-precision", test_cache_double_precision);
  ADD_FIXTURE("/cache/lru-order-persists", test_cache_lru_order_persists);
  ADD_FIXTURE("/cache/load-errors", test_cache_load_errors);
  ADD_FIXTURE("/cache/load-partial", test_cache_load_partial);
  ADD_FIXTURE("/cache/load-many-entries", test_cache_load_many_entries);
  ADD_FIXTURE("/cache/negative-zero", test_cache_negative_zero);
  ADD_FIXTURE("/cache/load-invalid-elements", test_cache_load_invalid_elements_do_not_count);
  ADD_FIXTURE("/cache/hostile-nesting", test_cache_hostile_nesting);
  ADD_FIXTURE("/cache/hostile-nesting-bypass", test_cache_hostile_nesting_bypass);
  ADD_FIXTURE("/cache/duplicate-of-evicted", test_cache_duplicate_of_evicted);
  ADD_FIXTURE("/cache/save-failure", test_cache_save_failure);
  ADD_FIXTURE("/cache/save-async", test_cache_save_async);
  ADD_FIXTURE("/cache/save-async-failure-and-cancel", test_cache_save_async_failure_and_cancel);

  g_test_add_func("/json/append-string", test_json_append_string);
  ADD_FIXTURE("/watchlist/control-bytes-roundtrip", test_watchlist_control_bytes_roundtrip);
  ADD_FIXTURE("/cache/control-bytes-roundtrip", test_cache_control_bytes_roundtrip);
  ADD_FIXTURE("/fileio/stale-temps-symlinks", test_fileio_stale_temps_symlinks);

  return g_test_run();
}
