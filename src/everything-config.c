/*
 * Everything-1.0.0 — INI configuration store
 *
 * SPDX-License-Identifier: MIT
 */
#include "everything-config.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFG_DIRNAME  "everything"
#define CFG_FILENAME "Everything.ini"
#define DB_DIRNAME   "everything"
#define DB_FILENAME  "index.sqlite"

EverythingConfig *
everything_config_new(void)
{
    EverythingConfig *cfg = g_new0(EverythingConfig, 1);
    cfg->index_roots     = g_strdup(DEFAULT_INDEX_ROOTS);
    cfg->skip_dirs       = g_strdup(SKIP_DIRS_EXACT);
    cfg->worker_threads  = DEFAULT_WORKER_THREADS;
    cfg->index_hidden    = 1;
    cfg->index_atime     = 0;
    cfg->follow_symlinks = 0;
    cfg->case_sensitive  = 0;
    cfg->match_whole_word= 0;
    cfg->match_path      = 0;
    cfg->enable_regex    = 0;
    cfg->max_results     = 100000;
    cfg->db_path         = NULL; /* computed lazily */
    return cfg;
}

void
everything_config_free(EverythingConfig *cfg)
{
    if (!cfg) return;
    g_free(cfg->index_roots);
    g_free(cfg->skip_dirs);
    g_free(cfg->db_path);
    g_free(cfg);
}

char *
everything_config_default_path(void)
{
    const char *xdg_cfg = g_getenv("XDG_CONFIG_HOME");
    char *dir;
    if (xdg_cfg && *xdg_cfg)
        dir = g_build_filename(xdg_cfg, CFG_DIRNAME, NULL);
    else
        dir = g_build_filename(g_get_home_dir(), ".config", CFG_DIRNAME, NULL);
    char *path = g_build_filename(dir, CFG_FILENAME, NULL);
    g_free(dir);
    return path;
}

char *
everything_index_default_path(void)
{
    const char *xdg_data = g_getenv("XDG_DATA_HOME");
    char *dir;
    if (xdg_data && *xdg_data)
        dir = g_build_filename(xdg_data, DB_DIRNAME, NULL);
    else
        dir = g_build_filename(g_get_home_dir(), ".local", "share", DB_DIRNAME, NULL);
    /* Make sure the directory exists so SQLite can write the file. */
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, DB_FILENAME, NULL);
    g_free(dir);
    return path;
}

/* Parse a comma-separated list, trimming whitespace. Returns a newly
 * allocated GStrv (NULL-terminated string array). Caller frees with
 * g_strfreev. Empty items are dropped. */
static GStrv
parse_csv(const char *s)
{
    if (!s || !*s) return g_new0(char *, 1);
    gchar **raw = g_strsplit(s, ",", -1);
    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; raw[i]; i++) {
        char *t = g_strstrip(g_strdup(raw[i]));
        if (t && *t)
            g_ptr_array_add(out, t);
        else
            g_free(t);
    }
    g_ptr_array_add(out, NULL);
    gchar **result = (gchar **)g_ptr_array_free(out, FALSE);
    g_strfreev(raw);
    return result;
}

