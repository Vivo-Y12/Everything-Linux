/*
 * Everything-1.0.0 — Search engine implementation
 *
 * SPDX-License-Identifier: MIT
 */
#include "everything-search.h"

#include <ctype.h>
#include <glib.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ *
 * Request lifecycle
 * ------------------------------------------------------------------ */

SearchRequest *
search_request_new(const char *query)
{
    SearchRequest *r = g_new0(SearchRequest, 1);
    r->query    = g_strdup(query ? query : "");
    r->sort_col = SORT_BY_NAME;
    r->sort_dir = SORT_ASC;
    r->limit    = 100000;
    return r;
}

void
search_request_free(SearchRequest *r)
{
    if (!r) return;
    g_free(r->query);
    g_free(r);
}

/* ------------------------------------------------------------------ *
 * Formatting helpers (UI also uses these)
 * ------------------------------------------------------------------ */

char *
format_size(uint64_t bytes)
{
    if (bytes < 1024ULL)
        return g_strdup_printf("%llu B", (unsigned long long)bytes);
    double v = (double)bytes;
    static const char *units[] = {"KB", "MB", "GB", "TB", "PB"};
    int u = -1;
    do { v /= 1024.0; u++; } while (v >= 1024.0 && u < 4);
    if (v >= 100.0) return g_strdup_printf("%.0f %s", v, units[u]);
    if (v >= 10.0)  return g_strdup_printf("%.1f %s", v, units[u]);
    return g_strdup_printf("%.2f %s", v, units[u]);
}

char *
format_time(time_t t)
{
    struct tm tm;
    if (!localtime_r(&t, &tm)) return g_strdup("?");
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return g_strdup(buf);
}

/* ------------------------------------------------------------------ *
 * Token classification
 * ------------------------------------------------------------------ */

typedef enum {
    TOK_END = 0,
    TOK_WORD,        /* plain substring */
    TOK_PHRASE,      /* "..." */
    TOK_OR,          /* | */
    TOK_NOT,         /* ! */
    TOK_LPAREN,      /* ( */
    TOK_RPAREN,      /* ) */
    TOK_OPERATOR     /* ext:foo, size:>1mb, path:foo, ... */
} TokKind;

typedef struct {
    TokKind  kind;
    char    *text;     /* operand; for TOK_OPERATOR it is the part after ':' */
    char    *op;       /* for TOK_OPERATOR: the operator name (ext, size, ...) */
} Token;

typedef struct {
    const char *p;
    GString    *err;
} Lexer;

static void
lex_skip_ws(Lexer *L)
{
    while (*L->p && g_ascii_isspace(*L->p)) L->p++;
}

static Token
lex_next(Lexer *L)
{
    lex_skip_ws(L);
    Token t = {0};
    if (*L->p == '\0') { t.kind = TOK_END; return t; }

    char c = *L->p;
    if (c == '|') { L->p++; t.kind = TOK_OR; return t; }
    if (c == '!') { L->p++; t.kind = TOK_NOT; return t; }
    if (c == '(') { L->p++; t.kind = TOK_LPAREN; return t; }
    if (c == ')') { L->p++; t.kind = TOK_RPAREN; return t; }
    if (c == '"') {
        L->p++;
        GString *s = g_string_new(NULL);
        while (*L->p && *L->p != '"') {
            g_string_append_c(s, *L->p); L->p++;
        }
        if (*L->p == '"') L->p++;
        t.kind = TOK_PHRASE;
        t.text = g_string_free(s, FALSE);
        return t;
    }

    /* Otherwise a word or an operator. An operator is an identifier
     * followed by ':' with the next char not whitespace/pipe. */
    const char *start = L->p;
    while (*L->p && !g_ascii_isspace(*L->p) &&
           *L->p != '|' && *L->p != '!' &&
           *L->p != '(' && *L->p != ')') {
        if (*L->p == ':') {
            /* Found a colon — split into operator + operand. */
            gsize oplen = (gsize)(L->p - start);
            char *op = g_strndup(start, oplen);
            L->p++; /* skip ':' */
            const char *operand = L->p;
            while (*L->p && !g_ascii_isspace(*L->p) &&
                   *L->p != '|' && *L->p != '!' &&
                   *L->p != '(' && *L->p != ')')
                L->p++;
            gsize operandlen = (gsize)(L->p - operand);
            t.kind  = TOK_OPERATOR;
            t.op    = op;
            t.text  = g_strndup(operand, operandlen);
            return t;
        }
        L->p++;
    }
    gsize wlen = (gsize)(L->p - start);
    t.kind = TOK_WORD;
    t.text = g_strndup(start, wlen);
    return t;
}

