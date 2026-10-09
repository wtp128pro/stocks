/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/*
 * test-picker.c - StkPicker under a display with a fake StkProvider.
 *
 * Needs a GDK display (run under xvfb-run); exits 77 (meson: skipped) when
 * gtk_init_check() fails. Nothing touches the network or the disk: the fake
 * provider completes searches only when the test says so (success, failure,
 * or cancellation), and the watchlist is never saved.
 *
 * Determinism: the tests pump the main loop until a condition holds (mapped
 * rows, completed layout, a fired timer) with a generous deadline instead of
 * sleeping a fixed time; the only fixed waits are the ones that prove that
 * something does NOT happen, and the one timing assertion is a lower bound
 * (the debounce never fires early). No test depends on the window being the
 * active X window or on X keyboard focus, which under Xvfb without a window
 * manager belongs to whichever test window happens to be topmost: keyboard
 * focus is driven programmatically and only the focus chain is asserted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stk-picker.h"

/* ------------------------------------------------------------------ */
/* Main loop helpers                                                  */
/* ------------------------------------------------------------------ */

#define DEADLINE_US (15 * G_USEC_PER_SEC)

/* Runs the main loop for @ms milliseconds (used to show absence of events). */
static void
pump_ms(guint ms)
{
  gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;
  GMainContext *ctx = g_main_context_default();
  while (g_get_monotonic_time() < end) {
    while (g_main_context_iteration(ctx, FALSE))
      ;
    g_usleep(2000);
  }
}

/* Pumps until @cond holds or the deadline passes; then asserts @cond. */
#define WAIT_FOR(cond)                                                       \
  G_STMT_START                                                               \
  {                                                                          \
    gint64 wf_end_ = g_get_monotonic_time() + DEADLINE_US;                   \
    while (!(cond) && g_get_monotonic_time() < wf_end_) {                    \
      if (!g_main_context_iteration(NULL, FALSE))                            \
        g_usleep(1000);                                                      \
    }                                                                        \
    g_assert_true(cond);                                                     \
  }                                                                          \
  G_STMT_END

/* ------------------------------------------------------------------ */
/* Fake provider                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
  char *text;
  GCancellable *cancellable;
  GTask *task; /* NULL once completed */
} Pending;

#define FAKE_TYPE_PROVIDER (fake_provider_get_type())
G_DECLARE_FINAL_TYPE(FakeProvider, fake_provider, FAKE, PROVIDER, GObject)

struct _FakeProvider {
  GObject parent_instance;
  GPtrArray *searches; /* Pending* */
  gboolean ignore_cancel;
};

static void fake_provider_iface_init(StkProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(FakeProvider, fake_provider, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(STK_TYPE_PROVIDER, fake_provider_iface_init))

static void
pending_free(gpointer data)
{
  Pending *p = data;
  g_free(p->text);
  g_clear_object(&p->cancellable);
  g_clear_object(&p->task);
  g_free(p);
}

static void
fake_provider_finalize(GObject *object)
{
  g_ptr_array_unref(FAKE_PROVIDER(object)->searches);
  G_OBJECT_CLASS(fake_provider_parent_class)->finalize(object);
}

static void
fake_provider_class_init(FakeProviderClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = fake_provider_finalize;
}

static void
fake_provider_init(FakeProvider *self)
{
  self->searches = g_ptr_array_new_with_free_func(pending_free);
}

static void
fake_search_async(StkProvider *provider, const char *text, GCancellable *cancellable,
                  GAsyncReadyCallback callback, gpointer user_data)
{
  FakeProvider *self = FAKE_PROVIDER(provider);
  Pending *p = g_new0(Pending, 1);
  p->text = g_strdup(text);
  p->cancellable = cancellable != NULL ? g_object_ref(cancellable) : NULL;
  p->task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_check_cancellable(p->task, !self->ignore_cancel);
  g_ptr_array_add(self->searches, p);
}

static GPtrArray *
fake_search_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  (void)provider;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
fake_unused_quote_async(StkProvider *provider, const char *symbol, StkKind kind,
                        GCancellable *cancellable, GAsyncReadyCallback callback,
                        gpointer user_data)
{
  (void)provider, (void)symbol, (void)kind, (void)cancellable, (void)callback, (void)user_data;
  g_assert_not_reached();
}

static StkQuote *
fake_unused_quote_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  (void)provider, (void)result, (void)error;
  g_assert_not_reached();
}

static void
fake_unused_series_async(StkProvider *provider, const char *symbol, StkKind kind, StkRange range,
                         GCancellable *cancellable, GAsyncReadyCallback callback,
                         gpointer user_data)
{
  (void)provider, (void)symbol, (void)kind, (void)range, (void)cancellable, (void)callback,
      (void)user_data;
  g_assert_not_reached();
}

static StkSeries *
fake_unused_series_finish(StkProvider *provider, GAsyncResult *result, GError **error)
{
  (void)provider, (void)result, (void)error;
  g_assert_not_reached();
}

static void
fake_provider_iface_init(StkProviderInterface *iface)
{
  iface->search_async = fake_search_async;
  iface->search_finish = fake_search_finish;
  /* The picker must never use quotes or series. */
  iface->fetch_quote_async = fake_unused_quote_async;
  iface->fetch_quote_finish = fake_unused_quote_finish;
  iface->fetch_series_async = fake_unused_series_async;
  iface->fetch_series_finish = fake_unused_series_finish;
}

static Pending *
fake_pending(FakeProvider *f, guint i)
{
  g_assert_cmpuint(i, <, f->searches->len);
  return g_ptr_array_index(f->searches, i);
}

static void
fake_complete(FakeProvider *f, guint i, GPtrArray *hits)
{
  Pending *p = fake_pending(f, i);
  g_assert_nonnull(p->task);
  GTask *task = g_steal_pointer(&p->task);
  g_task_return_pointer(task, hits, (GDestroyNotify)g_ptr_array_unref);
  g_object_unref(task);
}

static void
fake_fail(FakeProvider *f, guint i, GError *error)
{
  Pending *p = fake_pending(f, i);
  g_assert_nonnull(p->task);
  GTask *task = g_steal_pointer(&p->task);
  g_task_return_error(task, error);
  g_object_unref(task);
}

static GPtrArray *
hits_new(void)
{
  return g_ptr_array_new_with_free_func((GDestroyNotify)stk_search_hit_free);
}

static void
hits_add(GPtrArray *hits, const char *symbol, const char *name, StkKind kind)
{
  StkSearchHit *h = g_new0(StkSearchHit, 1);
  h->symbol = g_strdup(symbol);
  h->name = g_strdup(name);
  h->kind = kind;
  g_ptr_array_add(hits, h);
}

/* ------------------------------------------------------------------ */
/* Widget helpers                                                     */
/* ------------------------------------------------------------------ */

static GtkWidget *
find_named(GtkWidget *root, const char *name)
{
  if (strcmp(gtk_widget_get_name(root), name) == 0)
    return root;
  for (GtkWidget *c = gtk_widget_get_first_child(root); c != NULL;
       c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_named(c, name);
    if (found != NULL)
      return found;
  }
  return NULL;
}

static void
collect_named(GtkWidget *root, const char *name, GPtrArray *out)
{
  if (strcmp(gtk_widget_get_name(root), name) == 0)
    g_ptr_array_add(out, root);
  for (GtkWidget *c = gtk_widget_get_first_child(root); c != NULL;
       c = gtk_widget_get_next_sibling(c))
    collect_named(c, name, out);
}

static const char *
toggle_symbol(GtkWidget *toggle)
{
  return gtk_label_get_text(GTK_LABEL(gtk_widget_get_next_sibling(toggle)));
}

