/*
 * Everything-1.0.0 — Filesystem indexer implementation
 *
 * SPDX-License-Identifier: MIT
 */
#include "everything-index.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Tunables */
#define DB_SCHEMA_VERSION     1
#define BATCH_INSERT_SIZE     512
#define CRAWLER_QUEUE_MAX     8192

/* ------------------------------------------------------------------ *
 * Internal types
 * ------------------------------------------------------------------ */

struct _EverythingIndex {
    EverythingConfig    *cfg;
    sqlite3             *db;
    char                *db_path;

    /* Cached skip-list — populated once at construction time so that
     * should_skip() doesn't allocate a GStrv on every file entry
     * during a crawl (1M files = 1M allocations previously). */
    GStrv                cached_skips;

    /* Serialises access to the static prepared statements in
     * db_upsert_entry / db_delete_entry_by_path. Those statements are
     * declared `static` inside the functions for performance (one
     * prepare per process), but without a mutex two concurrent
     * workers can overwrite each other's bindings and corrupt the
     * FTS index. SQLite itself is FULLMUTEX-serialised but our own
     * statement cache is not. */
    GMutex              stmt_lock;

    /* Crawler state */
    pthread_t          *workers;
    guint               n_workers;
    GMutex              queue_lock;
    GCond               queue_cond;
    GAsyncQueue        *dir_queue;        /* pending directories to scan */
    GHashTable         *inflight_dirs;   /* set of dirs being scanned/queued */
    volatile gint       crawling;        /* atomic flag */
    volatile gint       stop_requested;
    guint64             visited_files;
    guint64             visited_dirs;
    GMutex              stats_lock;

    /* inotify watcher */
    int                 inotify_fd;
    pthread_t           inotify_thread;
    GHashTable         *wd_to_path;      /* int wd -> char* path */
    GHashTable         *path_to_wd;      /* char* path -> int wd */
    GMutex              inotify_lock;
    guint               watch_count;
    volatile gint       inotify_running;

    /* Callbacks (called from worker threads; UI must marshal) */
    IndexProgressCallback  progress_cb;
    gpointer               progress_data;
    IndexStateCallback     state_cb;
    gpointer               state_data;
    IndexChangedCallback   changed_cb;
    gpointer               changed_data;

    IndexState         state;
    char              *state_message;
    GMutex             state_lock;

    /* Atomic counter of running worker threads. The last worker to
     * exit transitions the index state from CRAWLING to WATCHING. */
    volatile gint      workers_running;
};

/* ------------------------------------------------------------------ *
 * Forward declarations
 * ------------------------------------------------------------------ */

static gboolean  db_open(EverythingIndex *idx, GError **error);
static gboolean  db_schema_init(EverythingIndex *idx, GError **error);
static gboolean  db_begin_transaction(sqlite3 *db);
static gboolean  db_commit_transaction(sqlite3 *db);

static void     *crawler_worker(void *arg);
static gboolean  crawl_directory(EverythingIndex *idx, const char *path);
static void      entry_to_attrs(const struct stat *st, char attrs[11]);
static char     *ext_of(const char *name);

static gboolean  start_inotify(EverythingIndex *idx);
static void      stop_inotify(EverythingIndex *idx);
static void     *inotify_worker(void *arg);
static gboolean  inotify_watch_dir(EverythingIndex *idx, const char *path);
static void      inotify_unwatch_dir(EverythingIndex *idx, const char *path);
static void      inotify_handle_event(EverythingIndex *idx,
                                      struct inotify_event *ev);

static void      set_state(EverythingIndex *idx, IndexState s, const char *msg);
static void      fire_changed(EverythingIndex *idx);

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

EverythingIndex *
everything_index_new(EverythingConfig *cfg, GError **error)
{
    g_return_val_if_fail(cfg != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    EverythingIndex *idx = g_new0(EverythingIndex, 1);
    idx->cfg = cfg;
    g_mutex_init(&idx->queue_lock);
    g_mutex_init(&idx->stmt_lock);
    g_cond_init(&idx->queue_cond);
    g_mutex_init(&idx->stats_lock);
    g_mutex_init(&idx->inotify_lock);
    g_mutex_init(&idx->state_lock);

    idx->dir_queue = g_async_queue_new();
    idx->inflight_dirs = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
    idx->wd_to_path = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, g_free);
    idx->path_to_wd = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, NULL);

    idx->db_path = cfg->db_path ? g_strdup(cfg->db_path)
                                : everything_index_default_path();

    /* Cache the skip-list once — avoids 1M+ GStrv allocations during
     * a full-disk crawl. */
    idx->cached_skips = everything_config_skips(cfg);

    if (!db_open(idx, error)) {
        everything_index_free(idx);
        return NULL;
    }
    if (!db_schema_init(idx, error)) {
        everything_index_free(idx);
        return NULL;
    }

    set_state(idx, INDEX_STATE_IDLE, "ready");
    return idx;
}

