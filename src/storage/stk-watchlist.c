/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
#include "stk-watchlist.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "stk-fileio.h"

#define WATCHLIST_VERSION 1

struct _StkWatchlist {
  GObject parent_instance;
  char *path;
  GPtrArray *entries; /* StkWatchlistEntry*, freed with entry_free */
  guint64 gen;        /* bumped by every successful add/remove/move */
  guint64 saved_gen;  /* gen captured by the last successful load/save snapshot */
  gboolean save_in_flight;
};

/* Source tag of stk_watchlist_save_async() tasks; only its address is used. */
static char save_tag;

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(StkWatchlist, stk_watchlist, G_TYPE_OBJECT)

static void
entry_free(gpointer data)
{
  StkWatchlistEntry *e = data;
  g_free(e->symbol);
  g_free(e->name);
  g_free(e);
}

static void
stk_watchlist_finalize(GObject *object)
{
  StkWatchlist *self = STK_WATCHLIST(object);
  g_free(self->path);
  g_ptr_array_unref(self->entries);
  G_OBJECT_CLASS(stk_watchlist_parent_class)->finalize(object);
}

static void
stk_watchlist_class_init(StkWatchlistClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = stk_watchlist_finalize;
  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
                                         G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
}

static void
stk_watchlist_init(StkWatchlist *self)
{
  self->entries = g_ptr_array_new_with_free_func(entry_free);
}

StkWatchlist *
stk_watchlist_new(const char *path)
{
  StkWatchlist *self = g_object_new(STK_TYPE_WATCHLIST, NULL);
  self->path = path != NULL
                 ? g_strdup(path)
                 : g_build_filename(g_get_user_config_dir(), "stocks", "watchlist.json", NULL);
  return self;
}

const char *
stk_watchlist_get_path(StkWatchlist *self)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), NULL);
  return self->path;
}

guint
stk_watchlist_get_n_entries(StkWatchlist *self)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), 0);
  return self->entries->len;
}

const StkWatchlistEntry *
stk_watchlist_get_entry(StkWatchlist *self, guint index)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), NULL);
  return index < self->entries->len ? g_ptr_array_index(self->entries, index) : NULL;
}

/* @symbol must satisfy stk_symbol_is_valid() (so it is at most 15 bytes). */
static void
upcase_symbol(const char *symbol, char out[16])
{
  gsize i = 0;
  for (; symbol[i] != '\0'; i++)
    out[i] = g_ascii_toupper(symbol[i]);
  out[i] = '\0';
}

/* @upper must already be upper-case and valid. */
static gint
find_upper(StkWatchlist *self, const char *upper)
{
  for (guint i = 0; i < self->entries->len; i++) {
    const StkWatchlistEntry *e = g_ptr_array_index(self->entries, i);
    if (strcmp(e->symbol, upper) == 0)
      return (gint)i;
  }
  return -1;
}

gint
stk_watchlist_index_of(StkWatchlist *self, const char *symbol)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), -1);
  if (!stk_symbol_is_valid(symbol))
    return -1;
  char upper[16];
  upcase_symbol(symbol, upper);
  return find_upper(self, upper);
}

gboolean
stk_watchlist_contains(StkWatchlist *self, const char *symbol)
{
  return stk_watchlist_index_of(self, symbol) >= 0;
}

static gboolean
kind_is_valid(StkKind kind)
{
  return (guint)kind <= (guint)STK_KIND_INDEX;
}

static gboolean
name_is_valid(const char *name)
{
  return strlen(name) <= STK_WATCHLIST_MAX_NAME_BYTES && g_utf8_validate(name, -1, NULL);
}

/* Appends without emitting. Returns FALSE on any validation failure. */
static gboolean
append_entry(StkWatchlist *self, const char *symbol, const char *name, StkKind kind)
{
  if (!stk_symbol_is_valid(symbol) || !kind_is_valid(kind))
    return FALSE;
  if (name == NULL)
    name = "";
  if (!name_is_valid(name) || self->entries->len >= STK_WATCHLIST_MAX_ENTRIES)
    return FALSE;

  char upper[16];
  upcase_symbol(symbol, upper);
  if (find_upper(self, upper) >= 0)
    return FALSE;
  StkWatchlistEntry *e = g_new(StkWatchlistEntry, 1);
  e->symbol = g_strdup(upper);
  e->name = g_strdup(name);
  e->kind = kind;
  g_ptr_array_add(self->entries, e);
  self->gen++;
  return TRUE;
}

gboolean
stk_watchlist_add(StkWatchlist *self, const char *symbol, const char *name, StkKind kind)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), FALSE);
  if (!append_entry(self, symbol, name, kind))
    return FALSE;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