gboolean
everything_config_load(EverythingConfig *cfg, GError **error)
{
    g_return_val_if_fail(cfg != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    char *path = everything_config_default_path();
    GKeyFile *kf = g_key_file_new();
    gboolean loaded = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, error);
    if (!loaded) {
        /* Missing file is not fatal — use defaults. */
        if (error && (*error && g_error_matches(*error, G_FILE_ERROR, G_FILE_ERROR_NOENT))) {
            g_clear_error(error);
        }
        g_key_file_free(kf);
        g_free(path);
        return TRUE;
    }
    g_free(path);

    const char *section = "Everything";
    gchar *roots   = g_key_file_get_string(kf, section, "index_roots",    NULL);
    gchar *skip    = g_key_file_get_string(kf, section, "skip_dirs",      NULL);
    gint  wthreads = g_key_file_get_integer(kf, section, "worker_threads",NULL);
    gint  hidden   = g_key_file_get_integer(kf, section, "index_hidden",  NULL);
    gint  atime    = g_key_file_get_integer(kf, section, "index_atime",   NULL);
    gint  sym      = g_key_file_get_integer(kf, section, "follow_symlinks",NULL);
    gint  cs       = g_key_file_get_integer(kf, section, "case_sensitive",NULL);
    gint  ww       = g_key_file_get_integer(kf, section, "match_whole_word",NULL);
    gint  mp       = g_key_file_get_integer(kf, section, "match_path",    NULL);
    gint  re       = g_key_file_get_integer(kf, section, "enable_regex",  NULL);
    gint  maxr     = g_key_file_get_integer(kf, section, "max_results",    NULL);
    gchar *dbp     = g_key_file_get_string(kf, section, "db_path",        NULL);

    if (roots)            { g_free(cfg->index_roots);     cfg->index_roots     = roots; }
    if (skip)             { g_free(cfg->skip_dirs);       cfg->skip_dirs       = skip; }
    if (wthreads)         { cfg->worker_threads  = wthreads; }
    if (hidden)           { cfg->index_hidden    = hidden; }
    if (atime)            { cfg->index_atime     = atime; }
    if (sym)              { cfg->follow_symlinks = sym; }
    if (cs)               { cfg->case_sensitive  = cs; }
    if (ww)               { cfg->match_whole_word= ww; }
    if (mp)               { cfg->match_path      = mp; }
    if (re)               { cfg->enable_regex    = re; }
    if (maxr)             { cfg->max_results     = maxr; }
    if (dbp)              { g_free(cfg->db_path); cfg->db_path = dbp; }

    g_key_file_free(kf);
    return TRUE;
}

gboolean
everything_config_save(EverythingConfig *cfg, GError **error)
{
    g_return_val_if_fail(cfg != NULL, FALSE);

    char *path = everything_config_default_path();
    char *dir  = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    GKeyFile *kf = g_key_file_new();
    const char *section = "Everything";
    g_key_file_set_comment(kf, section, NULL,
        " Everything-1.0.0 native Linux port configuration file. "
        "Generated automatically — edit at your own risk while the app is closed.", NULL);
    g_key_file_set_string(kf, section, "index_roots",      cfg->index_roots);
    g_key_file_set_string(kf, section, "skip_dirs",        cfg->skip_dirs);
    g_key_file_set_integer(kf, section, "worker_threads",  cfg->worker_threads);
    g_key_file_set_integer(kf, section, "index_hidden",    cfg->index_hidden);
    g_key_file_set_integer(kf, section, "index_atime",     cfg->index_atime);
    g_key_file_set_integer(kf, section, "follow_symlinks", cfg->follow_symlinks);
    g_key_file_set_integer(kf, section, "case_sensitive",  cfg->case_sensitive);
    g_key_file_set_integer(kf, section, "match_whole_word",cfg->match_whole_word);
    g_key_file_set_integer(kf, section, "match_path",      cfg->match_path);
    g_key_file_set_integer(kf, section, "enable_regex",    cfg->enable_regex);
    g_key_file_set_integer(kf, section, "max_results",     cfg->max_results);
    if (cfg->db_path)
        g_key_file_set_string(kf, section, "db_path",      cfg->db_path);

    gboolean ok = g_key_file_save_to_file(kf, path, error);
    g_key_file_free(kf);
    g_free(path);
    return ok;
}

/* Convenience used by tests/UI: split cfg->index_roots into a cleaned
 * GStrv. Caller frees with g_strfreev. */
GStrv
everything_config_roots(EverythingConfig *cfg)
{
    g_return_val_if_fail(cfg != NULL, NULL);
    return parse_csv(cfg->index_roots);
}

GStrv
everything_config_skips(EverythingConfig *cfg)
{
    g_return_val_if_fail(cfg != NULL, NULL);
    return parse_csv(cfg->skip_dirs);
}