void
everything_index_free(EverythingIndex *idx)
{
    if (!idx) return;
    everything_index_stop(idx);

    /* The static prepared statements in db_upsert_entry and
     * db_delete_entry_by_path are never explicitly finalized (a
     * deliberate performance choice — preparing them once per process
     * is much cheaper than per-row). That makes sqlite3_close() refuse
     * to close with SQLITE_BUSY. We use sqlite3_close_v2() which is
     * non-blocking, but BEFORE that we force a TRUNCATE checkpoint so
     * every pending WAL frame is written into the main .sqlite file.
     * Without this, a fast subsequent invocation (back-to-back CLI
     * commands in a script) can fail to see the just-written data
     * because the WAL file was never checkpointed. */
    if (idx->db) {
        sqlite3_wal_checkpoint_v2(idx->db, NULL,
                                  SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
        sqlite3_busy_handler(idx->db, NULL, NULL);
        sqlite3_close_v2(idx->db);
        idx->db = NULL;
    }

    if (idx->dir_queue) g_async_queue_unref(idx->dir_queue);
    if (idx->inflight_dirs) g_hash_table_destroy(idx->inflight_dirs);
    if (idx->wd_to_path) g_hash_table_destroy(idx->wd_to_path);
    if (idx->path_to_wd) g_hash_table_destroy(idx->path_to_wd);
    g_mutex_clear(&idx->queue_lock);
    g_mutex_clear(&idx->stmt_lock);
    g_cond_clear(&idx->queue_cond);
    g_mutex_clear(&idx->stats_lock);
    g_mutex_clear(&idx->inotify_lock);
    g_mutex_clear(&idx->state_lock);
    g_free(idx->db_path);
    g_free(idx->state_message);
    g_free(idx->workers);
    g_strfreev(idx->cached_skips);
    g_free(idx);
}

/* ------------------------------------------------------------------ *
 * SQLite schema & helpers
 * ------------------------------------------------------------------ */

static gboolean
db_open(EverythingIndex *idx, GError **error)
{
    /* Ensure the parent directory exists. */
    char *parent = g_path_get_dirname(idx->db_path);
    g_mkdir_with_parents(parent, 0700);
    g_free(parent);

    int rc = sqlite3_open_v2(idx->db_path, &idx->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                             | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        g_set_error(error, 1, rc,
                    "Failed to open SQLite index '%s': %s",
                    idx->db_path, sqlite3_errmsg(idx->db));
        return FALSE;
    }
    /* Performance pragmas — safe with WAL. */
    sqlite3_exec(idx->db, "PRAGMA journal_mode=WAL;",     NULL, NULL, NULL);
    sqlite3_exec(idx->db, "PRAGMA synchronous=NORMAL;",   NULL, NULL, NULL);
    sqlite3_exec(idx->db, "PRAGMA temp_store=MEMORY;",    NULL, NULL, NULL);
    sqlite3_exec(idx->db, "PRAGMA mmap_size=268435456;",  NULL, NULL, NULL);
    sqlite3_exec(idx->db, "PRAGMA cache_size=-65536;",    NULL, NULL, NULL); /* 64 MiB */
    sqlite3_exec(idx->db, "PRAGMA foreign_keys=ON;",      NULL, NULL, NULL);
    sqlite3_busy_timeout(idx->db, 30000);
    return TRUE;
}

static gboolean
db_schema_init(EverythingIndex *idx, GError **error)
{
    const char *sql =
        "CREATE TABLE IF NOT EXISTS schema_meta ("
        "  key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        /* Use plain INTEGER PRIMARY KEY (== rowid) rather than
         * AUTOINCREMENT. AUTOINCREMENT would never reuse IDs even
         * after a DELETE, so the rowid sequence and the FTS5 rowid
         * sequence would diverge after a rebuild — breaking the
         * JOIN files_fts.rowid = files.id. With plain INTEGER PRIMARY
         * KEY, DELETE FROM files resets the rowid sequence. */
        "CREATE TABLE IF NOT EXISTS files ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  path TEXT NOT NULL,"
        "  fullpath TEXT NOT NULL UNIQUE,"
        "  size INTEGER NOT NULL DEFAULT 0,"
        "  is_dir INTEGER NOT NULL DEFAULT 0,"
        "  ext TEXT,"
        "  mtime INTEGER NOT NULL DEFAULT 0,"
        "  ctime INTEGER NOT NULL DEFAULT 0,"
        "  atime INTEGER NOT NULL DEFAULT 0,"
        "  mode INTEGER NOT NULL DEFAULT 0,"
        "  attrs TEXT NOT NULL DEFAULT '');"
        "DROP INDEX IF EXISTS idx_files_id_old;"
        "CREATE INDEX IF NOT EXISTS idx_files_path   ON files(path);"
        "CREATE INDEX IF NOT EXISTS idx_files_ext    ON files(ext);"
        "CREATE INDEX IF NOT EXISTS idx_files_size   ON files(size);"
        "CREATE INDEX IF NOT EXISTS idx_files_mtime  ON files(mtime);"
        "CREATE INDEX IF NOT EXISTS idx_files_is_dir ON files(is_dir);"
        "CREATE VIRTUAL TABLE IF NOT EXISTS files_fts USING fts5("
        "  name, path,"
        "  content='',"
        "  contentless_delete=1,"
        "  tokenize='unicode61 remove_diacritics 1');";
    char *err = NULL;
    int rc = sqlite3_exec(idx->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_set_error(error, 1, rc, "Schema init failed: %s", err ? err : "?");
        sqlite3_free(err);
        return FALSE;
    }
    sqlite3_exec(idx->db,
        "INSERT OR REPLACE INTO schema_meta(key,value) VALUES('version','1');",
        NULL, NULL, NULL);
    return TRUE;
}

static gboolean
db_begin_transaction(sqlite3 *db)
{
    return sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) == SQLITE_OK;
}
static gboolean
db_commit_transaction(sqlite3 *db)
{
    return sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK;
}

