/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 wtp128pro */
/* main.c - entry point of the stocks desktop app. */
#include <glib-unix.h>
#include <glib.h>
#include <signal.h>

#include "stk-app.h"

/**
 * on_terminate:
 * @user_data: (not nullable): the StkApp (a GApplication) the signal sources
 *   were registered for (borrowed raw pointer).
 *
 * SIGINT / SIGTERM close the app the same way closing the window does: the
 * main loop ends, the application's shutdown flushes pending saves. The
 * source stays installed, so a second signal during the shutdown wait calls
 * g_application_quit() again, which only repeats the quit request (the wait
 * is not cut short).
 *
 * Preconditions:
 *  - dispatched by the default main context on the main thread, i.e. only while a main
 *    context is iterated. main() installs the sources before g_application_run() and never
 *    removes them, and main() itself iterates no main context after g_application_run()
 *    returns; @user_data is alive while g_application_run() iterates the main context.
 * Postconditions:
 *  - g_application_quit() was called on @user_data; the source is kept.
 * Thread affinity: main thread (default main context).
 * Ownership: @user_data is borrowed; no reference is taken or released.
 *
 * Returns: G_SOURCE_CONTINUE always (the source stays installed until the process exits).
 */
static gboolean
on_terminate(gpointer user_data)
{
  g_application_quit(G_APPLICATION(user_data));
  return G_SOURCE_CONTINUE;
}

/**
 * main:
 * @argc: number of entries in @argv, as given to the process.
 * @argv: (array length=argc): the command line; passed to g_application_run().
 *
 * Entry point: names the program, creates the application, routes SIGINT and
 * SIGTERM to a clean quit and runs the application until it exits.
 *
 * Preconditions:
 *  - called once by the C runtime; @argv has @argc valid entries.
 * Postconditions:
 *  - both signal sources stay installed until the process exits (they are never removed, so
 *    the default SIGINT/SIGTERM disposition is never restored); the application reference is
 *    released on return.
 * Thread affinity: main thread.
 * Ownership: the application is owned by main() (g_autoptr) and released on
 *   return; @argv is borrowed by g_application_run().
 *
 * Returns: the exit status of g_application_run().
 */
int
main(int argc, char **argv)
{
  g_set_prgname("stocks");
  g_set_application_name("Stocks");
  gtk_window_set_default_icon_name(STK_APP_ID);

  g_autoptr(StkApp) app = stk_app_new();
  g_unix_signal_add(SIGINT, on_terminate, app);
  g_unix_signal_add(SIGTERM, on_terminate, app);
  return g_application_run(G_APPLICATION(app), argc, argv);
}
