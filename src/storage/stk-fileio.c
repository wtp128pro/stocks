/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
#define _XOPEN_SOURCE 700 /* fsync(), O_DIRECTORY, O_CLOEXEC under -std=c11 */

#include "stk-fileio.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void
set_errno_error(GError **error, int err, const char *what, const char *path)
{
  g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(err), "%s '%s': %s", what,
              path, g_strerror(err));
}

/* Best effort: make the rename durable. Failures are deliberately ignored. */
static void
sync_directory(const char *dir)
{
  int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0)
    return;
  (void)fsync(fd);
  (void)close(fd);
}

gboolean
stk_file_write_atomic(const char *path, const char *data, gsize len, GError **error)
{
  g_return_val_if_fail(path != NULL && path[0] != '\0', FALSE);
  g_return_val_if_fail(data != NULL || len == 0, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  g_autofree char *dir = g_path_get_dirname(path);
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    set_errno_error(error, errno, "cannot create directory", dir);
    return FALSE;
  }

  g_autofree char *tmp = g_strconcat(path, STK_TEMP_INFIX, "XXXXXX", NULL);
  int fd = g_mkstemp_full(tmp, O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0) {
    set_errno_error(error, errno, "cannot create temporary file for", path);
    return FALSE;
  }

  int err = 0;
  gsize done = 0;
  while (done < len) {
    ssize_t n = write(fd, data + done, len - done);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      err = errno;
      break;
    }
    done += (gsize)n;
  }
  if (err == 0 && fsync(fd) != 0)
    err = errno;
  if (close(fd) != 0 && err == 0)
    err = errno;
  if (err != 0) {
    set_errno_error(error, err, "cannot write", tmp);
    (void)g_unlink(tmp);
    return FALSE;
  }

  if (rename(tmp, path) != 0) {
    set_errno_error(error, errno, "cannot replace", path);
    (void)g_unlink(tmp);
    return FALSE;
  }
  sync_directory(dir);
  return TRUE;
}

gboolean
stk_file_read_bounded(const char *path, gsize max_bytes, char **contents, gsize *length,
                      GError **error)
{
  g_return_val_if_fail(path != NULL && path[0] != '\0', FALSE);
  g_return_val_if_fail(contents != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail(max_bytes < G_MAXSIZE / 4, FALSE);

  *contents = NULL;
  if (length != NULL)
    *length = 0;

  /* O_NONBLOCK: opening a FIFO must not hang before the type check below. */
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    set_errno_error(error, errno, "cannot open", path);
    return FALSE;
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    set_errno_error(error, errno, "cannot stat", path);
    (void)close(fd);
    return FALSE;
  }
  if (!S_ISREG(st.st_mode)) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "'%s' is not a regular file", path);
    (void)close(fd);
    return FALSE;
  }
  if (st.st_size < 0 || (guint64)st.st_size > max_bytes) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                "'%s' is larger than the %" G_GSIZE_FORMAT " byte limit", path, max_bytes);
    (void)close(fd);
    return FALSE;
  }

  /* Never read more than limit = max_bytes + 1 bytes: reaching it proves the
   * file (which may have grown since fstat) exceeds max_bytes. One more byte
   * is reserved for the terminating NUL. */
  const gsize limit = max_bytes + 1;
  gsize alloc = MIN((gsize)st.st_size + 1, limit) + 1;
  char *buf = g_malloc(alloc);
  gsize used = 0;
  int err = 0;
  for (;;) {
    if (used == alloc - 1) {
      if (used >= limit)
        break;
      alloc = MIN(alloc * 2, limit + 1);
      buf = g_realloc(buf, alloc);
    }
    ssize_t n = read(fd, buf + used, alloc - 1 - used);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      err = errno;
      break;
    }
    if (n == 0)
      break;
    used += (gsize)n;
  }
  (void)close(fd);

  if (err != 0) {
    set_errno_error(error, err, "cannot read", path);
    g_free(buf);
    return FALSE;
  }
  if (used > max_bytes) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                "'%s' is larger than the %" G_GSIZE_FORMAT " byte limit", path, max_bytes);
    g_free(buf);
    return FALSE;
  }
  buf[used] = '\0';
  *contents = buf;
  if (length != NULL)
    *length = used;
  return TRUE;
}

#define STALE_TEMP_AGE_SECONDS 60
#define TEMP_SUFFIX_LEN 6