gboolean
stk_watchlist_remove(StkWatchlist *self, const char *symbol)
{
  gint i = stk_watchlist_index_of(self, symbol);
  if (i < 0)
    return FALSE;
  g_ptr_array_remove_index(self->entries, (guint)i);
  self->gen++;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

gboolean
stk_watchlist_move(StkWatchlist *self, guint from, guint to)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), FALSE);
  guint n = self->entries->len;
  if (from >= n || to >= n || from == to)
    return FALSE;

  gpointer *pdata = self->entries->pdata;
  gpointer moved = pdata[from];
  if (from < to)
    memmove(&pdata[from], &pdata[from + 1], (to - from) * sizeof(gpointer));
  else
    memmove(&pdata[to + 1], &pdata[to], (from - to) * sizeof(gpointer));
  pdata[to] = moved;
  self->gen++;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Persistence                                                              */
/* ------------------------------------------------------------------------ */

static GError *
pending_error(void)
{
  return g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PENDING, "watchlist: an asynchronous save is running");
}

/* Fills @self (which must be empty) from parsed JSON. */
static gboolean
fill_from_json(StkWatchlist *self, JsonNode *root, guint *dropped, GError **error)
{
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "watchlist: top level is not an object");
    return FALSE;
  }
  JsonObject *obj = json_node_get_object(root);
  JsonNode *vnode = json_object_get_member(obj, "version");
  if (vnode == NULL || !JSON_NODE_HOLDS_VALUE(vnode) ||
      json_node_get_value_type(vnode) != G_TYPE_INT64) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "watchlist: missing or invalid version");
    return FALSE;
  }
  gint64 version = json_node_get_int(vnode);
  if (version != WATCHLIST_VERSION) {
    g_set_error(error, STK_ERROR, STK_ERROR_PARSE, "watchlist: unsupported version %" G_GINT64_FORMAT,
                version);
    return FALSE;
  }
  JsonNode *enode = json_object_get_member(obj, "entries");
  if (enode == NULL || !JSON_NODE_HOLDS_ARRAY(enode)) {
    g_set_error_literal(error, STK_ERROR, STK_ERROR_PARSE, "watchlist: missing entries array");
    return FALSE;
  }

  JsonArray *arr = json_node_get_array(enode);
  guint n = json_array_get_length(arr);
  for (guint i = 0; i < n; i++) {
    JsonNode *item = json_array_get_element(arr, i);
    gboolean ok = FALSE;
    if (JSON_NODE_HOLDS_OBJECT(item)) {
      JsonObject *o = json_node_get_object(item);
      JsonNode *sym = json_object_get_member(o, "symbol");
      JsonNode *nm = json_object_get_member(o, "name");
      JsonNode *kd = json_object_get_member(o, "kind");
      StkKind kind;
      if (sym != NULL && nm != NULL && kd != NULL && JSON_NODE_HOLDS_VALUE(sym) &&
          JSON_NODE_HOLDS_VALUE(nm) && JSON_NODE_HOLDS_VALUE(kd) &&
          json_node_get_value_type(sym) == G_TYPE_STRING &&
          json_node_get_value_type(nm) == G_TYPE_STRING &&
          json_node_get_value_type(kd) == G_TYPE_STRING &&
          stk_kind_from_string(json_node_get_string(kd), &kind))
        ok = append_entry(self, json_node_get_string(sym), json_node_get_string(nm), kind);
    }
    if (!ok)
      (*dropped)++;
  }
  return TRUE;
}

gboolean
stk_watchlist_load(StkWatchlist *self, guint *n_dropped, GError **error)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  if (n_dropped != NULL)
    *n_dropped = 0;
  if (self->save_in_flight) {
    g_propagate_error(error, pending_error());
    return FALSE;
  }

  gboolean had_entries = self->entries->len > 0;
  g_ptr_array_set_size(self->entries, 0);

  stk_file_remove_stale_temps(self->path);
  g_autofree char *text = NULL;
  gsize len = 0;
  gboolean ok = stk_file_read_bounded(self->path, STK_WATCHLIST_MAX_FILE_BYTES, &text, &len, error);
  guint dropped = 0;
  if (ok && !stk_json_check_strict(text, len, STK_JSON_MAX_DEPTH)) {
    g_set_error(error, STK_ERROR, STK_ERROR_PARSE,
                "watchlist: not strict JSON or nested deeper than %d", STK_JSON_MAX_DEPTH);
    ok = FALSE;
  }
  if (ok) {
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) perr = NULL;
    if (!json_parser_load_from_data(parser, text, (gssize)len, &perr)) {
      g_set_error(error, STK_ERROR, STK_ERROR_PARSE, "watchlist: %s", perr->message);
      ok = FALSE;
    } else {
      ok = fill_from_json(self, json_parser_get_root(parser), &dropped, error);
    }
  }

  if (!ok)
    g_ptr_array_set_size(self->entries, 0);
  else if (n_dropped != NULL)
    *n_dropped = dropped;
  self->saved_gen = self->gen;
  if (had_entries || self->entries->len > 0)
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return ok;
}