/* ------------------------------------------------------------------ *
 * SQL builder
 *
 * We do not aim for a full boolean expression tree — that would be
 * overkill for the UI's needs. Instead, we build a flat WHERE clause
 * of ANDed predicates, with OR support via IN/UNION semantics.
 *
 * Strategy:
 *   - Collect all "positive" filename substrings into one FTS5 MATCH.
 *   - Collect all "negative" filename substrings into NOT-subqueries.
 *   - Collect operators (ext:, size:, dm:, path:, folder:, file:,
 *     empty:) as additional AND predicates on the files table.
 *
 * This matches Everything's user-visible behaviour for the common case
 * ("a b c" returns files whose name contains a, b and c as substrings).
 * ------------------------------------------------------------------ */

typedef struct {
    GPtrArray *pos_terms;     /* char* — filename substrings to AND */
    GPtrArray *neg_terms;     /* char* — filename substrings to NOT contain */
    GPtrArray *or_terms;      /* char* — alternative substrings (any matches) */
    GPtrArray *phrases;       /* char* — quoted exact phrases */
    GPtrArray *path_terms;    /* char* — path substrings */
    GPtrArray *ext_eq;        /* char* — exact extension */
    GPtrArray *ext_ne;        /* char* — excluded extension */
    GString   *extra_where;   /* extra AND fragments, parameterised */
    GPtrArray *params;        /* char* — bound string params for extra_where */
    gboolean   only_dirs;
    gboolean   only_files;
    gboolean   only_empty;
    gboolean   has_size_filter;
    int        size_op;       /* SQLITE_INDEX_? — we re-encode as <,>,<=,>=,= */
    uint64_t   size_value;
} QueryBuilder;

static QueryBuilder *
qb_new(void)
{
    QueryBuilder *q = g_new0(QueryBuilder, 1);
    q->pos_terms  = g_ptr_array_new_with_free_func(g_free);
    q->neg_terms  = g_ptr_array_new_with_free_func(g_free);
    q->or_terms   = g_ptr_array_new_with_free_func(g_free);
    q->phrases    = g_ptr_array_new_with_free_func(g_free);
    q->path_terms = g_ptr_array_new_with_free_func(g_free);
    q->ext_eq     = g_ptr_array_new_with_free_func(g_free);
    q->ext_ne     = g_ptr_array_new_with_free_func(g_free);
    q->extra_where = g_string_new(NULL);
    q->params      = g_ptr_array_new_with_free_func(g_free);
    return q;
}

static void
qb_free(QueryBuilder *q)
{
    if (!q) return;
    g_ptr_array_free(q->pos_terms,  TRUE);
    g_ptr_array_free(q->neg_terms,  TRUE);
    g_ptr_array_free(q->or_terms,   TRUE);
    g_ptr_array_free(q->phrases,    TRUE);
    g_ptr_array_free(q->path_terms, TRUE);
    g_ptr_array_free(q->ext_eq,     TRUE);
    g_ptr_array_free(q->ext_ne,     TRUE);
    g_string_free(q->extra_where, TRUE);
    g_ptr_array_free(q->params,    TRUE);
    g_free(q);
}

/* Parse a size token like "1mb", "500kb", "1.5gb". Returns FALSE if
 * unparseable. */
static gboolean
parse_size(const char *s, uint64_t *out)
{
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (end == s) return FALSE;
    while (*end && g_ascii_isspace(*end)) end++;
    if (g_ascii_strcasecmp(end, "b")  == 0 || *end == '\0')
        { *out = (uint64_t)v; return TRUE; }
    if (g_ascii_strcasecmp(end, "kb") == 0 || g_ascii_strcasecmp(end, "k") == 0)
        { *out = (uint64_t)(v * 1024); return TRUE; }
    if (g_ascii_strcasecmp(end, "mb") == 0 || g_ascii_strcasecmp(end, "m") == 0)
        { *out = (uint64_t)(v * 1024 * 1024); return TRUE; }
    if (g_ascii_strcasecmp(end, "gb") == 0 || g_ascii_strcasecmp(end, "g") == 0)
        { *out = (uint64_t)(v * 1024 * 1024 * 1024); return TRUE; }
    return FALSE;
}