/* ------------------------------------------------------------------ *
 * State & callback helpers
 * ------------------------------------------------------------------ */

static void
set_state(EverythingIndex *idx, IndexState s, const char *msg)
{
    g_mutex_lock(&idx->state_lock);
    idx->state = s;
    g_free(idx->state_message);
    idx->state_message = msg ? g_strdup(msg) : NULL;
    g_mutex_unlock(&idx->state_lock);
    if (idx->state_cb)
        idx->state_cb(idx, s, msg, idx->state_data);
}

IndexState
everything_index_state(EverythingIndex *idx)
{
    g_mutex_lock(&idx->state_lock);
    IndexState s = idx->state;
    g_mutex_unlock(&idx->state_lock);
    return s;
}

const char *
everything_index_state_message(EverythingIndex *idx)
{
    g_mutex_lock(&idx->state_lock);
    const char *m = idx->state_message;
    g_mutex_unlock(&idx->state_lock);
    return m;
}

void
everything_index_set_progress_cb(EverythingIndex *idx, IndexProgressCallback cb,
                                 gpointer user_data)
{
    idx->progress_cb = cb; idx->progress_data = user_data;
}
void
everything_index_set_state_cb(EverythingIndex *idx, IndexStateCallback cb,
                              gpointer user_data)
{
    idx->state_cb = cb; idx->state_data = user_data;
}
void
everything_index_set_changed_cb(EverythingIndex *idx, IndexChangedCallback cb,
                                gpointer user_data)
{
    idx->changed_cb = cb; idx->changed_data = user_data;
}

static void
fire_changed(EverythingIndex *idx)
{
    if (idx->changed_cb) idx->changed_cb(idx, idx->changed_data);
}

/* ------------------------------------------------------------------ *
 * Crawler
 * ------------------------------------------------------------------ */

static gboolean
should_skip(EverythingIndex *idx, const char *fullpath)
{
    /* Uses the cached skip-list (idx->cached_skips) instead of calling
     * everything_config_skips() on every entry — that allocated a GStrv
     * per file and was the #1 performance hotspot during crawls. */
    GStrv skips = idx->cached_skips;
    for (guint i = 0; skips && skips[i]; i++) {
        if (g_strcmp0(fullpath, skips[i]) == 0) return TRUE;
    }
    return FALSE;
}

static char *
ext_of(const char *name)
{
    /* Returns a newly-allocated lowercase extension without the dot,
     * or NULL if no extension. Directories get NULL. */
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || dot[1] == '\0') return NULL;
    char *ext = g_ascii_strdown(dot + 1, -1);
    return ext;
}

static void
entry_to_attrs(const struct stat *st, char attrs[11])
{
    /* Reproduce the ls -l style "drwxr-xr-x" string. */
    memset(attrs, '-', 10);
    attrs[10] = '\0';
    attrs[0] = S_ISDIR(st->st_mode)  ? 'd' :
               S_ISLNK(st->st_mode)  ? 'l' :
               S_ISCHR(st->st_mode)  ? 'c' :
               S_ISBLK(st->st_mode)  ? 'b' :
               S_ISFIFO(st->st_mode) ? 'p' :
               S_ISSOCK(st->st_mode) ? 's' : '-';
    attrs[1] = (st->st_mode & S_IRUSR) ? 'r' : '-';
    attrs[2] = (st->st_mode & S_IWUSR) ? 'w' : '-';
    attrs[3] = (st->st_mode & S_ISUID) ? 's' :
               (st->st_mode & S_IXUSR) ? 'x' : '-';
    attrs[4] = (st->st_mode & S_IRGRP) ? 'r' : '-';
    attrs[5] = (st->st_mode & S_IWGRP) ? 'w' : '-';
    attrs[6] = (st->st_mode & S_ISGID) ? 's' :
               (st->st_mode & S_IXGRP) ? 'x' : '-';
    attrs[7] = (st->st_mode & S_IROTH) ? 'r' : '-';
    attrs[8] = (st->st_mode & S_IWOTH) ? 'w' : '-';
    attrs[9] = (st->st_mode & S_ISVTX) ? 't' :
               (st->st_mode & S_IXOTH) ? 'x' : '-';
}