static int
compare_strings(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Sorted, space separated symbols of the mapped rows of list @list_name;
 * g_free() the result. */
static char *
visible_symbols(GtkWidget *picker, const char *list_name)
{
  GtkWidget *list = find_named(picker, list_name);
  g_assert_nonnull(list);
  g_autoptr(GPtrArray) toggles = g_ptr_array_new();
  collect_named(list, "stk-picker-toggle", toggles);
  g_autoptr(GPtrArray) syms = g_ptr_array_new();
  for (guint i = 0; i < toggles->len; i++)
    if (gtk_widget_get_mapped(g_ptr_array_index(toggles, i)))
      g_ptr_array_add(syms, (gpointer)toggle_symbol(g_ptr_array_index(toggles, i)));
  g_ptr_array_sort(syms, compare_strings);
  GString *s = g_string_new(NULL);
  for (guint i = 0; i < syms->len; i++) {
    if (i > 0)
      g_string_append_c(s, ' ');
    g_string_append(s, g_ptr_array_index(syms, i));
  }
  return g_string_free(s, FALSE);
}

static guint
list_n_items(GtkWidget *picker, const char *list_name)
{
  GtkWidget *list = find_named(picker, list_name);
  g_assert_nonnull(list);
  return g_list_model_get_n_items(G_LIST_MODEL(gtk_list_view_get_model(GTK_LIST_VIEW(list))));
}

/* The mapped check button of @symbol's row in @list_name, or NULL. */
static GtkWidget *
find_toggle(GtkWidget *picker, const char *list_name, const char *symbol)
{
  GtkWidget *list = find_named(picker, list_name);
  g_assert_nonnull(list);
  g_autoptr(GPtrArray) toggles = g_ptr_array_new();
  collect_named(list, "stk-picker-toggle", toggles);
  for (guint i = 0; i < toggles->len; i++) {
    GtkWidget *t = g_ptr_array_index(toggles, i);
    if (gtk_widget_get_mapped(t) && strcmp(toggle_symbol(t), symbol) == 0)
      return t;
  }
  return NULL;
}

/* Part of a row: 0 name label, 1 category label, 2 kind label. */
static GtkWidget *
row_part(GtkWidget *toggle, int which)
{
  GtkWidget *row = gtk_widget_get_parent(toggle);
  GtkWidget *info = gtk_widget_get_next_sibling(gtk_widget_get_next_sibling(toggle));
  switch (which) {
  case 0:
    return gtk_widget_get_first_child(info);
  case 1:
    return gtk_widget_get_last_child(info);
  default:
    return gtk_widget_get_last_child(row);
  }
}

static const char *
row_label(GtkWidget *toggle, int which)
{
  return gtk_label_get_text(GTK_LABEL(row_part(toggle, which)));
}

static guint
count_toggles(GtkWidget *list)
{
  g_autoptr(GPtrArray) toggles = g_ptr_array_new();
  collect_named(list, "stk-picker-toggle", toggles);
  return toggles->len;
}

static GtkEditable *
entry_of(GtkWidget *picker, const char *name)
{
  GtkWidget *e = find_named(picker, name);
  g_assert_nonnull(e);
  return GTK_EDITABLE(e);
}

static void
set_filter_text(GtkWidget *picker, const char *text)
{
  gtk_editable_set_text(entry_of(picker, "stk-picker-filter-entry"), text);
}

static void
set_search_text(GtkWidget *picker, const char *text)
{
  gtk_editable_set_text(entry_of(picker, "stk-picker-search-entry"), text);
}

static GtkWidget *
status_label(GtkWidget *picker)
{
  return find_named(picker, "stk-picker-status");
}

static gboolean
status_visible(GtkWidget *picker)
{
  return gtk_widget_get_visible(status_label(picker));
}

static const char *
status_text(GtkWidget *picker)
{
  return gtk_label_get_text(GTK_LABEL(status_label(picker)));
}

static gboolean
status_is_error(GtkWidget *picker)
{
  return gtk_widget_has_css_class(status_label(picker), "error");
}

static gboolean
results_visible(GtkWidget *picker)
{
  return gtk_widget_is_visible(find_named(picker, "stk-picker-results"));
}

/* ------------------------------------------------------------------ */
/* Fixture                                                            */
/* ------------------------------------------------------------------ */

static const char CATALOG_JSON[] =
    "{\"version\":1,\"items\":["
    "{\"symbol\":\"AAA\",\"name\":\"Alpha Growth Fund\",\"kind\":\"fund\",\"category\":\"Large Growth\"},"
    "{\"symbol\":\"BBB\",\"name\":\"Beta Tech ETF\",\"kind\":\"etf\",\"category\":\"Technology\"},"
    "{\"symbol\":\"CCC\",\"name\":\"Gamma Bond ETF\",\"kind\":\"etf\",\"category\":\"Bonds\"},"
    "{\"symbol\":\"DDD\",\"name\":\"Delta Bond Fund\",\"kind\":\"fund\",\"category\":\"Bonds\"},"
    "{\"symbol\":\"EEE\",\"name\":\"\xC3\x89psilon Caf\xC3\xA9\",\"kind\":\"etf\",\"category\":\"Technology\"}"
    "]}";

typedef struct {
  StkCatalog *catalog;
  StkWatchlist *watchlist;
  FakeProvider *fake;
  GtkWidget *picker; /* NULL once destroyed and finalized */
  guint changed;     /* "watchlist-changed" emissions */
  guint close_requests;
} Fx;

static void
on_changed(StkPicker *picker, gpointer user_data)
{
  (void)picker;
  (*(guint *)user_data)++;
}

static gboolean
on_close_request(GtkWindow *window, gpointer user_data)
{
  (void)window;
  (*(guint *)user_data)++;
  return FALSE;
}

/* Builds the picker over CATALOG_JSON (or the default catalog when
 * @use_default) and presents it. */
static void
fx_init(Fx *fx, gboolean use_default, gboolean present)
{
  memset(fx, 0, sizeof *fx);
  GError *error = NULL;
  fx->catalog = use_default ? stk_catalog_load_default(&error)
                            : stk_catalog_new_from_data(CATALOG_JSON, strlen(CATALOG_JSON), &error);
  g_assert_no_error(error);
  g_assert_nonnull(fx->catalog);
  fx->watchlist = stk_watchlist_new("/nonexistent-dir/stk-test-watchlist.json");
  fx->fake = g_object_new(FAKE_TYPE_PROVIDER, NULL);
  fx->picker = stk_picker_new(fx->catalog, fx->watchlist, STK_PROVIDER(fx->fake));
  g_assert_true(STK_IS_PICKER(fx->picker));
  g_object_add_weak_pointer(G_OBJECT(fx->picker), (gpointer *)&fx->picker);
  g_signal_connect(fx->picker, "watchlist-changed", G_CALLBACK(on_changed), &fx->changed);
  g_signal_connect(fx->picker, "close-request", G_CALLBACK(on_close_request),
                   &fx->close_requests);
  if (present) {
    /* tall enough for every row of the small fixtures to be on screen */
    gtk_window_set_default_size(GTK_WINDOW(fx->picker), 520, 1600);
    gtk_window_present(GTK_WINDOW(fx->picker));
    GtkWidget *list = find_named(fx->picker, "stk-picker-catalog");
    WAIT_FOR(gtk_widget_get_width(list) > 0);
  }
}

/* Destroys the picker (if still alive) and waits until it is finalized, which
 * also proves that no request or timer keeps it alive; then releases the rest.
 * Unfinished fake searches are failed with G_IO_ERROR_CANCELLED. */
static void
fx_clear(Fx *fx)
{
  if (fx->picker != NULL) {
    gtk_window_destroy(GTK_WINDOW(fx->picker));
    WAIT_FOR(fx->picker == NULL);
  }
  for (guint i = 0; i < fx->fake->searches->len; i++) {
    Pending *p = fake_pending(fx->fake, i);
    if (p->task != NULL)
      fake_fail(fx->fake, i, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "teardown"));
  }
  g_object_unref(fx->fake);
  g_object_unref(fx->watchlist);
  stk_catalog_free(fx->catalog);
}

/* ------------------------------------------------------------------ */
/* P1: catalog list and filters                                       */
/* ------------------------------------------------------------------ */

static void
assert_symbols(GtkWidget *picker, const char *list, const char *expected, guint n)
{
  g_assert_cmpuint(list_n_items(picker, list), ==, n);
  gint64 end = g_get_monotonic_time() + DEADLINE_US;
  char *syms = visible_symbols(picker, list);
  while (strcmp(syms, expected) != 0 && g_get_monotonic_time() < end) {
    g_free(syms);
    if (!g_main_context_iteration(NULL, FALSE))
      g_usleep(1000);
    syms = visible_symbols(picker, list);
  }
  g_assert_cmpstr(syms, ==, expected);
  g_free(syms);
}

static void
test_filters(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  GtkWidget *p = fx.picker;
  GtkDropDown *kind = GTK_DROP_DOWN(find_named(p, "stk-picker-kind"));
  GtkDropDown *cat = GTK_DROP_DOWN(find_named(p, "stk-picker-category"));

  g_assert_false(status_visible(p));
  g_assert_false(results_visible(p));
  assert_symbols(p, "stk-picker-catalog", "AAA BBB CCC DDD EEE", 5);

  /* the category drop-down lists each non-empty category once, sorted */
  GListModel *cats = gtk_drop_down_get_model(cat);
  g_assert_cmpuint(g_list_model_get_n_items(cats), ==, 4);
  GtkStringList *sl = GTK_STRING_LIST(cats);
  g_assert_cmpstr(gtk_string_list_get_string(sl, 0), ==, "All categories");
  g_assert_cmpstr(gtk_string_list_get_string(sl, 1), ==, "Bonds");
  g_assert_cmpstr(gtk_string_list_get_string(sl, 2), ==, "Large Growth");
  g_assert_cmpstr(gtk_string_list_get_string(sl, 3), ==, "Technology");

  /* row content: name, category and kind labels */
  WAIT_FOR(find_toggle(p, "stk-picker-catalog", "BBB") != NULL);
  GtkWidget *t = find_toggle(p, "stk-picker-catalog", "BBB");
  g_assert_cmpstr(row_label(t, 0), ==, "Beta Tech ETF");
  g_assert_cmpstr(row_label(t, 1), ==, "Technology");
  g_assert_cmpstr(row_label(t, 2), ==, "ETF");
  WAIT_FOR(find_toggle(p, "stk-picker-catalog", "AAA") != NULL);
  g_assert_cmpstr(row_label(find_toggle(p, "stk-picker-catalog", "AAA"), 2), ==, "Fund");

  /* kind filter */
  gtk_drop_down_set_selected(kind, 1);
  assert_symbols(p, "stk-picker-catalog", "BBB CCC EEE", 3);
  gtk_drop_down_set_selected(kind, 2);
  assert_symbols(p, "stk-picker-catalog", "AAA DDD", 2);
  gtk_drop_down_set_selected(kind, 0);
  assert_symbols(p, "stk-picker-catalog", "AAA BBB CCC DDD EEE", 5);

  /* "Indices" is the 4th position; the fixture catalog has no index */
  GListModel *kinds = gtk_drop_down_get_model(kind);
  g_assert_cmpuint(g_list_model_get_n_items(kinds), ==, 4);
  g_assert_cmpstr(gtk_string_list_get_string(GTK_STRING_LIST(kinds), 3), ==, "Indices");
  gtk_drop_down_set_selected(kind, 3);
  assert_symbols(p, "stk-picker-catalog", "", 0);
  gtk_drop_down_set_selected(kind, 0);
  assert_symbols(p, "stk-picker-catalog", "AAA BBB CCC DDD EEE", 5);

  /* category filter (exact category) */
  gtk_drop_down_set_selected(cat, 1);
  assert_symbols(p, "stk-picker-catalog", "CCC DDD", 2);
  gtk_drop_down_set_selected(kind, 1);
  assert_symbols(p, "stk-picker-catalog", "CCC", 1);
  gtk_drop_down_set_selected(cat, 0);
  gtk_drop_down_set_selected(kind, 0);

  /* substring filter: symbol, name or category; ignores ASCII case and
   * surrounding whitespace */
  set_filter_text(p, "bond");
  assert_symbols(p, "stk-picker-catalog", "CCC DDD", 2);
  set_filter_text(p, "  TECH ");
  assert_symbols(p, "stk-picker-catalog", "BBB EEE", 2);
  set_filter_text(p, "aaa");
  assert_symbols(p, "stk-picker-catalog", "AAA", 1);
  set_filter_text(p, "Caf\xC3\xA9");
  assert_symbols(p, "stk-picker-catalog", "EEE", 1);
  set_filter_text(p, "zzz");
  assert_symbols(p, "stk-picker-catalog", "", 0);

  /* all three combined */
  set_filter_text(p, "bond");
  gtk_drop_down_set_selected(kind, 2);
  gtk_drop_down_set_selected(cat, 1);
  assert_symbols(p, "stk-picker-catalog", "DDD", 1);
  set_filter_text(p, "");
  gtk_drop_down_set_selected(kind, 0);
  gtk_drop_down_set_selected(cat, 0);
  assert_symbols(p, "stk-picker-catalog", "AAA BBB CCC DDD EEE", 5);

  /* the entry is bounded (no unbounded allocation from huge pastes) */
  g_autofree char *huge = g_strnfill(100000, 'q');
  set_filter_text(p, huge);
  g_assert_cmpuint(strlen(gtk_editable_get_text(entry_of(p, "stk-picker-filter-entry"))), <=,
                   STK_PICKER_MAX_QUERY_CHARS);
  set_search_text(p, huge);
  g_assert_cmpuint(strlen(gtk_editable_get_text(entry_of(p, "stk-picker-search-entry"))), <=,
                   STK_PICKER_MAX_QUERY_CHARS);
  set_search_text(p, "");
  set_filter_text(p, "");
  assert_symbols(p, "stk-picker-catalog", "AAA BBB CCC DDD EEE", 5);

  g_assert_cmpuint(fx.changed, ==, 0);
  fx_clear(&fx);
}