/* Parse a relative date keyword into a unix timestamp for "since". */
static gboolean
parse_relative_date(const char *s, time_t *out)
{
    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    /* Start of today */
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    time_t today_start = mktime(&tm);

    if (g_ascii_strcasecmp(s, "today")        == 0) { *out = today_start; return TRUE; }
    if (g_ascii_strcasecmp(s, "yesterday")    == 0) { *out = today_start - 86400; return TRUE; }
    if (g_ascii_strcasecmp(s, "thisweek")     == 0) {
        /* Monday as start of week */
        int dow = (tm.tm_wday + 6) % 7;
        *out = today_start - (time_t)dow * 86400; return TRUE; }
    if (g_ascii_strcasecmp(s, "lastweek")     == 0) {
        int dow = (tm.tm_wday + 6) % 7;
        *out = today_start - (time_t)(dow + 7) * 86400; return TRUE; }
    if (g_ascii_strcasecmp(s, "thismonth")    == 0) {
        tm.tm_mday = 1; *out = mktime(&tm); return TRUE; }
    if (g_ascii_strcasecmp(s, "lastmonth")    == 0) {
        tm.tm_mday = 1; tm.tm_mon -= 1; *out = mktime(&tm); return TRUE; }
    if (g_ascii_strcasecmp(s, "thisyear")     == 0) {
        tm.tm_mon = 0; tm.tm_mday = 1; *out = mktime(&tm); return TRUE; }
    /* YYYY-MM-DD */
    int y, m, d;
    if (sscanf(s, "%d-%d-%d", &y, &m, &d) == 3 && y > 1970) {
        struct tm t2 = {0};
        t2.tm_year = y - 1900; t2.tm_mon = m - 1; t2.tm_mday = d;
        t2.tm_hour = 0; t2.tm_isdst = -1;
        *out = mktime(&t2);
        return TRUE;
    }
    return FALSE;
}