/* Insert (or replace) one entry into the index.  Called only from
 * crawler worker threads and the inotify worker.  Caller must hold
 * an open transaction (handled by the worker loop).
 *
 * This function uses `static` prepared statements for performance
 * (one prepare per process). Because those statements are shared
 * across all worker threads, the entire body is wrapped in
 * idx->stmt_lock to prevent two workers from overwriting each
 * other's bindings. SQLite itself is FULLMUTEX-serialised so the
 * extra lock costs nothing in throughput — SQLite would serialise
 * anyway. */
static gboolean
db_upsert_entry(EverythingIndex *idx, const EntryInfo *e)
{
    sqlite3 *db = idx->db;
    g_mutex_lock(&idx->stmt_lock);

    static sqlite3_stmt *stmt = NULL;
    if (G_UNLIKELY(stmt == NULL)) {
        const char *sql =
            "INSERT INTO files(name, path, fullpath, size, is_dir, ext, "
            "                  mtime, ctime, atime, mode, attrs) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(fullpath) DO UPDATE SET "
            "  name=excluded.name, path=excluded.path, size=excluded.size, "
            "  is_dir=excluded.is_dir, ext=excluded.ext, "
            "  mtime=excluded.mtime, ctime=excluded.ctime, "
            "  atime=excluded.atime, mode=excluded.mode, attrs=excluded.attrs;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            g_mutex_unlock(&idx->stmt_lock);
            return FALSE;
        }
    }
    sqlite3_bind_text(stmt, 1,  e->name,      -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2,  e->path,      -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3,  e->fullpath,  -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4,(sqlite3_int64)e->size);
    sqlite3_bind_int(stmt, 5,  e->is_dir);
    sqlite3_bind_text(stmt, 6,  e->ext ? e->ext : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 7,(sqlite3_int64)e->mtime);
    sqlite3_bind_int64(stmt, 8,(sqlite3_int64)e->ctime);
    sqlite3_bind_int64(stmt, 9,(sqlite3_int64)e->atime);
    sqlite3_bind_int(stmt, 10,(int)e->mode);
    sqlite3_bind_text(stmt, 11, e->attrs, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);
    if (rc != SQLITE_DONE) {
        g_mutex_unlock(&idx->stmt_lock);
        return FALSE;
    }

    /* Resolve the rowid for this fullpath.  We cannot use
     * sqlite3_last_insert_rowid() because ON CONFLICT DO UPDATE does
     * not change it.  A UNIQUE-indexed SELECT on fullpath is O(log n)
     * and effectively free. */
    static sqlite3_stmt *id_stmt = NULL;
    if (G_UNLIKELY(id_stmt == NULL)) {
        if (sqlite3_prepare_v2(db, "SELECT id FROM files WHERE fullpath=?;",
                               -1, &id_stmt, NULL) != SQLITE_OK) {
            g_mutex_unlock(&idx->stmt_lock);
            return TRUE; /* non-fatal — FTS just won't be updated */
        }
    }
    sqlite3_bind_text(id_stmt, 1, e->fullpath, -1, SQLITE_STATIC);
    sqlite3_int64 rowid = 0;
    if (sqlite3_step(id_stmt) == SQLITE_ROW)
        rowid = sqlite3_column_int64(id_stmt, 0);
    sqlite3_reset(id_stmt);
    if (rowid == 0) {
        g_mutex_unlock(&idx->stmt_lock);
        return TRUE;
    }

    /* Insert into FTS contentless table, keyed by the same rowid as
     * files.id so the JOIN in search_compile works.  Replace any
     * existing FTS row for this rowid first so we don't accumulate
     * stale tokens. */
    static sqlite3_stmt *fts_del = NULL;
    static sqlite3_stmt *fts_ins = NULL;
    if (G_UNLIKELY(fts_del == NULL)) {
        sqlite3_prepare_v2(db,
            "INSERT INTO files_fts(files_fts, rowid) VALUES('delete', ?);",
            -1, &fts_del, NULL);
    }
    if (G_UNLIKELY(fts_ins == NULL)) {
        sqlite3_prepare_v2(db,
            "INSERT INTO files_fts(rowid, name, path) VALUES (?,?,?);",
            -1, &fts_ins, NULL);
    }
    if (fts_del) {
        sqlite3_bind_int64(fts_del, 1, rowid);
        sqlite3_step(fts_del);
        sqlite3_reset(fts_del);
    }
    if (fts_ins) {
        sqlite3_bind_int64(fts_ins, 1, rowid);
        sqlite3_bind_text(fts_ins, 2, e->name, -1, SQLITE_STATIC);
        sqlite3_bind_text(fts_ins, 3, e->path, -1, SQLITE_STATIC);
        rc = sqlite3_step(fts_ins);
        sqlite3_reset(fts_ins);
        (void)rc;
    }
    g_mutex_unlock(&idx->stmt_lock);
    return TRUE;
}

