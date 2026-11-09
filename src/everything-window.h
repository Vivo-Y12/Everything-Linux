/*
 * Everything-1.0.0 — Main GTK4 application window
 *
 * Reproduces the user-visible layout of voidtools Everything:
 *   - Header bar with a prominent search entry (matched-substring
 *     highlighting is a future enhancement; this version uses plain
 *     text).
 *   - A sortable GtkColumnView with columns: Name, Path, Size,
 *     Extension/Type, Date Modified, Attributes.
 *   - A status bar showing index state and result count.
 *   - A right-click context menu: Open, Open Path, Copy Path,
 *     Copy Name, Copy Full Name, Properties.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef EVERYTHING_WINDOW_H
#define EVERYTHING_WINDOW_H

#include <adwaita.h>
#include <gtk/gtk.h>
#include "everything-index.h"

#define EVERYTHING_TYPE_WINDOW (everything_window_get_type())
G_DECLARE_FINAL_TYPE(EverythingWindow, everything_window,
                     EVERYTHING, WINDOW, AdwApplicationWindow)

EverythingWindow *everything_window_new(GtkApplication *app,
                                        EverythingIndex *idx);

/* Re-run the search against the current query; called from the search
 * entry "changed" signal handler (debounced) and after index changes. */
void everything_window_refresh(EverythingWindow *win);

/* Trigger a full rebuild from the UI (Tools menu / Ctrl+R). */
void everything_window_rebuild_index(EverythingWindow *win);

#endif /* EVERYTHING_WINDOW_H */