static void
handle_operator(QueryBuilder *q, const char *op, const char *operand)
{
    if (g_ascii_strcasecmp(op, "ext") == 0) {
        if (*operand == '!') {
            /* Support multiple extensions separated by ; e.g. ext:!txt;log */
            gchar **parts = g_strsplit(operand + 1, ";", -1);
            for (guint i = 0; parts[i]; i++) {
                char *t = g_strstrip(parts[i]);
                if (*t) g_ptr_array_add(q->ext_ne, g_ascii_strdown(t, -1));
            }
            g_strfreev(parts);
        } else {
            /* Support multiple extensions separated by ; e.g. ext:cpp;h;cs */
            gchar **parts = g_strsplit(operand, ";", -1);
            for (guint i = 0; parts[i]; i++) {
                char *t = g_strstrip(parts[i]);
                if (*t) g_ptr_array_add(q->ext_eq, g_ascii_strdown(t, -1));
            }
            g_strfreev(parts);
        }
        return;
    }
    if (g_ascii_strcasecmp(op, "path") == 0) {
        g_ptr_array_add(q->path_terms, g_strdup(operand));
        return;
    }
    if (g_ascii_strcasecmp(op, "parent") == 0) {
        /* parent:/path/to/dir — find files directly in this folder.
         * Matches f.path exactly (case-insensitive). */
        g_string_append_printf(q->extra_where, " AND lower(f.path) = lower(?)");
        g_ptr_array_add(q->params, g_strdup(operand));
        return;
    }
    if (g_ascii_strcasecmp(op, "len") == 0) {
        /* len:>20 or len:10 or len:5..15 — filename length filter */
        const char *p = operand;
        int op_type = 4;
        if (*p == '>') { op_type = 2; p++; if (*p=='=') {op_type=3;p++;} }
        else if (*p == '<') { op_type = 0; p++; if (*p=='=') {op_type=1;p++;} }
        char *end;
        long v = strtol(p, &end, 10);
        if (end != p) {
            /* Check for range: len:5..15 */
            if (*end == '.' && end[1] == '.') {
                long v2 = strtol(end + 2, &end, 10);
                if (end != end + 2) {
                    g_string_append_printf(q->extra_where,
                        " AND length(f.name) BETWEEN %ld AND %ld", v, v2);
                    return;
                }
            }
            const char *sqlop = (op_type==0)?"<":(op_type==1)?"<=":(op_type==2)?">":(op_type==3)?">=":"=";
            g_string_append_printf(q->extra_where, " AND length(f.name) %s %ld",
                sqlop, v);
        }
        return;
    }
    if (g_ascii_strcasecmp(op, "folder") == 0) {
        q->only_dirs = (operand[0] == '\0' || g_ascii_strcasecmp(operand,"yes") == 0
                        || g_ascii_strcasecmp(operand,"1") == 0 || g_ascii_strcasecmp(operand,"true") == 0);
        return;
    }
    if (g_ascii_strcasecmp(op, "file") == 0) {
        q->only_files = (operand[0] == '\0' || g_ascii_strcasecmp(operand,"yes") == 0
                         || g_ascii_strcasecmp(operand,"1") == 0 || g_ascii_strcasecmp(operand,"true") == 0);
        return;
    }
    if (g_ascii_strcasecmp(op, "empty") == 0) {
        q->only_empty = TRUE; q->only_files = TRUE;
        return;
    }
    if (g_ascii_strcasecmp(op, "size") == 0) {
        /* operand like ">1mb" or ">=500kb" or "==100" or "10mb..50mb" */
        const char *p = operand;
        int op_type = -1; /* 0:< 1:<= 2:> 3:>= 4:== */
        if (*p == '<' && p[1] == '=') { op_type = 1; p += 2; }
        else if (*p == '>' && p[1] == '=') { op_type = 3; p += 2; }
        else if (*p == '<') { op_type = 0; p += 1; }
        else if (*p == '>') { op_type = 2; p += 1; }
        else if (*p == '=' && p[1] == '=') { op_type = 4; p += 2; }
        else if (*p == '=') { op_type = 4; p += 1; }
        else { op_type = 4; }
        uint64_t v = 0;
        if (parse_size(p, &v)) {
            /* Check for range: size:10mb..50mb */
            const char *dotdot = strstr(p, "..");
            if (dotdot) {
                uint64_t v2 = 0;
                if (parse_size(dotdot + 2, &v2)) {
                    g_string_append_printf(q->extra_where,
                        " AND f.size BETWEEN %llu AND %llu",
                        (unsigned long long)v, (unsigned long long)v2);
                    return;
                }
            }
            q->has_size_filter = TRUE;
            q->size_op = op_type;
            q->size_value = v;
        }
        return;
    }
    if (g_ascii_strcasecmp(op, "dm") == 0 || g_ascii_strcasecmp(op, "dc") == 0
        || g_ascii_strcasecmp(op, "da") == 0) {
        /* operand like "lastweek" or ">=2024-01-01" or "2025-07-01..2026-06-30" */
        const char *p = operand;
        const char *col = (g_ascii_strcasecmp(op, "dc") == 0) ? "ctime" :
                          (g_ascii_strcasecmp(op, "da") == 0) ? "atime" : "mtime";
        int op_type = 3; /* default >= */
        if (*p == '<' && p[1] == '=') { op_type = 1; p += 2; }
        else if (*p == '>' && p[1] == '=') { op_type = 3; p += 2; }
        else if (*p == '<') { op_type = 0; p += 1; }
        else if (*p == '>') { op_type = 2; p += 1; }
        else if (*p == '=' && p[1] == '=') { op_type = 4; p += 2; }
        else if (*p == '=') { op_type = 4; p += 1; }
        time_t t = 0;
        if (parse_relative_date(p, &t)) {
            /* Check for range: dm:2025-07-01..2026-06-30 */
            const char *dotdot = strstr(p, "..");
            if (dotdot) {
                time_t t2 = 0;
                if (parse_relative_date(dotdot + 2, &t2)) {
                    g_string_append_printf(q->extra_where,
                        " AND f.%s BETWEEN %lld AND %lld",
                        col, (long long)t, (long long)t2);
                    return;
                }
            }
            const char *sqlop = (op_type==0)?"<":(op_type==1)?"<=":(op_type==2)?">":(op_type==3)?">=":"==";
            g_string_append_printf(q->extra_where, " AND f.%s %s %lld",
                col, sqlop, (long long)t);
        }
        return;
    }
    /* Unknown operator: treat the whole thing as a path substring. */
    g_ptr_array_add(q->pos_terms, g_strdup_printf("%s:%s", op, operand));
}

