/*
 * everything-cli — standalone command-line interface to the indexer and
 * search engine. Used both as an integration test (so we can verify
 * the backend works without a GUI) and as a useful power-user tool.
 *
 * Usage:
 *   everything-cli index                    rebuild the index
 *   everything-cli status                   print index stats
 *   everything-cli search "QUERY"           run a search
 *   everything-cli search -j "QUERY"        output results as JSON
 *
 * SPDX-License-Identifier: MIT
 */
#include "everything-config.h"
#include "everything-index.h"
#include "everything-search.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static gboolean json_output = FALSE;
static gboolean verbose     = FALSE;
static gint    max_results  = 1000;
static const gchar *sort_by = "name";
static const gchar *sort_dir= "asc";
static gchar  *roots_arg    = NULL;

static GOptionEntry entries[] = {
    { "json",     'j', 0, G_OPTION_ARG_NONE,   &json_output,  "Output results as JSON",            NULL },
    { "verbose",  'v', 0, G_OPTION_ARG_NONE,   &verbose,      "Verbose logging",                   NULL },
    { "limit",    'n', 0, G_OPTION_ARG_INT,    &max_results,  "Maximum results",                   "N" },
    { "sort",     's', 0, G_OPTION_ARG_STRING, &sort_by,      "Sort column (name/path/size/type/extension/mtime/ctime/atime/attrs)", "COL" },
    { "dir",      'd', 0, G_OPTION_ARG_STRING, &sort_dir,     "Sort direction (asc|desc)",         "DIR" },
    { "roots",    'r', 0, G_OPTION_ARG_STRING, &roots_arg,    "Override index roots (comma-sep)",  "ROOTS" },
    { NULL }
};

static SortColumn
parse_sort_col(const char *s)
{
    if (g_ascii_strcasecmp(s, "name") == 0)       return SORT_BY_NAME;
    if (g_ascii_strcasecmp(s, "path") == 0)       return SORT_BY_PATH;
    if (g_ascii_strcasecmp(s, "size") == 0)       return SORT_BY_SIZE;
    if (g_ascii_strcasecmp(s, "type") == 0)       return SORT_BY_TYPE;
    if (g_ascii_strcasecmp(s, "ext")  == 0 ||
        g_ascii_strcasecmp(s, "extension") == 0)  return SORT_BY_EXTENSION;
    if (g_ascii_strcasecmp(s, "mtime") == 0 ||
        g_ascii_strcasecmp(s, "dm")     == 0)     return SORT_BY_MTIME;
    if (g_ascii_strcasecmp(s, "ctime") == 0)      return SORT_BY_CTIME;
    if (g_ascii_strcasecmp(s, "atime") == 0)      return SORT_BY_ATIME;
    if (g_ascii_strcasecmp(s, "attrs") == 0)      return SORT_BY_ATTRIBUTES;
    return SORT_BY_NAME;
}

static void
on_progress(EverythingIndex *idx, guint64 visited, guint64 total, gpointer u)
{
    (void)idx; (void)total; (void)u;
    if (verbose && (visited & 0x3FFF) == 0)
        fprintf(stderr, "\r\033[K  crawled %llu entries...",
                (unsigned long long)visited);
}

static void
on_state(EverythingIndex *idx, IndexState s, const char *msg, gpointer u)
{
    (void)idx; (void)u;
    if (verbose)
        fprintf(stderr, "[state] %d: %s\n", (int)s, msg ? msg : "");
}

static int
do_index(EverythingConfig *cfg)
{
    GError *err = NULL;
    EverythingIndex *idx = everything_index_new(cfg, &err);
    if (!idx) {
        fprintf(stderr, "Failed to open index: %s\n", err ? err->message : "?");
        return 2;
    }
    everything_index_set_progress_cb(idx, on_progress, NULL);
    everything_index_set_state_cb(idx, on_state, NULL);

    if (!everything_index_rebuild(idx, &err)) {
        fprintf(stderr, "Rebuild failed: %s\n", err ? err->message : "?");
        everything_index_free(idx);
        return 3;
    }

    /* Wait for crawler to finish. We do this by polling state. */
    while (everything_index_state(idx) == INDEX_STATE_CRAWLING)
        g_usleep(100000);

    if (verbose) fprintf(stderr, "\n");
    fprintf(stderr, "Indexed %llu entries.\n",
            (unsigned long long)everything_index_count(idx));
    everything_index_stop(idx);
    everything_index_free(idx);
    return 0;
}

static int
do_status(EverythingConfig *cfg)
{
    GError *err = NULL;
    EverythingIndex *idx = everything_index_new(cfg, &err);
    if (!idx) {
        fprintf(stderr, "Failed to open index: %s\n", err ? err->message : "?");
        return 2;
    }
    printf("Index path:    %s\n", everything_index_db_path(idx));
    printf("State:         %d (%s)\n",
           (int)everything_index_state(idx),
           everything_index_state_message(idx) ?: "");
    printf("Indexed items: %llu\n",
           (unsigned long long)everything_index_count(idx));
    everything_index_free(idx);
    return 0;
}

static void
print_row_human(const EverythingResultRow *r)
{
    char *size = format_size(r->size);
    char *mt   = format_time(r->mtime);
    printf("%-40s  %-12s  %10s  %s  %s\n",
           r->name, r->type_label, size, mt, r->path);
    g_free(size); g_free(mt);
}