static void
test_default_catalog_virtualised(void)
{
  Fx fx;
  fx_init(&fx, TRUE, TRUE);
  gsize n = 0;
  stk_catalog_entries(fx.catalog, &n);
  g_assert_cmpuint(n, >, 100);
  g_assert_cmpuint(list_n_items(fx.picker, "stk-picker-catalog"), ==, n);

  /* only the visible rows have widgets */
  GtkWidget *list = find_named(fx.picker, "stk-picker-catalog");
  WAIT_FOR(count_toggles(list) > 0);
  g_assert_cmpuint(count_toggles(list), <, n);

  /* filter with the catalog's own semantics */
  gsize n_hits = 0;
  g_autofree const StkCatalogEntry **hits =
      stk_catalog_filter(fx.catalog, STK_CATALOG_FILTER_FUND, "growth", &n_hits);
  g_assert_cmpuint(n_hits, >, 0);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(find_named(fx.picker, "stk-picker-kind")), 2);
  set_filter_text(fx.picker, "growth");
  g_assert_cmpuint(list_n_items(fx.picker, "stk-picker-catalog"), ==, n_hits);
  fx_clear(&fx);
}

/* ------------------------------------------------------------------ */
/* P1: toggles and watchlist sync                                     */
/* ------------------------------------------------------------------ */

static gboolean
toggle_active(GtkWidget *toggle)
{
  return gtk_check_button_get_active(GTK_CHECK_BUTTON(toggle));
}

static void
test_toggle_sync(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  GtkWidget *p = fx.picker;
  WAIT_FOR(find_toggle(p, "stk-picker-catalog", "BBB") != NULL);
  GtkWidget *bbb = find_toggle(p, "stk-picker-catalog", "BBB");
  g_assert_false(toggle_active(bbb));

  /* on: added with the catalog's kind and name, one signal */
  gtk_check_button_set_active(GTK_CHECK_BUTTON(bbb), TRUE);
  g_assert_true(stk_watchlist_contains(fx.watchlist, "BBB"));
  g_assert_cmpuint(stk_watchlist_get_n_entries(fx.watchlist), ==, 1);
  const StkWatchlistEntry *e = stk_watchlist_get_entry(fx.watchlist, 0);
  g_assert_cmpstr(e->symbol, ==, "BBB");
  g_assert_cmpstr(e->name, ==, "Beta Tech ETF");
  g_assert_cmpint(e->kind, ==, STK_KIND_ETF);
  g_assert_cmpuint(fx.changed, ==, 1);
  g_assert_true(stk_watchlist_is_dirty(fx.watchlist)); /* edited; the picker never saves */

  /* off */
  gtk_check_button_set_active(GTK_CHECK_BUTTON(bbb), FALSE);
  g_assert_false(stk_watchlist_contains(fx.watchlist, "BBB"));
  g_assert_cmpuint(fx.changed, ==, 2);

  /* fund kind */
  WAIT_FOR(find_toggle(p, "stk-picker-catalog", "AAA") != NULL);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(find_toggle(p, "stk-picker-catalog", "AAA")),
                              TRUE);
  g_assert_cmpint(stk_watchlist_get_entry(fx.watchlist, 0)->kind, ==, STK_KIND_FUND);
  g_assert_cmpuint(fx.changed, ==, 3);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(find_toggle(p, "stk-picker-catalog", "AAA")),
                              FALSE);
  g_assert_cmpuint(fx.changed, ==, 4);

  /* external changes update the rows, and emit nothing */
  g_assert_true(stk_watchlist_add(fx.watchlist, "CCC", "x", STK_KIND_ETF));
  g_assert_true(toggle_active(find_toggle(p, "stk-picker-catalog", "CCC")));
  g_assert_false(toggle_active(find_toggle(p, "stk-picker-catalog", "DDD")));
  g_assert_true(stk_watchlist_add(fx.watchlist, "ddd", "y", STK_KIND_FUND)); /* case-insensitive */
  g_assert_true(toggle_active(find_toggle(p, "stk-picker-catalog", "DDD")));
  g_assert_true(stk_watchlist_remove(fx.watchlist, "CCC"));
  g_assert_false(toggle_active(find_toggle(p, "stk-picker-catalog", "CCC")));
  g_assert_false(stk_watchlist_move(fx.watchlist, 0, 0));
  g_assert_cmpuint(fx.changed, ==, 4);
  g_assert_cmpuint(stk_watchlist_get_n_entries(fx.watchlist), ==, 1);

  /* a row that was filtered out and comes back shows the current state */
  set_filter_text(p, "eee");
  assert_symbols(p, "stk-picker-catalog", "EEE", 1);
  g_assert_true(stk_watchlist_add(fx.watchlist, "BBB", "b", STK_KIND_ETF));
  set_filter_text(p, "");
  WAIT_FOR(find_toggle(p, "stk-picker-catalog", "BBB") != NULL);
  g_assert_true(toggle_active(find_toggle(p, "stk-picker-catalog", "BBB")));
  g_assert_true(toggle_active(find_toggle(p, "stk-picker-catalog", "DDD")));
  g_assert_false(toggle_active(find_toggle(p, "stk-picker-catalog", "AAA")));

  /* toggling off an entry that was added elsewhere removes it */
  gtk_check_button_set_active(GTK_CHECK_BUTTON(find_toggle(p, "stk-picker-catalog", "BBB")),
                              FALSE);
  g_assert_false(stk_watchlist_contains(fx.watchlist, "BBB"));
  g_assert_cmpuint(fx.changed, ==, 5);
  fx_clear(&fx);
}

static void
test_row_activation(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  GtkWidget *list = find_named(fx.picker, "stk-picker-catalog");
  WAIT_FOR(find_toggle(fx.picker, "stk-picker-catalog", "AAA") != NULL);

  g_signal_emit_by_name(list, "activate", 0u); /* Enter / double click on row 0 */
  g_assert_true(stk_watchlist_contains(fx.watchlist, "AAA"));
  g_assert_cmpuint(fx.changed, ==, 1);
  g_assert_true(toggle_active(find_toggle(fx.picker, "stk-picker-catalog", "AAA")));
  g_signal_emit_by_name(list, "activate", 0u);
  g_assert_false(stk_watchlist_contains(fx.watchlist, "AAA"));
  g_assert_cmpuint(fx.changed, ==, 2);
  g_assert_false(toggle_active(find_toggle(fx.picker, "stk-picker-catalog", "AAA")));
  g_signal_emit_by_name(list, "activate", 99u); /* out of range: ignored */
  g_assert_cmpuint(fx.changed, ==, 2);
  fx_clear(&fx);
}

/* TRUE iff the keyboard focus of @picker is @name or a descendant of it. */
static gboolean
focus_within(GtkWidget *picker, const char *name)
{
  for (GtkWidget *w = gtk_window_get_focus(GTK_WINDOW(picker)); w != NULL;
       w = gtk_widget_get_parent(w))
    if (strcmp(gtk_widget_get_name(w), name) == 0)
      return TRUE;
  return FALSE;
}

