/*
 * Everything-1.0.0 — Native Linux port of voidtools Everything
 * Common types and constants shared across the indexer, search engine and UI.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 voidtools (original Everything)
 *                         2026 Everything-1.0.0 Linux port contributors
 */
#ifndef EVERYTHING_TYPES_H
#define EVERYTHING_TYPES_H

#include <glib.h>
#include <glib-object.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>

#define APP_ID              "org.voidtools.Everything"
#define APP_DISPLAY_NAME    "Everything"
#define APP_VERSION         "1.0.0"

/* Default roots to crawl when the user does not configure a custom list.
 * These are the same directories that an end-user would expect to be
 * indexed on a typical Linux desktop install. */
#define DEFAULT_INDEX_ROOTS "/,/home"

/* Virtual / kernel-only filesystems that must never be crawled even if
 * the user explicitly requests them: they either do not contain real
 * files, are infinitely deep, or change with every read. */
#define SKIP_DIRS_EXACT     "/proc,/sys,/dev,/run,/snap,/var/lib/flatpak,/var/lib/snapd"

/* Number of worker threads used by the crawler. 0 == auto (g_get_num_processors). */
#define DEFAULT_WORKER_THREADS 0

/* Soft cap on the number of inotify watches. The Linux default is 8192,
 * which is too low for a full-disk index. The app will still run if the
 * cap is hit but will log a warning recommending
 *   sysctl fs.inotify.max_user_watches=524288 */
#define INOTIFY_WARN_LIMIT   8192

typedef enum {
    INDEX_STATE_IDLE = 0,
    INDEX_STATE_CRAWLING,
    INDEX_STATE_WATCHING,
    INDEX_STATE_PAUSED,
    INDEX_STATE_ERROR
} IndexState;

/* Plain C struct used internally by the indexer/search engine. */
typedef struct {
    char       *fullpath;   /* /parent/name */
    char       *name;       /* basename */
    char       *path;       /* dirname (parent, no trailing slash) */
    char       *ext;        /* lowercase extension without dot, may be NULL */
    uint64_t    size;
    int         is_dir;
    time_t      mtime;
    time_t      ctime;
    time_t      atime;
    mode_t      mode;
    char        attrs[11];  /* "drwxr-xr-x" + nul */
} EntryInfo;

/* GObject wrapper around the same data, used by the UI's GListStore.
 * GListStore requires items to be GType-registered GObjects, so the
 * search engine returns plain ResultRow structs which the UI wraps. */
#define EVERYTHING_TYPE_RESULT_ROW (everything_result_row_get_type())
G_DECLARE_FINAL_TYPE(EverythingResultRow, everything_result_row,
                     EVERYTHING, RESULT_ROW, GObject)

struct _EverythingResultRow {
    GObject parent_instance;

    int64_t  id;
    char    *name;
    char    *path;
    char    *fullpath;
    uint64_t size;
    int      is_dir;
    char    *ext;
    char    *type_label;     /* "Folder", "File", or extension uppercased */
    char    *attrs;
    time_t   mtime;
    time_t   ctime;
    time_t   atime;
};

/* Backwards-compat alias used by the search engine. */
typedef EverythingResultRow ResultRow;

GType           everything_result_row_get_type(void);
EverythingResultRow *everything_result_row_new(void);

#endif /* EVERYTHING_TYPES_H */