/* JSON-escape a string into stdout. */
static void
json_escape(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static void
print_row_json(const EverythingResultRow *r, gboolean first)
{
    if (!first) printf(",\n");
    char *mt = format_time(r->mtime);
    printf("  {\"name\":");        json_escape(r->name);
    printf(",\"path\":");          json_escape(r->path);
    printf(",\"fullpath\":");      json_escape(r->fullpath);
    printf(",\"size\":%llu",       (unsigned long long)r->size);
    printf(",\"is_dir\":%s",       r->is_dir ? "true" : "false");
    printf(",\"ext\":");           json_escape(r->ext);
    printf(",\"type\":");          json_escape(r->type_label);
    printf(",\"attrs\":");         json_escape(r->attrs);
    printf(",\"mtime\":\"%s\"}",   mt);
    g_free(mt);
}

static int
do_search(EverythingConfig *cfg, const char *query)
{
    GError *err = NULL;
    EverythingIndex *idx = everything_index_new(cfg, &err);
    if (!idx) {
        fprintf(stderr, "Failed to open index: %s\n", err ? err->message : "?");
        return 2;
    }

    /* If the index is empty, build it now. */
    if (everything_index_count(idx) == 0) {
        fprintf(stderr, "Index empty, building...\n");
        everything_index_set_progress_cb(idx, on_progress, NULL);
        everything_index_rebuild(idx, &err);
        while (everything_index_state(idx) == INDEX_STATE_CRAWLING)
            g_usleep(100000);
        if (verbose) fprintf(stderr, "\n");
    }

    SearchRequest *req = search_request_new(query);
    req->sort_col = parse_sort_col(sort_by);
    req->sort_dir = (g_ascii_strcasecmp(sort_dir, "desc") == 0) ? SORT_DESC : SORT_ASC;
    req->limit    = max_results;

    guint64 total = 0;
    GArray *rows = search_execute(idx, req, &total, &err);
    if (!rows) {
        fprintf(stderr, "Search failed: %s\n", err ? err->message : "?");
        search_request_free(req);
        everything_index_free(idx);
        return 3;
    }

    if (json_output) {
        printf("[\n");
        for (guint i = 0; i < rows->len; i++) {
            EverythingResultRow *r = g_array_index(rows, EverythingResultRow *, i);
            print_row_json(r, i == 0);
        }
        printf("\n]\n");
    } else {
        fprintf(stderr, "Results (%llu):\n", (unsigned long long)total);
        for (guint i = 0; i < rows->len; i++) {
            EverythingResultRow *r = g_array_index(rows, EverythingResultRow *, i);
            print_row_human(r);
        }
    }

    for (guint i = 0; i < rows->len; i++) {
        EverythingResultRow *r = g_array_index(rows, EverythingResultRow *, i);
        g_object_unref(r);
    }
    g_array_free(rows, TRUE);
    search_request_free(req);
    everything_index_free(idx);
    return 0;
}

int
main(int argc, char **argv)
{
    GError *err = NULL;
    GOptionContext *ctx = g_option_context_new("<command> [args]");
    g_option_context_set_summary(ctx,
        "Everything-1.0.0 — native Linux file indexer/searcher.\n"
        "Commands: index | status | search \"QUERY\"");
    g_option_context_add_main_entries(ctx, entries, NULL);
    /* g_option_context_parse_strv() takes ownership of argv and frees
     * it via g_strfreev, so we must hand it a glib-allocated copy and
     * not the C runtime's argv. */
    gchar **argv_copy = g_strdupv((gchar **)argv);
    if (!g_option_context_parse_strv(ctx, &argv_copy, &err)) {
        fprintf(stderr, "Argument parsing failed: %s\n", err->message);
        g_strfreev(argv_copy);
        return 1;
    }
    if (g_strv_length(argv_copy) < 2) {
        char *help = g_option_context_get_help(ctx, TRUE, NULL);
        fprintf(stderr, "%s", help);
        g_free(help);
        g_option_context_free(ctx);
        g_strfreev(argv_copy);
        return 1;
    }
    const char *cmd = argv_copy[1];
    /* For search we may also want argv_copy[2] as the query. */
    const char *arg2 = g_strv_length(argv_copy) > 2 ? argv_copy[2] : "";
    g_option_context_free(ctx);

    if (verbose) {
        const gchar *env = g_getenv("G_MESSAGES_DEBUG");
        if (!env) env = "";
        gchar *dbg = g_strdup_printf("%s all", env);
        g_setenv("G_MESSAGES_DEBUG", dbg, TRUE);
        g_free(dbg);
    }

    EverythingConfig *cfg = everything_config_new();
    everything_config_load(cfg, NULL);
    if (roots_arg) {
        g_free(cfg->index_roots);
        cfg->index_roots = g_strdup(roots_arg);
    }

    int rc;
    if (g_strcmp0(cmd, "index") == 0)        rc = do_index(cfg);
    else if (g_strcmp0(cmd, "status") == 0)  rc = do_status(cfg);
    else if (g_strcmp0(cmd, "search") == 0)  rc = do_search(cfg, arg2);
    else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        rc = 1;
    }

    everything_config_free(cfg);
    g_strfreev(argv_copy);
    return rc;
}