/* ------------------------------------------------------------------ *
 * Compile
 * ------------------------------------------------------------------ */

sqlite3_stmt *
search_compile(EverythingIndex *idx, const SearchRequest *req, GError **error)
{
    g_return_val_if_fail(idx != NULL, NULL);
    g_return_val_if_fail(req   != NULL, NULL);

    QueryBuilder *q = qb_new();
    Lexer L = { .p = req->query };
    gboolean after_or = FALSE;
    Token t;
    while ((t = lex_next(&L)).kind != TOK_END) {
        switch (t.kind) {
        case TOK_OR:
            /* Move the most recently added positive term into the OR
             * group so that "a | b" produces "(a OR b)" rather than
             * "a AND (b)". Use steal_index (not remove_index) so the
             * string is NOT freed by pos_terms' destroy func — it is
             * re-owned by or_terms. */
            if (q->pos_terms->len > 0) {
                gchar *last = (gchar *)g_ptr_array_steal_index(q->pos_terms,
                                                                q->pos_terms->len - 1);
                g_ptr_array_add(q->or_terms, last);
            }
            after_or = TRUE;
            break;
        case TOK_NOT:
            /* next token's text will be negated — handled below by
             * checking a marker. For simplicity, we treat the next
             * WORD token as a negative. */
            {
                Token n = lex_next(&L);
                if (n.kind == TOK_WORD || n.kind == TOK_PHRASE)
                    g_ptr_array_add(q->neg_terms, g_strdup(n.text));
                g_free(n.text); g_free(n.op);
            }
            break;
        case TOK_WORD:
            /* Wildcard handling: if it contains * or ?, use GLOB. */
            if (strpbrk(t.text, "*?")) {
                /* Convert into a path-glob: filename GLOB ? */
                g_string_append_printf(q->extra_where,
                    " AND f.name GLOB ?");
                g_ptr_array_add(q->params, g_strdup(t.text));
            } else if (after_or) {
                g_ptr_array_add(q->or_terms, g_strdup(t.text));
                after_or = FALSE;
            } else {
                g_ptr_array_add(q->pos_terms, g_strdup(t.text));
            }
            g_free(t.text);
            break;
        case TOK_PHRASE:
            g_ptr_array_add(q->phrases, g_strdup(t.text));
            g_free(t.text);
            break;
        case TOK_OPERATOR:
            handle_operator(q, t.op, t.text);
            g_free(t.op); g_free(t.text);
            break;
        default:
            g_free(t.text); g_free(t.op);
            break;
        }
    }

    /* Build the SQL. */
    GString *sql = g_string_new(NULL);
    g_string_append(sql,
        "SELECT f.id, f.name, f.path, f.fullpath, f.size, f.is_dir, "
        "       f.ext, f.attrs, f.mtime, f.ctime, f.atime "
        "FROM files f ");

    /* Build FTS MATCH clause if any positive terms or phrases exist.
     *
     * FTS5 MATCH syntax has special characters: " * ( ) : AND OR NOT.
     * User input is interpolated into the MATCH string, so we must
     * escape it to prevent injection / silent query breakage. We wrap
     * every term in double-quotes (FTS5 phrase syntax) and escape
     * embedded double-quotes by doubling them (" -> ""). The trailing
     * * for prefix matching goes OUTSIDE the closing quote. */
    gboolean need_fts_join = FALSE;
    GString *match_expr = g_string_new(NULL);
    gboolean first = TRUE;

    /* Helper: append an escaped, quoted term to match_expr.
     * If prefix is true, append * after the closing quote for
     * FTS5 prefix matching. */
    #define APPEND_TERM(str, prefix) do { \
        g_string_append_c(match_expr, '"'); \
        for (const char *_c = (str); *_c; _c++) { \
            if (*_c == '"') g_string_append_c(match_expr, '"'); \
            g_string_append_c(match_expr, *_c); \
        } \
        g_string_append_c(match_expr, '"'); \
        if (prefix) g_string_append_c(match_expr, '*'); \
    } while (0)

    for (guint i = 0; i < q->phrases->len; i++) {
        const char *p = (const char *)g_ptr_array_index(q->phrases, i);
        if (!first) g_string_append(match_expr, " AND ");
        first = FALSE;
        APPEND_TERM(p, FALSE);
        need_fts_join = TRUE;
    }
    for (guint i = 0; i < q->pos_terms->len; i++) {
        const char *s = (const char *)g_ptr_array_index(q->pos_terms, i);
        if (!first) g_string_append(match_expr, " AND ");
        first = FALSE;
        APPEND_TERM(s, TRUE);   /* prefix match: term* */
        need_fts_join = TRUE;
    }
    if (q->or_terms->len > 0) {
        if (!first) g_string_append(match_expr, " AND ");
        first = FALSE;
        g_string_append_c(match_expr, '(');
        for (guint i = 0; i < q->or_terms->len; i++) {
            const char *s = (const char *)g_ptr_array_index(q->or_terms, i);
            if (i > 0) g_string_append(match_expr, " OR ");
            APPEND_TERM(s, TRUE);
        }
        g_string_append_c(match_expr, ')');
        need_fts_join = TRUE;
    }
    #undef APPEND_TERM

    if (need_fts_join) {
        g_string_append(sql, "JOIN files_fts ON files_fts.rowid = f.id ");
    }
    g_string_append(sql, "WHERE 1=1");

    if (need_fts_join) {
        /* Bind MATCH expr — FTS5 supports ? placeholder for the whole
         * match string. */
        g_string_append(sql, " AND files_fts MATCH ?");
    }

    /* Negative filename substrings: NOT LIKE. */
    for (guint i = 0; i < q->neg_terms->len; i++) {
        const char *s = (const char *)g_ptr_array_index(q->neg_terms, i);
        g_string_append_printf(sql, " AND f.name NOT LIKE ?");
        g_ptr_array_add(q->params, g_strdup_printf("%%%s%%", s));
    }
    /* Path substrings: f.path LIKE %x%. */
    for (guint i = 0; i < q->path_terms->len; i++) {
        const char *s = (const char *)g_ptr_array_index(q->path_terms, i);
        g_string_append_printf(sql, " AND f.path LIKE ?");
        g_ptr_array_add(q->params, g_strdup_printf("%%%s%%", s));
    }
    /* Extensions: equality, case-insensitive. Multiple extensions are
     * OR'd: ext:c;h → (lower(f.ext) = 'c' OR lower(f.ext) = 'h'). */
    if (q->ext_eq->len > 0) {
        if (q->ext_eq->len == 1) {
            g_string_append_printf(sql, " AND lower(f.ext) = ?");
            g_ptr_array_add(q->params, g_strdup(g_ptr_array_index(q->ext_eq, 0)));
        } else {
            g_string_append(sql, " AND (");
            for (guint i = 0; i < q->ext_eq->len; i++) {
                const char *s = (const char *)g_ptr_array_index(q->ext_eq, i);
                if (i > 0) g_string_append(sql, " OR ");
                g_string_append(sql, "lower(f.ext) = ?");
                g_ptr_array_add(q->params, g_strdup(s));
            }
            g_string_append_c(sql, ')');
        }
    }
    for (guint i = 0; i < q->ext_ne->len; i++) {
        const char *s = (const char *)g_ptr_array_index(q->ext_ne, i);
        g_string_append_printf(sql, " AND lower(f.ext) != ?");
        g_ptr_array_add(q->params, g_strdup(s));
    }
    if (q->only_dirs)   g_string_append(sql, " AND f.is_dir = 1");
    if (q->only_files)  g_string_append(sql, " AND f.is_dir = 0");
    if (q->only_empty)  g_string_append(sql, " AND f.size = 0");
    if (q->has_size_filter) {
        const char *op = (q->size_op==0)?"<":(q->size_op==1)?"<=":(q->size_op==2)?">":(q->size_op==3)?">=":"=";
        g_string_append_printf(sql, " AND f.size %s %llu",
            op, (unsigned long long)q->size_value);
    }

    g_string_append(sql, q->extra_where->str);

    /* ORDER BY. */
    const char *colname = NULL;
    switch (req->sort_col) {
        case SORT_BY_NAME:       colname = "f.name";       break;
        case SORT_BY_PATH:       colname = "f.path";       break;
        case SORT_BY_SIZE:       colname = "f.size";       break;
        case SORT_BY_TYPE:       colname = "f.ext";        break;
        case SORT_BY_MTIME:      colname = "f.mtime";      break;
        case SORT_BY_CTIME:      colname = "f.ctime";      break;
        case SORT_BY_ATIME:      colname = "f.atime";      break;
        case SORT_BY_ATTRIBUTES: colname = "f.attrs";      break;
        case SORT_BY_EXTENSION:  colname = "f.ext";        break;
    }
    g_string_append_printf(sql, " ORDER BY %s %s",
        colname, req->sort_dir == SORT_ASC ? "ASC" : "DESC");

    /* Secondary sort for stability. */
    g_string_append_printf(sql, ", f.fullpath %s",
        req->sort_dir == SORT_ASC ? "ASC" : "DESC");

    if (req->limit > 0)
        g_string_append_printf(sql, " LIMIT %d", req->limit);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(everything_index_db(idx), sql->str, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_set_error(error, 1, rc, "Prepare failed: %s\nSQL: %s",
                    sqlite3_errmsg(everything_index_db(idx)), sql->str);
        g_string_free(sql, TRUE);
        g_string_free(match_expr, TRUE);
        qb_free(q);
        return NULL;
    }

    int bind_idx = 1;
    if (need_fts_join) {
        sqlite3_bind_text(stmt, bind_idx++, match_expr->str, -1, SQLITE_TRANSIENT);
    }
    for (guint i = 0; i < q->params->len; i++) {
        const char *p = (const char *)g_ptr_array_index(q->params, i);
        sqlite3_bind_text(stmt, bind_idx++, p, -1, SQLITE_TRANSIENT);
    }

    g_string_free(sql, TRUE);
    g_string_free(match_expr, TRUE);
    qb_free(q);
    return stmt;
}

