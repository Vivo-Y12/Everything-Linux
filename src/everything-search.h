/*
 * Everything-1.0.0 — Search engine
 *
 * Parses the user's query using a subset of voidtools Everything's query
 * syntax and produces a parameterised SQL statement that runs against
 * the files table + files_fts FTS5 virtual table.
 *
 * Supported syntax (a deliberately useful subset):
 *
 *   word              substring match on filename (default AND between words)
 *   "phrase"          exact phrase match on filename
 *   foo|bar           OR
 *   !foo              NOT (exclude)
 *   *.txt             wildcard glob on filename (converted to GLOB)
 *   path:foo          match in path component
 *   ext:txt           exact extension match
 *   folder:           only folders (no operand)
 *   file:             only files
 *   empty:            zero-byte files
 *   size:>1mb         size filter; supports < > <= >= == and units kb/mb/gb
 *   dm:lastweek       date-modified relative: today/yesterday/thisweek/
 *                     lastweek/thismonth/lastmonth/thisyear
 *   dm:>=2024-01-01   date-modified absolute (>= <= ==)
 *   regex:foo         regex (matched in C, not SQL — slower, post-filter)
 *
 * The output SQL is always:
 *   SELECT <cols> FROM files [JOIN files_fts ON ...]
 *   [WHERE ...]
 *   [ORDER BY ...]
 *   [LIMIT ?];
 * The caller owns the resulting sqlite3_stmt.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef EVERYTHING_SEARCH_H
#define EVERYTHING_SEARCH_H

#include <glib.h>
#include <sqlite3.h>
#include "everything-types.h"
#include "everything-index.h"

typedef enum {
    SORT_BY_NAME       = 0,
    SORT_BY_PATH       = 1,
    SORT_BY_SIZE       = 2,
    SORT_BY_TYPE       = 3,  /* extension */
    SORT_BY_MTIME      = 4,
    SORT_BY_CTIME      = 5,
    SORT_BY_ATIME      = 6,
    SORT_BY_ATTRIBUTES = 7,
    SORT_BY_EXTENSION  = 8
} SortColumn;

typedef enum { SORT_ASC = 0, SORT_DESC = 1 } SortDirection;

typedef struct {
    char          *query;          /* original user query */
    SortColumn     sort_col;
    SortDirection  sort_dir;
    int            limit;          /* 0 == unlimited */
    gboolean       match_case;     /* case-sensitive search */
    gboolean       match_whole_word; /* match whole words only */
    gboolean       match_path;     /* search in path as well as name */
} SearchRequest;

SearchRequest *search_request_new(const char *query);
void           search_request_free(SearchRequest *req);

/* Compile the request into a prepared statement bound with all values.
 * Returns NULL on parse error (caller frees via g_error_free). */
sqlite3_stmt  *search_compile(EverythingIndex *idx,
                              const SearchRequest *req,
                              GError **error);

/* Convenience: compile + fetch all rows into a GArray of ResultRow.
 * Caller frees each ResultRow's strings and the array itself. */
GArray        *search_execute(EverythingIndex *idx,
                              const SearchRequest *req,
                              guint64 *out_total,
                              GError **error);

/* Format a byte size as Everything does: 12.3 KB, 1.4 GB. */
char          *format_size(uint64_t bytes);

/* Format a unix timestamp as "YYYY-MM-DD HH:MM:SS" (local time). */
char          *format_time(time_t t);

#endif /* EVERYTHING_SEARCH_H */