/* Matches exactly "<base>" + STK_TEMP_INFIX + six ASCII letters/digits. */
static gboolean
is_temp_name(const char *name, const char *base, gsize base_len)
{
  const gsize infix_len = strlen(STK_TEMP_INFIX);
  if (strncmp(name, base, base_len) != 0 ||
      strncmp(name + base_len, STK_TEMP_INFIX, infix_len) != 0)
    return FALSE;
  const char *suffix = name + base_len + infix_len;
  if (strlen(suffix) != TEMP_SUFFIX_LEN)
    return FALSE;
  for (gsize i = 0; i < TEMP_SUFFIX_LEN; i++)
    if (!g_ascii_isalnum(suffix[i]))
      return FALSE;
  return TRUE;
}

void
stk_file_remove_stale_temps(const char *path)
{
  g_return_if_fail(path != NULL && path[0] != '\0');

  g_autofree char *dir = g_path_get_dirname(path);
  g_autofree char *base = g_path_get_basename(path);
  const gsize base_len = strlen(base);
  GDir *d = g_dir_open(dir, 0, NULL);
  if (d == NULL)
    return;
  const time_t now = time(NULL);
  const char *name;
  while ((name = g_dir_read_name(d)) != NULL) {
    if (!is_temp_name(name, base, base_len))
      continue;
    g_autofree char *full = g_build_filename(dir, name, NULL);
    struct stat st;
    if (lstat(full, &st) == 0 && S_ISREG(st.st_mode) && now - st.st_mtime > STALE_TEMP_AGE_SECONDS)
      (void)g_unlink(full);
  }
  g_dir_close(d);
}

/* Pops the innermost container: returns FALSE if none is open or if it is not
 * of the kind @want (bit 1 = object, bit 0 = array). */
static gboolean
pop_container(guint32 *kinds, guint *depth, guint32 want)
{
  if (*depth == 0)
    return FALSE;
  (*depth)--;
  return ((*kinds >> *depth) & 1u) == want;
}

gboolean
stk_json_check_strict(const char *text, gsize len, guint max_depth)
{
  g_return_val_if_fail(text != NULL || len == 0, FALSE);
  /* The container kinds live in one bit each of a 32-bit word. */
  g_return_val_if_fail(max_depth <= 32, FALSE);

  guint32 kinds = 0; /* bit i: container at depth i is an object */
  guint depth = 0;
  for (gsize i = 0; i < len; i++) {
    const unsigned char c = (unsigned char)text[i];
    switch (c) {
    case ' ': case '\t': case '\n': case '\r':
    case ',': case ':':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
    case '-': case '+': case '.': case 'e': case 'E':
    case 't': case 'r': case 'u': case 'f': case 'a': case 'l': case 's': case 'n':
      break;
    case '[':
    case '{':
      if (depth >= max_depth)
        return FALSE;
      if (c == '{')
        kinds |= (guint32)1 << depth;
      else
        kinds &= ~((guint32)1 << depth);
      depth++;
      break;
    case ']':
      if (!pop_container(&kinds, &depth, 0))
        return FALSE;
      break;
    case '}':
      if (!pop_container(&kinds, &depth, 1))
        return FALSE;
      break;
    case '"':
      for (i++;; i++) {
        if (i >= len)
          return FALSE; /* unterminated string */
        const unsigned char s = (unsigned char)text[i];
        if (s == '"')
          break;
        if (s < 0x20)
          return FALSE;
        if (s != '\\')
          continue;
        if (++i >= len)
          return FALSE;
        switch (text[i]) {
        case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
          break;
        case 'u':
          if (len - i < 5)
            return FALSE;
          for (int k = 1; k <= 4; k++)
            if (!g_ascii_isxdigit(text[i + k]))
              return FALSE;
          i += 4;
          break;
        default:
          return FALSE;
        }
      }
      break;
    default:
      return FALSE; /* ' / # NUL, other controls, non-ASCII, letters outside true/false/null */
    }
  }
  return depth == 0;
}

void
stk_json_append_string(GString *out, const char *s)
{
  g_return_if_fail(out != NULL && s != NULL);

  g_string_append_c(out, '"');
  for (; *s != '\0'; s++) {
    guchar c = (guchar)*s;
    if (c == '"' || c == '\\') {
      g_string_append_c(out, '\\');
      g_string_append_c(out, (char)c);
    } else if (c < 0x20) {
      g_string_append_printf(out, "\\u%04x", c);
    } else {
      g_string_append_c(out, (char)c);
    }
  }
  g_string_append_c(out, '"');
}