/* Serialises @entries (StkWatchlistEntry*); safe on any thread as long as the
 * array is not modified meanwhile. Returns the document (g_free()). Strings go
 * through stk_json_append_string(), never JsonGenerator, so that no control
 * byte is ever written raw (the loader rejects those). Size: at most
 * STK_WATCHLIST_MAX_ENTRIES entries of < 2 KiB (a 256-byte name escapes to at
 * most 1536 bytes), far below STK_WATCHLIST_MAX_FILE_BYTES. */
static char *
serialize_entries(const GPtrArray *entries, gsize *len)
{
  GString *out = g_string_sized_new(64 + entries->len * 128);
  g_string_append_printf(out, "{\n  \"version\": %d,\n  \"entries\": [", WATCHLIST_VERSION);
  for (guint i = 0; i < entries->len; i++) {
    const StkWatchlistEntry *e = g_ptr_array_index(entries, i);
    g_string_append(out, i > 0 ? ",\n    {\"symbol\": " : "\n    {\"symbol\": ");
    stk_json_append_string(out, e->symbol);
    g_string_append(out, ", \"name\": ");
    stk_json_append_string(out, e->name);
    g_string_append(out, ", \"kind\": ");
    stk_json_append_string(out, stk_kind_to_string(e->kind));
    g_string_append_c(out, '}');
  }
  g_string_append(out, entries->len > 0 ? "\n  ]\n}\n" : "]\n}\n");
  *len = out->len;
  return g_string_free(out, FALSE);
}

gboolean
stk_watchlist_is_dirty(StkWatchlist *self)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), FALSE);
  return self->gen != self->saved_gen;
}

gboolean
stk_watchlist_save(StkWatchlist *self, GError **error)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  if (self->save_in_flight) {
    g_propagate_error(error, pending_error());
    return FALSE;
  }

  gsize len = 0;
  g_autofree char *text = serialize_entries(self->entries, &len);
  if (!stk_file_write_atomic(self->path, text, len, error))
    return FALSE;
  self->saved_gen = self->gen;
  return TRUE;
}

typedef struct {
  GPtrArray *entries; /* deep copy, owned */
  guint64 gen;
  GTask *outer; /* the task handed to the caller, completed in save_done() */
} SaveJob;

static void
save_job_free(SaveJob *job)
{
  g_ptr_array_unref(job->entries);
  g_object_unref(job->outer);
  g_free(job);
}

static void
save_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  (void)cancellable;
  if (g_task_return_error_if_cancelled(task))
    return;
  const SaveJob *job = task_data;
  gsize len = 0;
  g_autofree char *text = serialize_entries(job->entries, &len);
  GError *error = NULL;
  if (stk_file_write_atomic(STK_WATCHLIST(source)->path, text, len, &error))
    g_task_return_boolean(task, TRUE);
  else
    g_task_return_error(task, error);
}

static void
save_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)user_data;
  StkWatchlist *self = STK_WATCHLIST(source);
  GTask *inner = G_TASK(result);
  const SaveJob *job = g_task_get_task_data(inner);
  GTask *outer = job->outer;
  guint64 gen = job->gen;

  GError *error = NULL;
  gboolean ok = g_task_propagate_boolean(inner, &error);
  self->save_in_flight = FALSE;
  if (ok) {
    self->saved_gen = gen;
    g_task_return_boolean(outer, TRUE);
  } else {
    g_task_return_error(outer, error);
  }
}

void
stk_watchlist_save_async(StkWatchlist *self, GCancellable *cancellable, GAsyncReadyCallback callback,
                         gpointer user_data)
{
  g_return_if_fail(STK_IS_WATCHLIST(self));

  GTask *outer = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(outer, &save_tag);
  /* A cancellation that arrives after the write started must not turn a
   * successful write into an error: only save_worker() honours it. */
  g_task_set_check_cancellable(outer, FALSE);
  if (self->save_in_flight) {
    g_task_return_error(outer, pending_error());
    g_object_unref(outer);
    return;
  }

  self->save_in_flight = TRUE;
  SaveJob *job = g_new0(SaveJob, 1);
  job->entries = g_ptr_array_new_full(self->entries->len, entry_free);
  for (guint i = 0; i < self->entries->len; i++) {
    const StkWatchlistEntry *src = g_ptr_array_index(self->entries, i);
    StkWatchlistEntry *copy = g_new(StkWatchlistEntry, 1);
    copy->symbol = g_strdup(src->symbol);
    copy->name = g_strdup(src->name);
    copy->kind = src->kind;
    g_ptr_array_add(job->entries, copy);
  }
  job->gen = self->gen;
  job->outer = outer;
  GTask *inner = g_task_new(self, cancellable, save_done, NULL);
  g_task_set_check_cancellable(inner, FALSE);
  g_task_set_task_data(inner, job, (GDestroyNotify)save_job_free);
  g_task_run_in_thread(inner, save_worker);
  g_object_unref(inner);
}

gboolean
stk_watchlist_save_finish(StkWatchlist *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(STK_IS_WATCHLIST(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  g_return_val_if_fail(g_async_result_is_tagged(result, &save_tag), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