static void
db_delete_entry_by_path(EverythingIndex *idx, const char *fullpath)
{
    sqlite3 *db = idx->db;
    g_mutex_lock(&idx->stmt_lock);
    static sqlite3_stmt *del_stmt = NULL;
    static sqlite3_stmt *id_stmt  = NULL;
    static sqlite3_stmt *fts_del  = NULL;
    if (G_UNLIKELY(del_stmt == NULL)) {
        if (sqlite3_prepare_v2(db,
            "DELETE FROM files WHERE fullpath=?;", -1, &del_stmt, NULL) != SQLITE_OK) {
            g_mutex_unlock(&idx->stmt_lock);
            return;
        }
        sqlite3_prepare_v2(db, "SELECT id FROM files WHERE fullpath=?;",
                           -1, &id_stmt, NULL);
        sqlite3_prepare_v2(db,
            "INSERT INTO files_fts(files_fts, rowid) VALUES('delete', ?);",
            -1, &fts_del, NULL);
    }
    /* Get the rowid first so we can delete the FTS row too. */
    sqlite3_int64 rowid = 0;
    if (id_stmt) {
        sqlite3_bind_text(id_stmt, 1, fullpath, -1, SQLITE_STATIC);
        if (sqlite3_step(id_stmt) == SQLITE_ROW)
            rowid = sqlite3_column_int64(id_stmt, 0);
        sqlite3_reset(id_stmt);
    }
    if (del_stmt) {
        sqlite3_bind_text(del_stmt, 1, fullpath, -1, SQLITE_STATIC);
        sqlite3_step(del_stmt);
        sqlite3_reset(del_stmt);
    }
    if (fts_del && rowid) {
        sqlite3_bind_int64(fts_del, 1, rowid);
        sqlite3_step(fts_del);
        sqlite3_reset(fts_del);
    }
    g_mutex_unlock(&idx->stmt_lock);
}

/* Read all entries in a directory and insert them into the DB. Returns
 * TRUE if the directory was successfully opened. Subdirectories are
 * pushed onto the async queue for other workers to pick up. */
static gboolean
crawl_directory(EverythingIndex *idx, const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        g_debug("opendir(%s) failed: %s", path, g_strerror(errno));
        return FALSE;
    }
    sqlite3 *db = idx->db;
    gboolean tx_started = db_begin_transaction(db);

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (g_atomic_int_get(&idx->stop_requested)) break;
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' ||
             (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        if (!idx->cfg->index_hidden && de->d_name[0] == '.') continue;

        char *fullpath = g_build_filename(path, de->d_name, NULL);

        if (should_skip(idx, fullpath)) {
            g_free(fullpath);
            continue;
        }

        struct stat st;
        int r = idx->cfg->follow_symlinks
                  ? stat(fullpath, &st)
                  : lstat(fullpath, &st);
        if (r != 0) {
            g_free(fullpath);
            continue;
        }

        EntryInfo e = {0};
        e.fullpath = fullpath;
        e.name     = (char *)de->d_name;
        e.path     = (char *)path;
        e.size     = (uint64_t)st.st_size;
        e.is_dir   = S_ISDIR(st.st_mode) ? 1 : 0;
        e.ext      = e.is_dir ? NULL : ext_of(de->d_name);
        e.mtime    = st.st_mtime;
        e.ctime    = st.st_ctime;
        e.atime    = st.st_atime;
        e.mode     = st.st_mode;
        entry_to_attrs(&st, e.attrs);

        db_upsert_entry(idx, &e);
        g_free(e.ext);

        g_mutex_lock(&idx->stats_lock);
        if (e.is_dir) idx->visited_dirs++;  else idx->visited_files++;
        guint64 visited = idx->visited_files + idx->visited_dirs;
        g_mutex_unlock(&idx->stats_lock);

        if (idx->progress_cb && (visited & 0x3FF) == 0)
            idx->progress_cb(idx, visited, 0, idx->progress_data);

        if (e.is_dir) {
            /* Push onto queue for further traversal. Avoid re-queuing
             * if already inflight. */
            g_mutex_lock(&idx->queue_lock);
            if (!g_hash_table_contains(idx->inflight_dirs, fullpath)) {
                char *copy = g_strdup(fullpath);
                g_hash_table_add(idx->inflight_dirs, copy);
                g_async_queue_push(idx->dir_queue, copy);
                g_cond_signal(&idx->queue_cond);
            }
            g_mutex_unlock(&idx->queue_lock);
        }

        g_free(fullpath);
    }
    closedir(dir);

    if (tx_started) db_commit_transaction(db);

    /* Try to install an inotify watch on this dir for live updates. */
    if (idx->inotify_running)
        inotify_watch_dir(idx, path);

    return TRUE;
}