/* Moves the focus one step the way the window does for Tab (@dir forward) or
 * Shift+Tab (backward) from wherever it is. The step must succeed. */
static void
tab_on(GtkWidget *picker, GtkDirectionType dir)
{
  g_assert_true(gtk_widget_child_focus(picker, dir));
}

/* Puts the focus on @start (or inside it) programmatically, without any help
 * from the display, then takes one tab_on() step.
 *
 * Only the focus chain is asserted, never window activation: GtkText, the
 * editable inside every GtkEntry, answers a focus move with "I have it" until
 * the window is the active X window (it consults has-focus), so a step that
 * LEAVES an entry would not move in an inactive window. Entries are therefore
 * only used as targets of a step, never as its start. */
static void
tab_from(GtkWidget *picker, GtkWidget *start, GtkDirectionType dir)
{
  g_assert_false(GTK_IS_EDITABLE(start));
  gtk_widget_grab_focus(start);
  GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(picker));
  g_assert_nonnull(focus);
  g_assert_true(focus == start || gtk_widget_is_ancestor(focus, start));
  tab_on(picker, dir);
}

/* The window orders its Tab stops by their allocated position (top to bottom,
 * then left to right), so the order is only the designed one once the layout
 * has run for all of them. TRUE iff the six stops, in the designed order,
 * have a size and strictly increasing positions. */
static gboolean
tab_stops_laid_out(GtkWidget *picker)
{
  static const char *const stops[] = {
    "stk-picker-filter-entry", "stk-picker-kind",    "stk-picker-category",
    "stk-picker-search-entry", "stk-picker-results", "stk-picker-catalog",
  };
  graphene_rect_t previous = GRAPHENE_RECT_INIT(-1, -1, 0, 0);
  for (guint i = 0; i < G_N_ELEMENTS(stops); i++) {
    GtkWidget *w = find_named(picker, stops[i]);
    graphene_rect_t bounds;
    if (gtk_widget_get_height(w) <= 0 || !gtk_widget_compute_bounds(w, picker, &bounds))
      return FALSE;
    if (bounds.origin.y < previous.origin.y ||
        (bounds.origin.y == previous.origin.y && bounds.origin.x <= previous.origin.x))
      return FALSE;
    previous = bounds;
  }
  return TRUE;
}

/* Tab order of the window: filter entry, kind, category, search entry, the
 * results list, the catalog list. In each list Tab visits one row (the row,
 * then its check button) and then leaves it, although the catalog holds
 * hundreds of rows, so that the keyboard user is not trapped. Every adjacent
 * pair is checked in at least one direction by a step that does not start on
 * an entry; see tab_from(). */
static void
test_keyboard_navigation(void)
{
  Fx fx;
  fx_init(&fx, TRUE, TRUE);
  GtkWidget *p = fx.picker;
  set_search_text(p, "micro");
  WAIT_FOR(fx.fake->searches->len == 1);
  GPtrArray *hits = hits_new();
  hits_add(hits, "MSFT", "Microsoft Corp", STK_KIND_STOCK);
  hits_add(hits, "MSFTX", "Microsoft Fund", STK_KIND_FUND);
  fake_complete(fx.fake, 0, hits);
  WAIT_FOR(find_toggle(p, "stk-picker-results", "MSFT") != NULL);
  WAIT_FOR(find_toggle(p, "stk-picker-results", "MSFTX") != NULL);
  WAIT_FOR(count_toggles(find_named(p, "stk-picker-catalog")) > 10);
  WAIT_FOR(tab_stops_laid_out(p));

  GtkWidget *kind = find_named(p, "stk-picker-kind");
  GtkWidget *category = find_named(p, "stk-picker-category");
  GtkWidget *results = find_named(p, "stk-picker-results");
  GtkWidget *catalog = find_named(p, "stk-picker-catalog");

  /* forward: kind, category, search entry */
  tab_from(p, kind, GTK_DIR_TAB_FORWARD);
  g_assert_true(focus_within(p, "stk-picker-category"));
  tab_from(p, category, GTK_DIR_TAB_FORWARD);
  g_assert_true(focus_within(p, "stk-picker-search-entry"));

  /* forward from the results list: a row, then its check button ... */
  gtk_widget_grab_focus(results);
  g_assert_true(focus_within(p, "stk-picker-results"));
  tab_on(p, GTK_DIR_TAB_FORWARD);
  g_assert_true(focus_within(p, "stk-picker-toggle"));
  g_assert_true(focus_within(p, "stk-picker-results"));
  g_assert_cmpstr(toggle_symbol(gtk_window_get_focus(GTK_WINDOW(p))), ==, "MSFT");
  /* ... then the list is left, although a second row follows, and the
   * catalog is entered: its row, then its check button ... */
  tab_on(p, GTK_DIR_TAB_FORWARD);
  g_assert_true(focus_within(p, "stk-picker-catalog"));
  g_assert_false(focus_within(p, "stk-picker-results"));
  tab_on(p, GTK_DIR_TAB_FORWARD);
  g_assert_true(focus_within(p, "stk-picker-toggle"));
  g_assert_true(focus_within(p, "stk-picker-catalog"));
  /* ... and the end of the chain wraps around to the first control */
  tab_on(p, GTK_DIR_TAB_FORWARD);
  g_assert_true(focus_within(p, "stk-picker-filter-entry"));

  /* backward: the filter entry follows the kind drop-down, the search entry
   * the results list, and the results list the catalog */
  tab_from(p, kind, GTK_DIR_TAB_BACKWARD);
  g_assert_true(focus_within(p, "stk-picker-filter-entry"));
  tab_from(p, results, GTK_DIR_TAB_BACKWARD);
  g_assert_true(focus_within(p, "stk-picker-search-entry"));
  tab_from(p, catalog, GTK_DIR_TAB_BACKWARD);
  g_assert_true(focus_within(p, "stk-picker-results"));
  fx_clear(&fx);
}

/* What Space does on a focused check button is its "activate" action. */
static void
test_keyboard_toggle(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  GtkWidget *p = fx.picker;
  WAIT_FOR(find_toggle(p, "stk-picker-catalog", "BBB") != NULL);
  GtkWidget *bbb = find_toggle(p, "stk-picker-catalog", "BBB");
  gtk_widget_grab_focus(bbb);
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(p)) == bbb);

  g_signal_emit_by_name(bbb, "activate");
  g_assert_true(stk_watchlist_contains(fx.watchlist, "BBB"));
  g_assert_true(toggle_active(bbb));
  g_assert_cmpuint(fx.changed, ==, 1);

  /* a change made elsewhere updates the row in place: the focus stays */
  g_assert_true(stk_watchlist_add(fx.watchlist, "CCC", "c", STK_KIND_ETF));
  g_assert_true(toggle_active(find_toggle(p, "stk-picker-catalog", "CCC")));
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(p)) == bbb);

  g_signal_emit_by_name(bbb, "activate");
  g_assert_false(stk_watchlist_contains(fx.watchlist, "BBB"));
  g_assert_false(toggle_active(bbb));
  g_assert_cmpuint(fx.changed, ==, 2);
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(p)) == bbb);
  fx_clear(&fx);
}

static void
destroy_picker(StkPicker *picker, gpointer user_data)
{
  (void)user_data;
  gtk_window_destroy(GTK_WINDOW(picker));
}

/* A "watchlist-changed" handler may destroy the picker, also from inside the
 * check button's own "toggled" emission. */
static void
test_destroy_in_handler(void)
{
  for (int by_row = 0; by_row < 2; by_row++) {
    Fx fx;
    fx_init(&fx, FALSE, TRUE);
    WAIT_FOR(find_toggle(fx.picker, "stk-picker-catalog", "AAA") != NULL);
    GtkWidget *toggle = find_toggle(fx.picker, "stk-picker-catalog", "AAA");
    GtkWidget *list = find_named(fx.picker, "stk-picker-catalog");
    g_signal_connect(fx.picker, "watchlist-changed", G_CALLBACK(destroy_picker), NULL);
    if (by_row)
      g_signal_emit_by_name(list, "activate", 0u);
    else
      gtk_check_button_set_active(GTK_CHECK_BUTTON(toggle), TRUE);
    WAIT_FOR(fx.picker == NULL);
    g_assert_true(stk_watchlist_contains(fx.watchlist, "AAA"));
    g_assert_cmpuint(fx.changed, ==, 1);
    fx_clear(&fx);
  }
}

static void
destroy_on_watchlist_changed(StkWatchlist *watchlist, gpointer user_data)
{
  (void)watchlist;
  GtkWidget **picker = user_data; /* &Fx.picker: NULL once finalized */
  if (*picker != NULL)
    gtk_window_destroy(GTK_WINDOW(*picker));
}

static void
hide_on_watchlist_changed(StkWatchlist *watchlist, gpointer user_data)
{
  (void)watchlist;
  GtkWidget **picker = user_data;
  if (*picker != NULL)
    gtk_widget_set_visible(*picker, FALSE);
}

/* A handler of the WATCHLIST's "changed" signal (connected after the picker's
 * own) destroys the picker in the middle of the picker's edit, through the
 * check button and through row activation, with and without an owner that
 * still holds a reference. The edit stands, nothing is emitted, nothing is
 * used after free (the ASan run of this test is the proof), and a destroyed
 * picker accepts no further edit. */
