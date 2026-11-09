/*
 * Everything-1.0.0 — Filesystem indexer
 *
 * Responsibilities:
 *   - Create and maintain an SQLite index of all files under the
 *     configured roots, using FTS5 for instant substring search.
 *   - Crawl the filesystem with a worker thread pool.
 *   - Maintain a live inotify watcher for incremental updates.
 *   - Provide a thread-safe query API returning ResultRow arrays.
 *
 * The original voidtools Everything reads the NTFS Master File Table
 * directly via DeviceIoControl(FSCTL_*) calls, which is impossible on
 * Linux because ext4/Btrfs/XFS have no equivalent of an MFT: filenames
 * live inside directory inodes. We therefore crawl the directory tree
 * once and rely on inotify for incremental updates. A first-time full
 * crawl of a typical Linux desktop (~500k files) completes in a few
 * seconds on a modern SSD, and subsequent startups reuse the on-disk
 * SQLite index and refresh in the background.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef EVERYTHING_INDEX_H
#define EVERYTHING_INDEX_H

#include <glib.h>
#include <sqlite3.h>
#include "everything-types.h"
#include "everything-config.h"

typedef struct _EverythingIndex EverythingIndex;

typedef void (* IndexProgressCallback)(EverythingIndex *idx,
                                       guint64 visited,
                                       guint64 total,
                                       gpointer user_data);

typedef void (* IndexStateCallback)(EverythingIndex *idx,
                                    IndexState new_state,
                                    const char *message,
                                    gpointer user_data);

typedef void (* IndexChangedCallback)(EverythingIndex *idx,
                                      gpointer user_data);

EverythingIndex *everything_index_new(EverythingConfig *cfg, GError **error);
void             everything_index_free(EverythingIndex *idx);

/* Build (or rebuild) the on-disk index from scratch. Non-blocking: spawns
 * worker threads and returns immediately. Progress/state callbacks fire
 * from worker threads, so the caller must marshal to the main thread. */
gboolean         everything_index_rebuild(EverythingIndex *idx, GError **error);

/* Verify the on-disk index exists and load it; if missing, kick off a
 * rebuild. */
gboolean         everything_index_ensure(EverythingIndex *idx, GError **error);

/* Stop any running crawler and the inotify watcher. */
void             everything_index_stop(EverythingIndex *idx);

/* Statistics. */
guint64          everything_index_count(EverythingIndex *idx);
IndexState       everything_index_state(EverythingIndex *idx);
const char      *everything_index_state_message(EverythingIndex *idx);

/* Callbacks. */
void             everything_index_set_progress_cb(EverythingIndex *idx,
                                                  IndexProgressCallback cb,
                                                  gpointer user_data);
void             everything_index_set_state_cb(EverythingIndex *idx,
                                               IndexStateCallback cb,
                                               gpointer user_data);
void             everything_index_set_changed_cb(EverythingIndex *idx,
                                                 IndexChangedCallback cb,
                                                 gpointer user_data);

/* SQLite handle (read-only queries by the search engine). */
sqlite3         *everything_index_db(EverythingIndex *idx);

/* On-disk path of the SQLite index. */
const char      *everything_index_db_path(EverythingIndex *idx);

/* Counts of visited entries since last rebuild. */
guint64          everything_index_visited_files(EverythingIndex *idx);
guint64          everything_index_visited_dirs(EverythingIndex *idx);

/* Count of active inotify watches. */
guint            everything_index_watch_count(EverythingIndex *idx);

#endif /* EVERYTHING_INDEX_H */