static void *
crawler_worker(void *arg)
{
    EverythingIndex *idx = (EverythingIndex *)arg;

    while (g_atomic_int_get(&idx->crawling) &&
           !g_atomic_int_get(&idx->stop_requested)) {
        /* Try to pop a directory off the queue. The string is owned by
         * idx->inflight_dirs (which has g_free as key-destroy), so we
         * must NOT free it here — g_hash_table_remove will. */
        g_mutex_lock(&idx->queue_lock);
        char *path = (char *)g_async_queue_try_pop(idx->dir_queue);
        if (!path) {
            gint64 end = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
            g_cond_wait_until(&idx->queue_cond, &idx->queue_lock, end);
            g_mutex_unlock(&idx->queue_lock);
            /* If the queue is empty AND no other worker is still busy,
             * we are done. */
            if (g_async_queue_length(idx->dir_queue) <= 0 &&
                g_hash_table_size(idx->inflight_dirs) == 0) {
                /* double-check after a tiny sleep to drain racers */
                g_usleep(50000);
                if (g_async_queue_length(idx->dir_queue) <= 0 &&
                    g_hash_table_size(idx->inflight_dirs) == 0) {
                    break;
                }
            }
            continue;
        }
        g_mutex_unlock(&idx->queue_lock);

        crawl_directory(idx, path);

        g_mutex_lock(&idx->queue_lock);
        /* This removes the key from the hash and frees it via g_free. */
        g_hash_table_remove(idx->inflight_dirs, path);
        g_mutex_unlock(&idx->queue_lock);
        /* Do NOT g_free(path) — already freed above. */
    }

    /* Last worker out transitions state and signals any waiting main
     * thread (CLI tests do a busy-wait on state). */
    if (g_atomic_int_dec_and_test(&idx->workers_running)) {
        g_atomic_int_set(&idx->crawling, 0);
        if (g_atomic_int_get(&idx->stop_requested))
            set_state(idx, INDEX_STATE_IDLE, "stopped");
        else
            set_state(idx, INDEX_STATE_WATCHING, "watching for changes");
    }

    return NULL;
}

gboolean
everything_index_rebuild(EverythingIndex *idx, GError **error)
{
    g_return_val_if_fail(idx != NULL, FALSE);

    if (g_atomic_int_get(&idx->crawling)) {
        g_set_error_literal(error, 1, 0, "Indexer already running");
        return FALSE;
    }

    /* Wipe existing data. */
    if (!db_begin_transaction(idx->db)) {
        g_set_error_literal(error, 1, 0, "BEGIN failed");
        return FALSE;
    }
    sqlite3_exec(idx->db, "DELETE FROM files;", NULL, NULL, NULL);
    /* For FTS5 contentless tables we cannot TRUNCATE; use the
     * special delete-from-contentless command via 'DELETE FROM files_fts'
     * which is supported since SQLite 3.42. */
    sqlite3_exec(idx->db,
        "INSERT INTO files_fts(files_fts) VALUES('delete-all');",
        NULL, NULL, NULL);
    db_commit_transaction(idx->db);

    g_mutex_lock(&idx->stats_lock);
    idx->visited_files = 0;
    idx->visited_dirs  = 0;
    g_mutex_unlock(&idx->stats_lock);

    /* Reset queues. The strings in the dir_queue are owned by
     * inflight_dirs, so we drain the queue without freeing; the
     * g_hash_table_remove_all below will free them. */
    g_async_queue_lock(idx->dir_queue);
    while (g_async_queue_try_pop_unlocked(idx->dir_queue) != NULL)
        ;
    g_async_queue_unlock(idx->dir_queue);
    g_hash_table_remove_all(idx->inflight_dirs);

    /* Determine thread count. */
    guint nt = idx->cfg->worker_threads;
    if (nt == 0) {
        nt = g_get_num_processors();
        if (nt < 1) nt = 1;
        if (nt > 8) nt = 8; /* cap — SQLite is the bottleneck */
    }
    idx->n_workers = nt;
    idx->workers = g_new0(pthread_t, nt);

    /* Seed the queue with configured roots. */
    GStrv roots = everything_config_roots(idx->cfg);
    for (guint i = 0; roots && roots[i]; i++) {
        char *r = g_strstrip(g_strdup(roots[i]));
        if (*r == '\0') { g_free(r); continue; }
        /* Normalize trailing slash. */
        if (g_str_has_suffix(r, "/") && strlen(r) > 1) {
            r[strlen(r) - 1] = '\0';
        }
        if (should_skip(idx, r)) { g_free(r); continue; }
        char *copy = g_strdup(r);
        g_hash_table_add(idx->inflight_dirs, copy);
        g_async_queue_push(idx->dir_queue, copy);
        g_free(r);
    }
    g_strfreev(roots);

    g_atomic_int_set(&idx->crawling, 1);
    g_atomic_int_set(&idx->stop_requested, 0);
    g_atomic_int_set(&idx->workers_running, nt);
    set_state(idx, INDEX_STATE_CRAWLING, "crawling filesystem");

    /* Start inotify watcher early so dirs get watched as they are scanned. */
    start_inotify(idx);

    /* Spawn worker threads. */
    for (guint i = 0; i < nt; i++)
        pthread_create(&idx->workers[i], NULL, crawler_worker, idx);

    return TRUE;
}

