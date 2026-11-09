/*
 * Everything-1.0.0 — INI configuration store
 *
 * Reads/writes an INI file compatible with the original voidtools Everything
 * format: simple [section]/key=value pairs, comment lines start with ';'.
 * Stored in the user's config directory (XDG_CONFIG_HOME).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef EVERYTHING_CONFIG_H
#define EVERYTHING_CONFIG_H

#include <glib.h>
#include "everything-types.h"

typedef struct _EverythingConfig EverythingConfig;

struct _EverythingConfig {
    char    *index_roots;        /* comma-separated list of roots */
    char    *skip_dirs;          /* comma-separated list of dirs to skip */
    int      worker_threads;     /* 0 == auto */
    int      index_hidden;       /* index dotfiles? default 1 */
    int      index_atime;        /* store atime? default 0 (cheap) */
    int      follow_symlinks;    /* default 0 */
    int      case_sensitive;     /* default 0 (search is case-insensitive) */
    int      match_whole_word;   /* default 0 */
    int      match_path;         /* default 0 — search in path too */
    int      enable_regex;       /* default 0 — PCRE-style */
    int      max_results;        /* default 100000 */
    char    *db_path;            /* override index sqlite path */
};

EverythingConfig *everything_config_new(void);
void              everything_config_free(EverythingConfig *cfg);

/* Load from $XDG_CONFIG_HOME/everything/Everything.ini, or defaults if missing. */
gboolean          everything_config_load(EverythingConfig *cfg, GError **error);

/* Save back to the same file. */
gboolean          everything_config_save(EverythingConfig *cfg, GError **error);

/* Default config path under XDG_CONFIG_HOME, never NULL. Caller frees. */
char             *everything_config_default_path(void);

/* Default SQLite index path under XDG_DATA_HOME, never NULL. Caller frees. */
char             *everything_index_default_path(void);

/* Convenience: split cfg->index_roots into a cleaned GStrv. Caller frees
 * with g_strfreev. */
GStrv             everything_config_roots(EverythingConfig *cfg);
GStrv             everything_config_skips(EverythingConfig *cfg);

#endif /* EVERYTHING_CONFIG_H */
