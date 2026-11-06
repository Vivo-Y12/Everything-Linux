/*
 * Everything-1.0.0 — application entry point
 *
 * SPDX-License-Identifier: MIT
 */
#include "everything-config.h"
#include "everything-index.h"
#include "everything-window.h"

#include <adwaita.h>
#include <glib.h>
#include <gtk/gtk.h>

static EverythingConfig *cfg = NULL;
static EverythingIndex  *idx = NULL;

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;
    if (!idx) {
        GError *err = NULL;
        idx = everything_index_new(cfg, &err);
        if (!idx) {
            /* GTK 4.10+ replaces GtkMessageDialog with GtkAlertDialog;
             * fall back to logging on the console if construction fails. */
            g_critical("Failed to open index: %s",
                       err ? err->message : "?");
            g_clear_error(&err);
            return;
        }
    }

    EverythingWindow *win = everything_window_new(app, idx);
    gtk_application_add_window(app, GTK_WINDOW(win));
    gtk_window_present(GTK_WINDOW(win));
}

static int
on_command_line(GApplication *gapp, GApplicationCommandLine *cmd, gpointer u)
{
    (void)u;
    /* Forward --help and basic options to the standard handler. */
    GVariantDict *opts = g_application_command_line_get_options_dict(cmd);
    (void)opts;
    g_application_activate(gapp);
    return 0;
}

int
main(int argc, char **argv)
{
    cfg = everything_config_new();
    GError *err = NULL;
    everything_config_load(cfg, &err);
    if (err) { g_clear_error(&err); }

    AdwApplication *app = adw_application_new(
        APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate",       G_CALLBACK(on_activate),       NULL);
    g_signal_connect(app, "command-line",   G_CALLBACK(on_command_line),   NULL);

    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    if (idx) everything_index_stop(idx);
    if (idx) everything_index_free(idx);
    everything_config_save(cfg, NULL);
    everything_config_free(cfg);
    return rc;
}