void
everything_index_stop(EverythingIndex *idx)
{
    if (!idx) return;
    g_atomic_int_set(&idx->stop_requested, 1);
    if (g_atomic_int_get(&idx->crawling)) {
        g_cond_broadcast(&idx->queue_cond);
        for (guint i = 0; i < idx->n_workers; i++)
            if (idx->workers[i])
                pthread_join(idx->workers[i], NULL);
        g_atomic_int_set(&idx->crawling, 0);
        g_free(idx->workers);
        idx->workers = NULL;
        idx->n_workers = 0;
    }
    stop_inotify(idx);
    set_state(idx, INDEX_STATE_IDLE, "stopped");
}

gboolean
everything_index_ensure(EverythingIndex *idx, GError **error)
{
    g_return_val_if_fail(idx != NULL, FALSE);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(idx->db,
            "SELECT COUNT(*) FROM files;", -1, &stmt, NULL) != SQLITE_OK) {
        g_set_error(error, 1, 0, "count query failed: %s",
                    sqlite3_errmsg(idx->db));
        return FALSE;
    }
    guint64 n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    if (n > 0) {
        /* Existing index — start inotify, refresh lazily. */
        start_inotify(idx);
        set_state(idx, INDEX_STATE_WATCHING, "watching for changes");
        return TRUE;
    }
    return everything_index_rebuild(idx, error);
}

guint64
everything_index_count(EverythingIndex *idx)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(idx->db, "SELECT COUNT(*) FROM files;",
            -1, &stmt, NULL) != SQLITE_OK) return 0;
    guint64 n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

sqlite3 *
everything_index_db(EverythingIndex *idx)
{
    return idx->db;
}

const char *
everything_index_db_path(EverythingIndex *idx)
{
    return idx->db_path;
}

guint64
everything_index_visited_files(EverythingIndex *idx)
{
    g_mutex_lock(&idx->stats_lock);
    guint64 n = idx->visited_files;
    g_mutex_unlock(&idx->stats_lock);
    return n;
}
guint64
everything_index_visited_dirs(EverythingIndex *idx)
{
    g_mutex_lock(&idx->stats_lock);
    guint64 n = idx->visited_dirs;
    g_mutex_unlock(&idx->stats_lock);
    return n;
}
guint
everything_index_watch_count(EverythingIndex *idx)
{
    g_mutex_lock(&idx->inotify_lock);
    guint n = idx->watch_count;
    g_mutex_unlock(&idx->inotify_lock);
    return n;
}

/* ------------------------------------------------------------------ *
 * inotify watcher
 * ------------------------------------------------------------------ */

static gboolean
start_inotify(EverythingIndex *idx)
{
    if (g_atomic_int_get(&idx->inotify_running)) return TRUE;
    idx->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (idx->inotify_fd < 0) {
        g_warning("inotify_init1 failed: %s", g_strerror(errno));
        return FALSE;
    }
    g_atomic_int_set(&idx->inotify_running, 1);
    pthread_create(&idx->inotify_thread, NULL, inotify_worker, idx);
    return TRUE;
}

static void
stop_inotify(EverythingIndex *idx)
{
    if (!g_atomic_int_get(&idx->inotify_running)) return;
    g_atomic_int_set(&idx->inotify_running, 0);
    if (idx->inotify_fd >= 0) close(idx->inotify_fd);
    idx->inotify_fd = -1;
    pthread_join(idx->inotify_thread, NULL);

    g_mutex_lock(&idx->inotify_lock);
    g_hash_table_remove_all(idx->wd_to_path);
    g_hash_table_remove_all(idx->path_to_wd);
    idx->watch_count = 0;
    g_mutex_unlock(&idx->inotify_lock);
}

