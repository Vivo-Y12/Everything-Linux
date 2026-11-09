/*
 * Everything-1.0.0 — ResultRow GObject type registration
 *
 * SPDX-License-Identifier: MIT
 */
#include "everything-types.h"

G_DEFINE_TYPE(EverythingResultRow, everything_result_row, G_TYPE_OBJECT)

static void
everything_result_row_finalize(GObject *obj)
{
    EverythingResultRow *r = EVERYTHING_RESULT_ROW(obj);
    g_clear_pointer(&r->name,       g_free);
    g_clear_pointer(&r->path,       g_free);
    g_clear_pointer(&r->fullpath,   g_free);
    g_clear_pointer(&r->ext,        g_free);
    g_clear_pointer(&r->type_label, g_free);
    g_clear_pointer(&r->attrs,      g_free);
    G_OBJECT_CLASS(everything_result_row_parent_class)->finalize(obj);
}

static void
everything_result_row_class_init(EverythingResultRowClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = everything_result_row_finalize;
}

static void
everything_result_row_init(EverythingResultRow *r)
{
    (void)r;
}

EverythingResultRow *
everything_result_row_new(void)
{
    return g_object_new(EVERYTHING_TYPE_RESULT_ROW, NULL);
}
