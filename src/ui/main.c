/* main.c - entry point of the stocks desktop app. */
#include <glib-unix.h>
#include <glib.h>
#include <signal.h>

#include "stk-app.h"

/* SIGINT / SIGTERM close the app the same way closing the window does: the
 * main loop ends, the application's shutdown flushes pending saves. */
static gboolean
on_terminate(gpointer user_data)
{
  g_application_quit(G_APPLICATION(user_data));
  return G_SOURCE_CONTINUE;
}

int
main(int argc, char **argv)
{
  g_set_prgname("stocks");
  g_set_application_name("Stocks");

  g_autoptr(StkApp) app = stk_app_new();
  g_unix_signal_add(SIGINT, on_terminate, app);
  g_unix_signal_add(SIGTERM, on_terminate, app);
  return g_application_run(G_APPLICATION(app), argc, argv);
}