static gboolean
inotify_watch_dir(EverythingIndex *idx, const char *path)
{
    g_mutex_lock(&idx->inotify_lock);
    if (g_hash_table_contains(idx->path_to_wd, path)) {
        g_mutex_unlock(&idx->inotify_lock);
        return TRUE;
    }
    g_mutex_unlock(&idx->inotify_lock);

    uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM
                  | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;
    int wd = inotify_add_watch(idx->inotify_fd, path, mask);
    if (wd < 0) {
        if (errno == ENOSPC) {
            /* Only log this once per run. */
            static gint warned = 0;
            if (g_atomic_int_compare_and_exchange(&warned, 0, 1))
                g_warning("inotify watch limit reached. "
                          "Increase fs.inotify.max_user_watches (sysctl).");
        }
        return FALSE;
    }
    g_mutex_lock(&idx->inotify_lock);
    g_hash_table_insert(idx->wd_to_path,  GINT_TO_POINTER(wd), g_strdup(path));
    g_hash_table_insert(idx->path_to_wd,  g_strdup(path),     GINT_TO_POINTER(wd));
    idx->watch_count++;
    if (idx->watch_count == INOTIFY_WARN_LIMIT + 1) {
        g_warning("inotify watches exceed %d. Consider raising "
                  "fs.inotify.max_user_watches.", INOTIFY_WARN_LIMIT);
    }
    g_mutex_unlock(&idx->inotify_lock);
    return TRUE;
}

static void
inotify_unwatch_dir(EverythingIndex *idx, const char *path)
{
    g_mutex_lock(&idx->inotify_lock);
    gpointer v = g_hash_table_lookup(idx->path_to_wd, path);
    if (v) {
        int wd = GPOINTER_TO_INT(v);
        inotify_rm_watch(idx->inotify_fd, wd);
        g_hash_table_remove(idx->wd_to_path, GINT_TO_POINTER(wd));
        g_hash_table_remove(idx->path_to_wd,  path);
        idx->watch_count--;
    }
    g_mutex_unlock(&idx->inotify_lock);
}

static void *
inotify_worker(void *arg)
{
    EverythingIndex *idx = (EverythingIndex *)arg;
    char buf[65536] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (g_atomic_int_get(&idx->inotify_running)) {
        ssize_t n = read(idx->inotify_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                g_usleep(200000); /* 200ms poll */
                continue;
            }
            if (errno == EINTR) continue;
            break;
        }
        for (char *p = buf; p < buf + n; ) {
            struct inotify_event *ev = (struct inotify_event *)p;
            inotify_handle_event(idx, ev);
            p += sizeof(struct inotify_event) + ev->len;
        }
        fire_changed(idx);
    }
    return NULL;
}

static void
inotify_handle_event(EverythingIndex *idx, struct inotify_event *ev)
{
    /* IN_Q_OVERFLOW: the kernel's inotify event queue overflowed and
     * events were dropped. The index is now potentially stale. Log a
     * warning and transition to ERROR state so the UI can prompt a
     * rebuild — silent data loss here would be a subtle, hard-to-diagnose
     * bug. */
    if (ev->wd == -1 && (ev->mask & IN_Q_OVERFLOW)) {
        static gint overflow_warned = 0;
        if (g_atomic_int_compare_and_exchange(&overflow_warned, 0, 1)) {
            g_warning("inotify kernel queue overflowed — index may be stale. "
                      "A rebuild is recommended (Tools → Rebuild index).");
        }
        set_state(idx, INDEX_STATE_ERROR,
                  "inotify queue overflowed — rebuild needed");
        return;
    }

    g_mutex_lock(&idx->inotify_lock);
    const char *dir = (const char *)g_hash_table_lookup(idx->wd_to_path,
                                GINT_TO_POINTER(ev->wd));
    char *dir_copy = dir ? g_strdup(dir) : NULL;
    g_mutex_unlock(&idx->inotify_lock);
    if (!dir_copy) return;

    const char *name = ev->len > 0 ? ev->name : "";
    char *fullpath = g_build_filename(dir_copy, name, (char *)NULL);

    if (ev->mask & (IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_MOVED_FROM)) {
        db_delete_entry_by_path(idx, fullpath);
        if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF))
            inotify_unwatch_dir(idx, dir_copy);
    }
    if (ev->mask & (IN_CREATE | IN_MOVED_TO | IN_MODIFY)) {
        struct stat st;
        if ((idx->cfg->follow_symlinks ? stat : lstat)(fullpath, &st) == 0) {
            EntryInfo e = {0};
            e.fullpath = fullpath;
            e.name     = (char *)name;
            e.path     = dir_copy;
            e.size     = (uint64_t)st.st_size;
            e.is_dir   = S_ISDIR(st.st_mode) ? 1 : 0;
            e.ext      = e.is_dir ? NULL : ext_of(name);
            e.mtime    = st.st_mtime;
            e.ctime    = st.st_ctime;
            e.atime    = st.st_atime;
            e.mode     = st.st_mode;
            entry_to_attrs(&st, e.attrs);
            gboolean tx = db_begin_transaction(idx->db);
            db_upsert_entry(idx, &e);
            if (tx) db_commit_transaction(idx->db);
            g_free(e.ext);
            if (e.is_dir && (ev->mask & (IN_CREATE | IN_MOVED_TO)))
                inotify_watch_dir(idx, fullpath);
        }
    }
    g_free(fullpath);
    g_free(dir_copy);
}