static void
test_destroy_in_watchlist_handler(void)
{
  for (int by_row = 0; by_row < 2; by_row++) {
    for (int held = 0; held < 2; held++) {
      Fx fx;
      fx_init(&fx, FALSE, TRUE);
      WAIT_FOR(find_toggle(fx.picker, "stk-picker-catalog", "AAA") != NULL);
      GtkWidget *toggle = find_toggle(fx.picker, "stk-picker-catalog", "AAA");
      GtkWidget *list = find_named(fx.picker, "stk-picker-catalog");
      g_signal_connect(fx.watchlist, "changed", G_CALLBACK(destroy_on_watchlist_changed),
                       &fx.picker);
      GtkWidget *owner = held ? g_object_ref(fx.picker) : NULL;

      if (by_row)
        g_signal_emit_by_name(list, "activate", 0u);
      else
        gtk_check_button_set_active(GTK_CHECK_BUTTON(toggle), TRUE);

      g_assert_true(stk_watchlist_contains(fx.watchlist, "AAA"));
      g_assert_cmpuint(fx.changed, ==, 0);
      if (held) {
        g_assert_nonnull(fx.picker); /* alive, but destroyed */
        g_signal_emit_by_name(list, "activate", 1u);
        g_assert_false(stk_watchlist_contains(fx.watchlist, "BBB"));
        g_assert_cmpuint(stk_watchlist_get_n_entries(fx.watchlist), ==, 1);
        g_assert_cmpuint(fx.changed, ==, 0);
        g_object_unref(owner);
      }
      WAIT_FOR(fx.picker == NULL);
      fx_clear(&fx);
    }
  }
}

/* A watchlist handler that merely hides the picker does not stop the edit
 * from being announced. */
static void
test_hide_in_watchlist_handler(void)
{
  for (int by_row = 0; by_row < 2; by_row++) {
    Fx fx;
    fx_init(&fx, FALSE, TRUE);
    WAIT_FOR(find_toggle(fx.picker, "stk-picker-catalog", "AAA") != NULL);
    GtkWidget *toggle = find_toggle(fx.picker, "stk-picker-catalog", "AAA");
    GtkWidget *list = find_named(fx.picker, "stk-picker-catalog");
    g_signal_connect(fx.watchlist, "changed", G_CALLBACK(hide_on_watchlist_changed), &fx.picker);
    if (by_row)
      g_signal_emit_by_name(list, "activate", 0u);
    else
      gtk_check_button_set_active(GTK_CHECK_BUTTON(toggle), TRUE);
    g_assert_true(stk_watchlist_contains(fx.watchlist, "AAA"));
    g_assert_cmpuint(fx.changed, ==, 1);
    g_assert_false(gtk_widget_get_visible(fx.picker));
    fx_clear(&fx);
  }
}

static void
test_full_watchlist(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  for (int i = 0; i < STK_WATCHLIST_MAX_ENTRIES; i++) {
    g_autofree char *sym = g_strdup_printf("S%04d", i);
    g_assert_true(stk_watchlist_add(fx.watchlist, sym, "", STK_KIND_STOCK));
  }
  WAIT_FOR(find_toggle(fx.picker, "stk-picker-catalog", "AAA") != NULL);
  GtkWidget *aaa = find_toggle(fx.picker, "stk-picker-catalog", "AAA");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(aaa), TRUE);

  /* refused: reverted, reported inline, nothing emitted */
  g_assert_false(toggle_active(aaa));
  g_assert_false(stk_watchlist_contains(fx.watchlist, "AAA"));
  g_assert_cmpuint(stk_watchlist_get_n_entries(fx.watchlist), ==, STK_WATCHLIST_MAX_ENTRIES);
  g_assert_cmpuint(fx.changed, ==, 0);
  g_assert_true(status_visible(fx.picker));
  g_assert_true(status_is_error(fx.picker));
  g_assert_nonnull(strstr(status_text(fx.picker), "full"));
  fx_clear(&fx);
}

/* The refusal message is removed by the next successful edit; an error of the
 * search is not an edit refusal and survives edits. */
static void
test_edit_error_cleared(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  GtkWidget *p = fx.picker;
  for (int i = 0; i < STK_WATCHLIST_MAX_ENTRIES - 1; i++) {
    g_autofree char *sym = g_strdup_printf("S%04d", i);
    g_assert_true(stk_watchlist_add(fx.watchlist, sym, "", STK_KIND_STOCK));
  }
  WAIT_FOR(find_toggle(p, "stk-picker-catalog", "AAA") != NULL);
  WAIT_FOR(find_toggle(p, "stk-picker-catalog", "BBB") != NULL);
  GtkWidget *aaa = find_toggle(p, "stk-picker-catalog", "AAA");
  GtkWidget *bbb = find_toggle(p, "stk-picker-catalog", "BBB");

  gtk_check_button_set_active(GTK_CHECK_BUTTON(aaa), TRUE); /* the last free slot */
  g_assert_true(stk_watchlist_contains(fx.watchlist, "AAA"));
  g_assert_cmpuint(fx.changed, ==, 1);
  g_assert_false(status_visible(p));

  gtk_check_button_set_active(GTK_CHECK_BUTTON(bbb), TRUE); /* refused */
  g_assert_false(toggle_active(bbb));
  g_assert_true(status_visible(p));
  g_assert_true(status_is_error(p));
  g_assert_nonnull(strstr(status_text(p), "full"));
  g_assert_cmpuint(fx.changed, ==, 1);

  gtk_check_button_set_active(GTK_CHECK_BUTTON(aaa), FALSE); /* a removal succeeds */
  g_assert_cmpuint(fx.changed, ==, 2);
  g_assert_false(status_visible(p));
  g_assert_false(status_is_error(p));

  gtk_check_button_set_active(GTK_CHECK_BUTTON(bbb), TRUE); /* an add succeeds */
  g_assert_true(stk_watchlist_contains(fx.watchlist, "BBB"));
  g_assert_cmpuint(fx.changed, ==, 3);
  g_assert_false(status_visible(p));

  /* a refusal followed by a successful add also clears it */
  g_assert_true(stk_watchlist_remove(fx.watchlist, "S0000"));
  g_assert_true(stk_watchlist_add(fx.watchlist, "S9999", "", STK_KIND_STOCK));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(aaa), TRUE); /* full: refused */
  g_assert_true(status_is_error(p));
  g_assert_true(stk_watchlist_remove(fx.watchlist, "S9999"));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(aaa), TRUE); /* accepted now */
  g_assert_true(stk_watchlist_contains(fx.watchlist, "AAA"));
  g_assert_false(status_visible(p));

  /* the search's own error is not touched by a successful edit */
  set_search_text(p, "one");
  WAIT_FOR(fx.fake->searches->len == 1);
  fake_fail(fx.fake, 0, g_error_new_literal(STK_ERROR, STK_ERROR_NETWORK, "boom"));
  WAIT_FOR(status_is_error(p));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(bbb), FALSE);
  g_assert_false(stk_watchlist_contains(fx.watchlist, "BBB"));
  g_assert_true(status_visible(p));
  g_assert_true(status_is_error(p));
  g_assert_cmpstr(status_text(p), ==, "Search failed: boom");
  fx_clear(&fx);
}

/* ------------------------------------------------------------------ */
/* P2: search                                                         */
/* ------------------------------------------------------------------ */

static void
test_debounce(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  GtkWidget *p = fx.picker;

  /* nothing without text, or with whitespace only */
  set_search_text(p, "   ");
  pump_ms(600);
  g_assert_cmpuint(fx.fake->searches->len, ==, 0);
  g_assert_false(status_visible(p));

  /* rapid typing: one search, for the final text, after the delay */
  gint64 t0 = g_get_monotonic_time();
  set_search_text(p, "a");
  set_search_text(p, "ap");
  set_search_text(p, "  app  ");
  g_assert_cmpuint(fx.fake->searches->len, ==, 0);
  WAIT_FOR(fx.fake->searches->len >= 1);
  g_assert_cmpint(g_get_monotonic_time() - t0, >=, 250 * 1000);
  pump_ms(700);
  g_assert_cmpuint(fx.fake->searches->len, ==, 1);
  g_assert_cmpstr(fake_pending(fx.fake, 0)->text, ==, "app");
  g_assert_false(g_cancellable_is_cancelled(fake_pending(fx.fake, 0)->cancellable));
  g_assert_true(status_visible(p)); /* "Searching..." */
  g_assert_false(status_is_error(p));
  fake_complete(fx.fake, 0, hits_new());
  WAIT_FOR(strcmp(status_text(p), "No results") == 0);
  g_assert_false(status_is_error(p));
  g_assert_false(results_visible(p));

  /* text removed before the timer fires: no search at all */
  set_search_text(p, "x");
  set_search_text(p, "");
  pump_ms(600);
  g_assert_cmpuint(fx.fake->searches->len, ==, 1);
  g_assert_false(status_visible(p));

  /* a second, later query is a second search */
  set_search_text(p, "msft");
  WAIT_FOR(fx.fake->searches->len == 2);
  g_assert_cmpstr(fake_pending(fx.fake, 1)->text, ==, "msft");
  fx_clear(&fx);
}

static GPtrArray *
sample_hits(void)
{
  GPtrArray *h = hits_new();
  hits_add(h, "MSFT", "Microsoft Corp", STK_KIND_STOCK);
  hits_add(h, "VTI", "Vanguard Total Stock Market", STK_KIND_ETF);
  hits_add(h, "FXAIX", "Fidelity 500 Index", STK_KIND_FUND);
  return h;
}