GArray *
search_execute(EverythingIndex *idx, const SearchRequest *req,
               guint64 *out_total, GError **error)
{
    sqlite3_stmt *stmt = search_compile(idx, req, error);
    if (!stmt) return NULL;

    GArray *rows = g_array_new(FALSE, FALSE, sizeof(EverythingResultRow *));
    guint64 total = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        EverythingResultRow *r = everything_result_row_new();
        r->id       = sqlite3_column_int64(stmt, 0);
        r->name     = g_strdup((const char *)sqlite3_column_text(stmt, 1));
        r->path     = g_strdup((const char *)sqlite3_column_text(stmt, 2));
        r->fullpath = g_strdup((const char *)sqlite3_column_text(stmt, 3));
        r->size     = (uint64_t)sqlite3_column_int64(stmt, 4);
        r->is_dir   = sqlite3_column_int(stmt, 5);
        const char *ext = (const char *)sqlite3_column_text(stmt, 6);
        r->ext      = g_strdup(ext ? ext : "");
        r->attrs    = g_strdup((const char *)sqlite3_column_text(stmt, 7));
        r->mtime    = (time_t)sqlite3_column_int64(stmt, 8);
        r->ctime    = (time_t)sqlite3_column_int64(stmt, 9);
        r->atime    = (time_t)sqlite3_column_int64(stmt, 10);
        if (r->is_dir)        r->type_label = g_strdup("Folder");
        else if (*r->ext)     r->type_label = g_ascii_strup(r->ext, -1);
        else                  r->type_label = g_strdup("File");
        g_array_append_val(rows, r);
        total++;
    }
    if (out_total) *out_total = total;
    sqlite3_finalize(stmt);
    return rows;
}