static void
test_results_and_toggle(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  GtkWidget *p = fx.picker;
  set_search_text(p, "micro");
  WAIT_FOR(fx.fake->searches->len == 1);

  GPtrArray *hits = sample_hits();
  hits_add(hits, "bad/sym", "invalid symbol", STK_KIND_STOCK); /* skipped */
  hits_add(hits, "BADKIND", "invalid kind", (StkKind)42);        /* skipped */
  GString *name = g_string_new(NULL);
  for (int i = 0; i < 300; i++)
    g_string_append(name, "\xC3\xA9");
  hits_add(hits, "LONGN", name->str, STK_KIND_ETF);
  g_string_free(name, TRUE);
  hits_add(hits, "ctrl", "A\tB\nC\x01", STK_KIND_STOCK);
  hits_add(hits, "BBB", "Beta (also in catalog)", STK_KIND_ETF);
  hits_add(hits, "^SPX", "Some Index", STK_KIND_INDEX); /* accepted, labelled "Index" */
  fake_complete(fx.fake, 0, hits);

  WAIT_FOR(results_visible(p));
  g_assert_false(status_visible(p));
  g_assert_cmpuint(list_n_items(p, "stk-picker-results"), ==, 7);
  assert_symbols(p, "stk-picker-results", "BBB CTRL FXAIX LONGN MSFT VTI ^SPX", 7);
  GtkWidget *t = find_toggle(p, "stk-picker-results", "MSFT");
  g_assert_cmpstr(row_label(t, 2), ==, "Stock");
  g_assert_cmpstr(row_label(t, 0), ==, "Microsoft Corp");
  g_assert_false(gtk_widget_get_visible(gtk_widget_get_last_child(
      gtk_widget_get_next_sibling(gtk_widget_get_next_sibling(t))))); /* no category */
  g_assert_cmpstr(row_label(find_toggle(p, "stk-picker-results", "VTI"), 2), ==, "ETF");
  g_assert_cmpstr(row_label(find_toggle(p, "stk-picker-results", "FXAIX"), 2), ==, "Fund");
  g_assert_cmpstr(row_label(find_toggle(p, "stk-picker-results", "^SPX"), 2), ==, "Index");

  /* toggling adds with the provider-reported kind */
  gtk_check_button_set_active(GTK_CHECK_BUTTON(t), TRUE);
  g_assert_cmpuint(fx.changed, ==, 1);
  const StkWatchlistEntry *e = stk_watchlist_get_entry(fx.watchlist, 0);
  g_assert_cmpstr(e->symbol, ==, "MSFT");
  g_assert_cmpstr(e->name, ==, "Microsoft Corp");
  g_assert_cmpint(e->kind, ==, STK_KIND_STOCK);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(find_toggle(p, "stk-picker-results", "FXAIX")),
                              TRUE);
  g_assert_cmpint(stk_watchlist_get_entry(fx.watchlist, 1)->kind, ==, STK_KIND_FUND);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(find_toggle(p, "stk-picker-results", "VTI")),
                              TRUE);
  g_assert_cmpint(stk_watchlist_get_entry(fx.watchlist, 2)->kind, ==, STK_KIND_ETF);

  /* over-long and control-character names are made safe, not rejected */
  gtk_check_button_set_active(GTK_CHECK_BUTTON(find_toggle(p, "stk-picker-results", "LONGN")),
                              TRUE);
  e = stk_watchlist_get_entry(fx.watchlist, 3);
  g_assert_cmpstr(e->symbol, ==, "LONGN");
  g_assert_cmpuint(strlen(e->name), ==, STK_WATCHLIST_MAX_NAME_BYTES);
  g_assert_true(g_utf8_validate(e->name, -1, NULL));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(find_toggle(p, "stk-picker-results", "CTRL")),
                              TRUE);
  e = stk_watchlist_get_entry(fx.watchlist, 4);
  g_assert_cmpstr(e->symbol, ==, "CTRL");
  g_assert_cmpstr(e->name, ==, "A B C ");
  g_assert_cmpuint(fx.changed, ==, 5);

  /* a result that is also a catalog entry: both lists follow the watchlist */
  WAIT_FOR(find_toggle(p, "stk-picker-catalog", "BBB") != NULL);
  GtkWidget *bbb_res = find_toggle(p, "stk-picker-results", "BBB");
  GtkWidget *bbb_cat = find_toggle(p, "stk-picker-catalog", "BBB");
  g_assert_false(toggle_active(bbb_res));
  g_assert_false(toggle_active(bbb_cat));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(bbb_res), TRUE);
  g_assert_true(toggle_active(bbb_cat));
  g_assert_true(stk_watchlist_remove(fx.watchlist, "BBB"));
  g_assert_false(toggle_active(bbb_res));
  g_assert_false(toggle_active(bbb_cat));
  g_assert_false(stk_watchlist_add(fx.watchlist, "VTI", "x", STK_KIND_ETF)); /* present */
  g_assert_true(toggle_active(find_toggle(p, "stk-picker-results", "VTI")));

  /* clearing the entry hides and empties the results */
  set_search_text(p, "");
  g_assert_cmpuint(list_n_items(p, "stk-picker-results"), ==, 0);
  g_assert_false(results_visible(p));
  fx_clear(&fx);
}

static void
test_result_cap(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  set_search_text(fx.picker, "t");
  WAIT_FOR(fx.fake->searches->len == 1);
  GPtrArray *hits = hits_new();
  for (int i = 0; i < 3 * STK_PICKER_MAX_RESULTS; i++) {
    g_autofree char *sym = g_strdup_printf("T%04d", i);
    hits_add(hits, sym, "n", STK_KIND_STOCK);
  }
  fake_complete(fx.fake, 0, hits);
  WAIT_FOR(list_n_items(fx.picker, "stk-picker-results") == STK_PICKER_MAX_RESULTS);
  /* the first ones are kept, in provider order */
  WAIT_FOR(find_toggle(fx.picker, "stk-picker-results", "T0000") != NULL);
  fx_clear(&fx);
}

/* Completes request #0 (the superseded one) with a result or an error. */
static void
complete_stale(Fx *fx, gboolean fails)
{
  if (fails)
    fake_fail(fx->fake, 0, g_error_new_literal(STK_ERROR, STK_ERROR_NETWORK, "stale boom"));
  else
    fake_complete(fx->fake, 0, sample_hits());
}

/* A completion that arrives after a newer query was typed is dropped.
 * @honours_cancel says whether the fake honours cancellation (the real
 * contract) or ignores it (a sloppy provider); @stale_first the completion
 * order; @stale_fails whether the stale request fails instead of succeeding. */
static void
run_stale(gboolean honours_cancel, gboolean stale_first, gboolean stale_fails)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  GtkWidget *p = fx.picker;
  fx.fake->ignore_cancel = !honours_cancel;

  set_search_text(p, "old");
  WAIT_FOR(fx.fake->searches->len == 1);
  set_search_text(p, "new");
  /* the previous request is cancelled as soon as the text changes */
  g_assert_true(g_cancellable_is_cancelled(fake_pending(fx.fake, 0)->cancellable));
  WAIT_FOR(fx.fake->searches->len == 2);
  g_assert_false(g_cancellable_is_cancelled(fake_pending(fx.fake, 1)->cancellable));
  g_assert_cmpstr(fake_pending(fx.fake, 1)->text, ==, "new");

  GPtrArray *newer = hits_new();
  hits_add(newer, "VTI", "Vanguard", STK_KIND_ETF);

  if (stale_first) {
    complete_stale(&fx, stale_fails);
    pump_ms(50);
    g_assert_false(results_visible(p));
    g_assert_cmpuint(list_n_items(p, "stk-picker-results"), ==, 0);
    g_assert_false(status_is_error(p));
    g_assert_null(strstr(status_text(p), "boom"));
    fake_complete(fx.fake, 1, newer);
  } else {
    fake_complete(fx.fake, 1, newer);
    WAIT_FOR(list_n_items(p, "stk-picker-results") == 1);
    complete_stale(&fx, stale_fails);
    pump_ms(50);
  }
  WAIT_FOR(list_n_items(p, "stk-picker-results") == 1);
  g_assert_true(results_visible(p));
  g_assert_false(status_visible(p));
  assert_symbols(p, "stk-picker-results", "VTI", 1);
  fx_clear(&fx);
}

static void
test_stale_results(void)
{
  for (int honours = 0; honours < 2; honours++)
    for (int first = 0; first < 2; first++)
      for (int fails = 0; fails < 2; fails++)
        run_stale(honours, first, fails);
}

static void
test_cancellation(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  GtkWidget *p = fx.picker;

  /* emptying the entry cancels the in-flight search; its completion is silent */
  set_search_text(p, "q");
  WAIT_FOR(fx.fake->searches->len == 1);
  g_assert_true(status_visible(p)); /* Searching... */
  set_search_text(p, "");
  g_assert_true(g_cancellable_is_cancelled(fake_pending(fx.fake, 0)->cancellable));
  g_assert_false(status_visible(p));
  fake_fail(fx.fake, 0, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled"));
  pump_ms(50);
  g_assert_false(status_visible(p));
  g_assert_false(results_visible(p));

  /* a search that completes is no longer cancelled by later edits (nothing
   * is in flight) and its results survive until the text changes */
  set_search_text(p, "ok");
  WAIT_FOR(fx.fake->searches->len == 2);
  fake_complete(fx.fake, 1, sample_hits());
  WAIT_FOR(results_visible(p));
  g_assert_false(g_cancellable_is_cancelled(fake_pending(fx.fake, 1)->cancellable));

  /* a provider that reports CANCELLED for the current request (nobody
   * cancelled it) clears the "Searching" message without an error */
  set_search_text(p, "weird");
  WAIT_FOR(fx.fake->searches->len == 3);
  g_assert_true(status_visible(p));
  fake_fail(fx.fake, 2, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled"));
  WAIT_FOR(!status_visible(p));
  g_assert_false(status_is_error(p));
  fx_clear(&fx);
}

static void
test_errors(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  GtkWidget *p = fx.picker;

  /* first show results, then fail: results are cleared and the error shown */
  set_search_text(p, "one");
  WAIT_FOR(fx.fake->searches->len == 1);
  fake_complete(fx.fake, 0, sample_hits());
  WAIT_FOR(results_visible(p));

  set_search_text(p, "two");
  WAIT_FOR(fx.fake->searches->len == 2);
  fake_fail(fx.fake, 1,
            g_error_new_literal(STK_ERROR, STK_ERROR_NETWORK, "boom\x01\n\t<b>x</b> & done"));
  WAIT_FOR(status_is_error(p));
  g_assert_true(status_visible(p));
  g_assert_cmpstr(status_text(p), ==, "Search failed: boom   <b>x</b> & done");
  g_assert_false(results_visible(p));
  g_assert_cmpuint(list_n_items(p, "stk-picker-results"), ==, 0);

  /* huge and invalid messages are bounded and made valid */
  set_search_text(p, "three");
  WAIT_FOR(fx.fake->searches->len == 3);
  g_autofree char *big = g_strnfill(20000, 'x');
  fake_fail(fx.fake, 2, g_error_new_literal(STK_ERROR, STK_ERROR_HTTP, big));
  WAIT_FOR(strlen(status_text(p)) > 20);
  g_assert_cmpuint(strlen(status_text(p)), ==,
                   strlen("Search failed: ") + STK_PICKER_MAX_MESSAGE_BYTES);

  set_search_text(p, "four");
  WAIT_FOR(fx.fake->searches->len == 4);
  g_autofree char *bad = g_strdup("ab\xff\xfe\xC3"); /* invalid UTF-8, truncated sequence */
  fake_fail(fx.fake, 3, g_error_new_literal(STK_ERROR, STK_ERROR_PARSE, bad));
  WAIT_FOR(strstr(status_text(p), "ab") != NULL);
  g_assert_true(g_utf8_validate(status_text(p), -1, NULL));
  g_assert_true(status_is_error(p));

  /* multi-byte text is cut on a character boundary */
  set_search_text(p, "five");
  WAIT_FOR(fx.fake->searches->len == 5);
  GString *mb = g_string_new(NULL);
  for (int i = 0; i < 500; i++)
    g_string_append(mb, "\xE2\x82\xAC"); /* 3 bytes each */
  fake_fail(fx.fake, 4, g_error_new_literal(STK_ERROR, STK_ERROR_PARSE, mb->str));
  g_string_free(mb, TRUE);
  WAIT_FOR(strstr(status_text(p), "\xE2\x82\xAC") != NULL);
  g_assert_true(g_utf8_validate(status_text(p), -1, NULL));
  g_assert_cmpuint(strlen(status_text(p)), <=,
                   strlen("Search failed: ") + STK_PICKER_MAX_MESSAGE_BYTES);

  /* the provider's own argument rejection is shown like any other error */
  set_search_text(p, "six");
  WAIT_FOR(fx.fake->searches->len == 6);
  fake_fail(fx.fake, 5, g_error_new_literal(STK_ERROR, STK_ERROR_INVALID_ARG, "bad text"));
  WAIT_FOR(strcmp(status_text(p), "Search failed: bad text") == 0);

  /* success clears the error; an empty result is a plain note */
  set_search_text(p, "seven");
  WAIT_FOR(fx.fake->searches->len == 7);
  fake_complete(fx.fake, 6, hits_new());
  WAIT_FOR(strcmp(status_text(p), "No results") == 0);
  g_assert_false(status_is_error(p));
  set_search_text(p, "eight");
  WAIT_FOR(fx.fake->searches->len == 8);
  fake_complete(fx.fake, 7, sample_hits());
  WAIT_FOR(results_visible(p));
  g_assert_false(status_visible(p));
  g_assert_false(status_is_error(p));
  fx_clear(&fx);
}

/* ------------------------------------------------------------------ */
/* P3: keyboard, close, dispose                                       */
/* ------------------------------------------------------------------ */

/* Emits "key-pressed" on the picker's capture-phase key controller(s);
 * returns whether one of them handled it. */
static gboolean
press_key(GtkWidget *picker, guint keyval, GdkModifierType state)
{
  gboolean handled = FALSE;
  GListModel *controllers = gtk_widget_observe_controllers(picker);
  for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
    g_autoptr(GObject) c = g_list_model_get_item(controllers, i);
    if (GTK_IS_EVENT_CONTROLLER_KEY(c) &&
        gtk_event_controller_get_propagation_phase(GTK_EVENT_CONTROLLER(c)) == GTK_PHASE_CAPTURE) {
      gboolean ret = FALSE;
      g_signal_emit_by_name(c, "key-pressed", keyval, 0u, state, &ret);
      handled = handled || ret;
    }
  }
  g_object_unref(controllers);
  return handled;
}

static void
test_escape(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  GtkWidget *p = fx.picker;

  /* other keys do nothing */
  g_assert_false(press_key(p, GDK_KEY_a, 0));
  g_assert_false(press_key(p, GDK_KEY_Escape, GDK_CONTROL_MASK));
  g_assert_cmpuint(fx.close_requests, ==, 0);
  g_assert_true(gtk_widget_get_visible(p));

  /* Escape with a pending debounce timer and an in-flight search */
  set_search_text(p, "abc");
  WAIT_FOR(fx.fake->searches->len == 1);
  set_search_text(p, "abcd"); /* cancels #0, arms the timer again */
  g_assert_true(press_key(p, GDK_KEY_Escape, 0));
  g_assert_cmpuint(fx.close_requests, ==, 1);
  WAIT_FOR(fx.picker == NULL);
  pump_ms(700); /* the cancelled timer must not fire */
  g_assert_cmpuint(fx.fake->searches->len, ==, 1);
  g_assert_true(g_cancellable_is_cancelled(fake_pending(fx.fake, 0)->cancellable));
  fx_clear(&fx);

  /* Escape cancels an in-flight request */
  fx_init(&fx, FALSE, TRUE);
  set_search_text(fx.picker, "abc");
  WAIT_FOR(fx.fake->searches->len == 1);
  g_assert_false(g_cancellable_is_cancelled(fake_pending(fx.fake, 0)->cancellable));
  g_assert_true(press_key(fx.picker, GDK_KEY_Escape, 0));
  g_assert_true(g_cancellable_is_cancelled(fake_pending(fx.fake, 0)->cancellable));
  WAIT_FOR(fx.picker == NULL);
  fx_clear(&fx);
}

static gboolean
veto_close(GtkWindow *window, gpointer user_data)
{
  (void)window;
  (*(guint *)user_data)++;
  return TRUE;
}

static void
test_close_veto(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  guint vetoes = 0;
  g_signal_connect(fx.picker, "close-request", G_CALLBACK(veto_close), &vetoes);
  set_search_text(fx.picker, "abc");
  WAIT_FOR(fx.fake->searches->len == 1);
  gtk_window_close(GTK_WINDOW(fx.picker));
  g_assert_cmpuint(vetoes, ==, 1);
  /* a vetoed close leaves the window and its search alone */
  g_assert_true(gtk_widget_get_visible(fx.picker));
  g_assert_false(g_cancellable_is_cancelled(fake_pending(fx.fake, 0)->cancellable));
  fake_complete(fx.fake, 0, sample_hits());
  WAIT_FOR(results_visible(fx.picker));
  fx_clear(&fx);
}

/* gtk_window_set_hide_on_close(): the window survives the close request, so
 * neither the in-flight search nor the pending debounce timer may be lost. */
static void
test_hide_on_close(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  GtkWidget *p = fx.picker;
  gtk_window_set_hide_on_close(GTK_WINDOW(p), TRUE);

  set_search_text(p, "msft");
  WAIT_FOR(fx.fake->searches->len == 1);
  gtk_window_close(GTK_WINDOW(p));
  g_assert_cmpuint(fx.close_requests, ==, 1);
  g_assert_false(gtk_widget_get_visible(p));
  g_assert_false(g_cancellable_is_cancelled(fake_pending(fx.fake, 0)->cancellable));
  fake_complete(fx.fake, 0, sample_hits());
  gtk_window_present(GTK_WINDOW(p));
  WAIT_FOR(results_visible(p));
  g_assert_false(status_visible(p));
  g_assert_cmpuint(list_n_items(p, "stk-picker-results"), ==, 3);

  set_search_text(p, "abc"); /* debounce timer pending when the window is hidden */
  gtk_window_close(GTK_WINDOW(p));
  g_assert_false(gtk_widget_get_visible(p));
  WAIT_FOR(fx.fake->searches->len == 2);
  g_assert_cmpstr(fake_pending(fx.fake, 1)->text, ==, "abc");
  g_assert_false(g_cancellable_is_cancelled(fake_pending(fx.fake, 1)->cancellable));
  fake_complete(fx.fake, 1, hits_new());
  gtk_window_present(GTK_WINDOW(p));
  WAIT_FOR(strcmp(status_text(p), "No results") == 0);
  g_assert_cmpuint(fx.fake->searches->len, ==, 2);
  fx_clear(&fx);
}

/* A veto from a handler that runs after the class handler (so after the
 * point where the window would have been destroyed) leaves the search alone. */
static void
test_close_veto_after(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  GtkWidget *p = fx.picker;
  guint vetoes = 0;
  g_signal_connect_after(p, "close-request", G_CALLBACK(veto_close), &vetoes);
  set_search_text(p, "abc");
  WAIT_FOR(fx.fake->searches->len == 1);
  set_search_text(p, "abcd"); /* cancels #0 and arms the timer again */
  g_assert_true(g_cancellable_is_cancelled(fake_pending(fx.fake, 0)->cancellable));
  gtk_window_close(GTK_WINDOW(p));
  g_assert_cmpuint(vetoes, ==, 1);
  g_assert_true(gtk_widget_get_visible(p));
  WAIT_FOR(fx.fake->searches->len == 2);
  g_assert_cmpstr(fake_pending(fx.fake, 1)->text, ==, "abcd");
  g_assert_false(g_cancellable_is_cancelled(fake_pending(fx.fake, 1)->cancellable));
  g_assert_true(status_visible(p)); /* Searching... */
  fake_complete(fx.fake, 1, sample_hits());
  WAIT_FOR(results_visible(p));
  g_assert_false(status_visible(p));
  fx_clear(&fx);
}

/* gtk_window_destroy() does not dispose a picker whose owner still holds a
 * reference; the picker nevertheless cancels its timer and its request. */
static void
test_destroy_with_owner_ref(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  GtkWidget *p = fx.picker;
  GtkWidget *owner = g_object_ref(p);
  set_search_text(p, "msft"); /* timer pending */
  gtk_window_destroy(GTK_WINDOW(p));
  pump_ms(700);
  g_assert_cmpuint(fx.fake->searches->len, ==, 0);
  g_object_unref(owner);
  WAIT_FOR(fx.picker == NULL);
  fx_clear(&fx);

  fx_init(&fx, FALSE, TRUE);
  p = fx.picker;
  owner = g_object_ref(p);
  set_search_text(p, "msft");
  WAIT_FOR(fx.fake->searches->len == 1);
  g_assert_true(status_visible(p)); /* Searching... */
  gtk_window_destroy(GTK_WINDOW(p));
  g_assert_nonnull(fx.picker);
  g_assert_true(g_cancellable_is_cancelled(fake_pending(fx.fake, 0)->cancellable));
  g_assert_false(status_visible(p));
  fake_complete(fx.fake, 0, sample_hits()); /* late completion: dropped */
  pump_ms(50);
  g_assert_cmpuint(list_n_items(p, "stk-picker-results"), ==, 0);
  g_assert_false(status_visible(p));
  g_object_unref(owner);
  WAIT_FOR(fx.picker == NULL);
  fx_clear(&fx);
}

static void
test_dispose_in_flight(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  set_search_text(fx.picker, "abc");
  WAIT_FOR(fx.fake->searches->len == 1);
  gtk_window_destroy(GTK_WINDOW(fx.picker));
  WAIT_FOR(fx.picker == NULL); /* the request does not keep the picker alive */
  g_assert_true(g_cancellable_is_cancelled(fake_pending(fx.fake, 0)->cancellable));
  /* late completions, success and failure, are dropped */
  fake_complete(fx.fake, 0, sample_hits());
  pump_ms(50);
  fx_clear(&fx);

  fx_init(&fx, FALSE, TRUE);
  fx.fake->ignore_cancel = TRUE;
  set_search_text(fx.picker, "abc");
  WAIT_FOR(fx.fake->searches->len == 1);
  gtk_window_destroy(GTK_WINDOW(fx.picker));
  WAIT_FOR(fx.picker == NULL);
  fake_fail(fx.fake, 0, g_error_new_literal(STK_ERROR, STK_ERROR_NETWORK, "late"));
  pump_ms(50);
  fx_clear(&fx);
}

static void
test_dispose_pending_timer(void)
{
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  set_search_text(fx.picker, "abc");
  gtk_window_destroy(GTK_WINDOW(fx.picker));
  WAIT_FOR(fx.picker == NULL);
  pump_ms(700);
  g_assert_cmpuint(fx.fake->searches->len, ==, 0);
  fx_clear(&fx);

  /* never presented */
  fx_init(&fx, FALSE, FALSE);
  set_search_text(fx.picker, "abc");
  gtk_window_destroy(GTK_WINDOW(fx.picker));
  WAIT_FOR(fx.picker == NULL);
  pump_ms(700);
  g_assert_cmpuint(fx.fake->searches->len, ==, 0);
  fx_clear(&fx);
}

static void
test_watchlist_outlives_picker(void)
{
  /* after the picker is gone, watchlist edits must not reach it */
  Fx fx;
  fx_init(&fx, FALSE, TRUE);
  WAIT_FOR(find_toggle(fx.picker, "stk-picker-catalog", "AAA") != NULL);
  gtk_window_destroy(GTK_WINDOW(fx.picker));
  WAIT_FOR(fx.picker == NULL);
  g_assert_true(stk_watchlist_add(fx.watchlist, "AAA", "a", STK_KIND_FUND));
  g_assert_true(stk_watchlist_remove(fx.watchlist, "AAA"));
  g_assert_cmpuint(fx.changed, ==, 0);
  fx_clear(&fx);
}

static void
test_bad_arguments(void)
{
  g_autoptr(StkCatalog) catalog = stk_catalog_new_from_data(CATALOG_JSON, strlen(CATALOG_JSON), NULL);
  g_autoptr(StkWatchlist) wl = stk_watchlist_new("/nonexistent-dir/x.json");
  g_autoptr(FakeProvider) fake = g_object_new(FAKE_TYPE_PROVIDER, NULL);
  g_test_expect_message("stocks", G_LOG_LEVEL_CRITICAL, "*catalog != NULL*");
  g_assert_null(stk_picker_new(NULL, wl, STK_PROVIDER(fake)));
  g_test_expect_message("stocks", G_LOG_LEVEL_CRITICAL, "*STK_IS_WATCHLIST*");
  g_assert_null(stk_picker_new(catalog, NULL, STK_PROVIDER(fake)));
  g_test_expect_message("stocks", G_LOG_LEVEL_CRITICAL, "*STK_IS_PROVIDER*");
  g_assert_null(stk_picker_new(catalog, wl, NULL));
  g_test_assert_expected_messages();
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

/* g_test_init() makes every warning fatal, but the installed GTK theme itself
 * may log parser warnings for its own stylesheet while it loads; those are not
 * ours and are reported without failing. Any other warning or critical still
 * aborts the test. */
static GLogWriterOutput
log_writer(GLogLevelFlags level, const GLogField *fields, gsize n_fields, gpointer user_data)
{
  if (level & G_LOG_LEVEL_WARNING) {
    for (gsize i = 0; i < n_fields; i++) {
      if (strcmp(fields[i].key, "MESSAGE") == 0 && fields[i].value != NULL &&
          g_str_has_prefix(fields[i].value, "Theme parser error: gtk.css")) {
        g_printerr("# ignored (GTK theme): %s\n", (const char *)fields[i].value);
        return G_LOG_WRITER_HANDLED;
      }
    }
  }
  return g_log_writer_default(level, fields, n_fields, user_data);
}

int
main(int argc, char **argv)
{
  g_log_set_writer_func(log_writer, NULL, NULL);
  g_test_init(&argc, &argv, NULL);
  /* Pin the X11 backend (Xvfb) so the test never touches a live compositor. */
  g_setenv("GDK_BACKEND", "x11", TRUE);
  gdk_set_allowed_backends("x11");
  if (!gtk_init_check()) {
    g_print("1..0 # SKIP no display\n");
    return 77;
  }
  g_assert_true(g_str_has_prefix(G_OBJECT_TYPE_NAME(gdk_display_get_default()), "GdkX11"));

  g_test_add_func("/picker/filters", test_filters);
  g_test_add_func("/picker/default-catalog", test_default_catalog_virtualised);
  g_test_add_func("/picker/toggle-sync", test_toggle_sync);
  g_test_add_func("/picker/row-activation", test_row_activation);
  g_test_add_func("/picker/keyboard/navigation", test_keyboard_navigation);
  g_test_add_func("/picker/keyboard/toggle", test_keyboard_toggle);
  g_test_add_func("/picker/destroy-in-handler", test_destroy_in_handler);
  g_test_add_func("/picker/destroy-in-watchlist-handler", test_destroy_in_watchlist_handler);
  g_test_add_func("/picker/hide-in-watchlist-handler", test_hide_in_watchlist_handler);
  g_test_add_func("/picker/full-watchlist", test_full_watchlist);
  g_test_add_func("/picker/edit-error-cleared", test_edit_error_cleared);
  g_test_add_func("/picker/search/debounce", test_debounce);
  g_test_add_func("/picker/search/results-and-toggle", test_results_and_toggle);
  g_test_add_func("/picker/search/result-cap", test_result_cap);
  g_test_add_func("/picker/search/stale", test_stale_results);
  g_test_add_func("/picker/search/cancellation", test_cancellation);
  g_test_add_func("/picker/search/errors", test_errors);
  g_test_add_func("/picker/escape", test_escape);
  g_test_add_func("/picker/close-veto", test_close_veto);
  g_test_add_func("/picker/close-veto-after", test_close_veto_after);
  g_test_add_func("/picker/hide-on-close", test_hide_on_close);
  g_test_add_func("/picker/dispose/owner-ref", test_destroy_with_owner_ref);
  g_test_add_func("/picker/dispose/in-flight", test_dispose_in_flight);
  g_test_add_func("/picker/dispose/pending-timer", test_dispose_pending_timer);
  g_test_add_func("/picker/dispose/watchlist-outlives", test_watchlist_outlives_picker);
  g_test_add_func("/picker/bad-arguments", test_bad_arguments);
  return g_test_run();
}
