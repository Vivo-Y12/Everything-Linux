/*
 * Everything-1.0.0 — Main GTK4 application window (implementation)
 *
 * SPDX-License-Identifier: MIT
 */
#include "everything-window.h"
#include "everything-config.h"
#include "everything-search.h"

#include <adwaita.h>
#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <gtk/gtk.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct _EverythingWindow {
    AdwApplicationWindow parent_instance;

    EverythingIndex *idx;

    /* Template widgets */
    AdwHeaderBar    *header_bar;
    GtkMenuButton   *menu_btn;
    GtkSearchEntry  *search_entry;
    GtkButton       *rebuild_btn;
    GtkScrolledWindow *scroller;
    GtkColumnView   *column_view;
    GtkLabel        *status_label;
    GtkLabel        *count_label;

    /* Data model */
    GListStore      *results;        /* of EverythingResultRow* (owned) */
    GtkSingleSelection *selection;

    /* Sort state */
    SortColumn       sort_col;
    SortDirection    sort_dir;

    /* Debounce timer for live-as-you-type search */
    guint            search_debounce_id;
};

G_DEFINE_FINAL_TYPE(EverythingWindow, everything_window, ADW_TYPE_APPLICATION_WINDOW)

enum {
    PROP_0,
    PROP_INDEX,
    N_PROPS
};
static GParamSpec *props[N_PROPS] = { NULL };

/* ------------- helpers ------------- */

static void
free_result_row(gpointer data)
{
    EverythingResultRow *r = EVERYTHING_RESULT_ROW(data);
    if (!r) return;
    /* The GObject's finalize handler frees the strings; we just drop
     * the ref we held in the GListStore. */
    g_object_unref(r);
}

static const char *
icon_name_for_row(const EverythingResultRow *r)
{
    if (r->is_dir) return "folder-symbolic";
    if (*r->ext) {
        if (g_ascii_strcasecmp(r->ext, "txt")  == 0) return "text-x-generic-symbolic";
        if (g_ascii_strcasecmp(r->ext, "md")   == 0) return "text-x-generic-symbolic";
        if (g_ascii_strcasecmp(r->ext, "pdf")  == 0) return "application-pdf-symbolic";
        if (g_ascii_strcasecmp(r->ext, "png")  == 0) return "image-x-generic-symbolic";
        if (g_ascii_strcasecmp(r->ext, "jpg")  == 0 ||
            g_ascii_strcasecmp(r->ext, "jpeg") == 0) return "image-x-generic-symbolic";
        if (g_ascii_strcasecmp(r->ext, "c")    == 0) return "text-x-generic-symbolic";
        if (g_ascii_strcasecmp(r->ext, "cpp")  == 0) return "text-x-generic-symbolic";
        if (g_ascii_strcasecmp(r->ext, "py")   == 0) return "text-x-generic-symbolic";
        if (g_ascii_strcasecmp(r->ext, "sh")   == 0) return "text-x-generic-symbolic";
        if (g_ascii_strcasecmp(r->ext, "zip")  == 0) return "application-zip-symbolic";
        if (g_ascii_strcasecmp(r->ext, "mp3")  == 0) return "audio-x-generic-symbolic";
        if (g_ascii_strcasecmp(r->ext, "mp4")  == 0) return "video-x-generic-symbolic";
    }
    return "text-x-generic-symbolic";
}

/* ------------- column factory setup/bind helpers ------------- */

/* "setup" callback for the Name column: builds an icon + label box.
 * NB: GtkSignalListItemFactory::setup passes (factory, item, user_data);
 * we ignore the factory. */
static void
name_col_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    (void)factory; (void)user_data;
    GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6));
    GtkImage *img = GTK_IMAGE(gtk_image_new());
    GtkLabel *lbl = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(lbl, 0.0);
    gtk_label_set_ellipsize(lbl, PANGO_ELLIPSIZE_END);
    gtk_box_append(box, GTK_WIDGET(img));
    gtk_box_append(box, GTK_WIDGET(lbl));
    gtk_list_item_set_child(item, GTK_WIDGET(box));
}

static void
name_col_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    (void)factory; (void)user_data;
    GtkBox *box = GTK_BOX(gtk_list_item_get_child(item));
    GtkImage *img = GTK_IMAGE(gtk_widget_get_first_child(GTK_WIDGET(box)));
    GtkLabel *lbl = GTK_LABEL(gtk_widget_get_next_sibling(GTK_WIDGET(img)));
    EverythingResultRow *r = EVERYTHING_RESULT_ROW(gtk_list_item_get_item(item));
    gtk_image_set_from_icon_name(img, icon_name_for_row(r));
    gtk_label_set_text(lbl, r->name);
}

/* "setup" callback for plain-text columns. */
static void
text_col_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    (void)factory; (void)user_data;
    GtkLabel *lbl = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(lbl, 0.0);
    gtk_label_set_ellipsize(lbl, PANGO_ELLIPSIZE_END);
    gtk_list_item_set_child(item, GTK_WIDGET(lbl));
}

static void
path_col_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    (void)factory; (void)user_data;
    GtkLabel *lbl = GTK_LABEL(gtk_list_item_get_child(item));
    EverythingResultRow *r = EVERYTHING_RESULT_ROW(gtk_list_item_get_item(item));
    gtk_label_set_text(lbl, r->path);
}

static void
size_col_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    (void)factory; (void)user_data;
    GtkLabel *lbl = GTK_LABEL(gtk_list_item_get_child(item));
    EverythingResultRow *r = EVERYTHING_RESULT_ROW(gtk_list_item_get_item(item));
    char *s = format_size(r->size);
    gtk_label_set_text(lbl, s);
    g_free(s);
}

static void
type_col_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    (void)factory; (void)user_data;
    GtkLabel *lbl = GTK_LABEL(gtk_list_item_get_child(item));
    EverythingResultRow *r = EVERYTHING_RESULT_ROW(gtk_list_item_get_item(item));
    gtk_label_set_text(lbl, r->type_label);
}

static void
mtime_col_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    (void)factory; (void)user_data;
    GtkLabel *lbl = GTK_LABEL(gtk_list_item_get_child(item));
    EverythingResultRow *r = EVERYTHING_RESULT_ROW(gtk_list_item_get_item(item));
    char *s = format_time(r->mtime);
    gtk_label_set_text(lbl, s);
    g_free(s);
}

static void
attrs_col_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    (void)factory; (void)user_data;
    GtkLabel *lbl = GTK_LABEL(gtk_list_item_get_child(item));
    EverythingResultRow *r = EVERYTHING_RESULT_ROW(gtk_list_item_get_item(item));
    gtk_label_set_text(lbl, r->attrs);
}

static GtkColumnViewColumn *
make_column(const char *title,
            void (*setup_fn)(GtkSignalListItemFactory *, GtkListItem *, gpointer),
            void (*bind_fn)(GtkSignalListItemFactory *, GtkListItem *, gpointer),
            gboolean expand)
{
    GtkListItemFactory *factory =
        GTK_LIST_ITEM_FACTORY(gtk_signal_list_item_factory_new());
    g_signal_connect(factory, "setup", G_CALLBACK(setup_fn), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(bind_fn),  NULL);

    GtkColumnViewColumn *col = gtk_column_view_column_new(title, factory);
    gtk_column_view_column_set_expand(col, expand);
    return col;
}

/* ------------- search execution ------------- */

static gboolean
do_search_idle(gpointer data)
{
    EverythingWindow *win = EVERYTHING_WINDOW(data);
    win->search_debounce_id = 0;

    const char *q = gtk_editable_get_text(GTK_EDITABLE(win->search_entry));
    SearchRequest *req = search_request_new(q);
    req->sort_col = win->sort_col;
    req->sort_dir = win->sort_dir;
    req->limit    = 100000;

    GError *err = NULL;
    guint64 total = 0;
    GArray *rows = search_execute(win->idx, req, &total, &err);
    search_request_free(req);

    if (!rows) {
        char *msg = g_strdup_printf("Search error: %s",
                                    err ? err->message : "?");
        gtk_label_set_text(win->status_label, msg);
        g_free(msg);
        g_clear_error(&err);
        return G_SOURCE_REMOVE;
    }

    /* Replace contents of results store. search_execute returns a GArray
     * of EverythingResultRow* pointers (GObjects with refcount 1).
     * g_list_store_append adds its own ref, so we drop ours after. */
    g_list_store_remove_all(win->results);
    for (guint i = 0; i < rows->len; i++) {
        EverythingResultRow *r = g_array_index(rows, EverythingResultRow *, i);
        g_list_store_append(win->results, r); /* store refs it */
        g_object_unref(r);                    /* drop our ref */
    }
    g_array_free(rows, TRUE); /* array owns nothing — refs transferred */

    char *status = g_strdup_printf("%llu results", (unsigned long long)total);
    gtk_label_set_text(win->count_label, status);
    g_free(status);

    return G_SOURCE_REMOVE;
}

static void
schedule_search(EverythingWindow *win)
{
    if (win->search_debounce_id)
        g_source_remove(win->search_debounce_id);
    /* 80ms debounce: feels instant but avoids hammering SQLite on every keystroke. */
    win->search_debounce_id = g_timeout_add(80, do_search_idle, win);
}

/* ------------- signal handlers (bound via .ui template) ------------- */

void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    (void)entry;
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    schedule_search(win);
}

void
on_search_activate(GtkSearchEntry *entry, gpointer user_data)
{
    (void)entry;
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    if (win->search_debounce_id) {
        g_source_remove(win->search_debounce_id);
        win->search_debounce_id = 0;
    }
    g_idle_add(do_search_idle, win);
}

void
on_rebuild_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    everything_window_rebuild_index(win);
}

void
on_row_activated(GtkColumnView *view, guint pos, gpointer user_data)
{
    (void)view;
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    g_return_if_fail(pos < g_list_model_get_n_items(G_LIST_MODEL(win->results)));
    EverythingResultRow *r = (EverythingResultRow *)g_list_model_get_item(G_LIST_MODEL(win->results), pos);
    g_return_if_fail(r != NULL);
    /* Open with the portal. g_filename_to_uri needs (filename, hostname, err). */
    GError *err = NULL;
    gchar *uri = g_filename_to_uri(r->fullpath, NULL, &err);
    if (uri) {
        gtk_show_uri(GTK_WINDOW(win), uri, GDK_CURRENT_TIME);
        g_free(uri);
    } else if (err) {
        char *msg = g_strdup_printf("Open failed: %s", err->message);
        gtk_label_set_text(win->status_label, msg);
        g_free(msg);
        g_error_free(err);
    }
}

/* ------------- index callbacks (fired from worker threads) ------------- */

static gboolean
emit_state_change_idle(gpointer data)
{
    gpointer *tuple = (gpointer *)data;
    EverythingWindow *win = EVERYTHING_WINDOW(tuple[0]);
    IndexState s = (IndexState)GPOINTER_TO_INT(tuple[1]);
    char *msg = (char *)tuple[2];

    const char *label = "Ready";
    switch (s) {
        case INDEX_STATE_CRAWLING: label = "Indexing…"; break;
        case INDEX_STATE_WATCHING: label = "Watching for changes"; break;
        case INDEX_STATE_PAUSED:   label = "Paused"; break;
        case INDEX_STATE_ERROR:    label = "Index error"; break;
        case INDEX_STATE_IDLE:     label = "Ready"; break;
    }
    char *full = g_strdup_printf("%s — %s", label, msg ? msg : "");
    gtk_label_set_text(win->status_label, full);
    g_free(full);

    g_object_unref(win);
    g_free(tuple);
    g_free(msg);
    return G_SOURCE_REMOVE;
}

static void
on_index_state(EverythingIndex *idx, IndexState s, const char *msg, gpointer user_data)
{
    (void)idx;
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    gpointer *tuple = g_new(gpointer, 3);
    tuple[0] = g_object_ref(win);
    tuple[1] = GINT_TO_POINTER(s);
    tuple[2] = g_strdup(msg ? msg : "");
    g_idle_add(emit_state_change_idle, tuple);
}

static gboolean
emit_changed_idle(gpointer data)
{
    EverythingWindow *win = EVERYTHING_WINDOW(data);
    schedule_search(win);
    g_object_unref(win);
    return G_SOURCE_REMOVE;
}

static void
on_index_changed(EverythingIndex *idx, gpointer user_data)
{
    (void)idx;
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    g_idle_add(emit_changed_idle, g_object_ref(win));
}

static gboolean
emit_progress_idle(gpointer data)
{
    gpointer *tuple = (gpointer *)data;
    EverythingWindow *win = EVERYTHING_WINDOW(tuple[0]);
    guint64 visited = GPOINTER_TO_SIZE(tuple[1]);
    g_object_unref(win);
    g_free(tuple);
    char *msg = g_strdup_printf("Indexed %llu entries…",
                                (unsigned long long)visited);
    gtk_label_set_text(win->status_label, msg);
    g_free(msg);
    return G_SOURCE_REMOVE;
}

static void
on_index_progress(EverythingIndex *idx, guint64 visited, guint64 total, gpointer user_data)
{
    (void)idx; (void)total;
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    gpointer *tuple = g_new(gpointer, 2);
    tuple[0] = g_object_ref(win);
    tuple[1] = GSIZE_TO_POINTER(visited);
    g_idle_add(emit_progress_idle, tuple);
}

/* ------------- column sort ------------- */

static void
on_column_clicked(GtkColumnViewColumn *col, gpointer user_data)
{
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    const char *title = gtk_column_view_column_get_title(col);
    SortColumn clicked = SORT_BY_NAME;
    if (g_strcmp0(title, "Name")        == 0) clicked = SORT_BY_NAME;
    else if (g_strcmp0(title, "Path")   == 0) clicked = SORT_BY_PATH;
    else if (g_strcmp0(title, "Size")   == 0) clicked = SORT_BY_SIZE;
    else if (g_strcmp0(title, "Type")   == 0) clicked = SORT_BY_TYPE;
    else if (g_strcmp0(title, "Modified")==0) clicked = SORT_BY_MTIME;
    else if (g_strcmp0(title, "Attributes")==0)clicked= SORT_BY_ATTRIBUTES;
    if (clicked == win->sort_col)
        win->sort_dir = (win->sort_dir == SORT_ASC) ? SORT_DESC : SORT_ASC;
    else {
        win->sort_col = clicked;
        win->sort_dir = SORT_ASC;
    }
    schedule_search(win);
}

/* ------------- context menu actions ------------- */

static EverythingResultRow *
selected_row(EverythingWindow *win)
{
    guint pos = gtk_single_selection_get_selected(win->selection);
    if (pos >= g_list_model_get_n_items(G_LIST_MODEL(win->results))) return NULL;
    return (EverythingResultRow *)g_list_model_get_item(G_LIST_MODEL(win->results), pos);
}

static void
action_copy_path(GSimpleAction *a, GVariant *v, gpointer user_data)
{
    (void)a; (void)v;
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    EverythingResultRow *r = selected_row(win);
    if (!r) return;
    GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(win));
    gdk_clipboard_set_text(cb, r->path);
}
static void
action_copy_name(GSimpleAction *a, GVariant *v, gpointer user_data)
{
    (void)a; (void)v;
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    EverythingResultRow *r = selected_row(win);
    if (!r) return;
    GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(win));
    gdk_clipboard_set_text(cb, r->name);
}
static void
action_copy_full(GSimpleAction *a, GVariant *v, gpointer user_data)
{
    (void)a; (void)v;
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    EverythingResultRow *r = selected_row(win);
    if (!r) return;
    GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(win));
    gdk_clipboard_set_text(cb, r->fullpath);
}
static void
action_open_path(GSimpleAction *a, GVariant *v, gpointer user_data)
{
    (void)a; (void)v;
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    EverythingResultRow *r = selected_row(win);
    if (!r) return;
    GError *err = NULL;
    gchar *argv[] = { (gchar *)"xdg-open", r->path, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                  NULL, NULL, NULL, &err);
    if (err) {
        char *msg = g_strdup_printf("Open path failed: %s", err->message);
        gtk_label_set_text(win->status_label, msg);
        g_free(msg); g_error_free(err);
    }
}
static void
action_rebuild(GSimpleAction *a, GVariant *v, gpointer user_data)
{
    (void)a; (void)v;
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    everything_window_rebuild_index(win);
}
/* Async export-csv callback: writes the chosen file. */
static void
on_save_response(GObject *source, GAsyncResult *res, gpointer user_data)
{
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
    GFile *f = gtk_file_dialog_save_finish(dlg, res, NULL);
    if (!f) { g_object_unref(win); return; }

    GError *err = NULL;
    GFileOutputStream *out = g_file_replace(f, NULL, FALSE,
                                             G_FILE_CREATE_NONE, NULL, &err);
    if (out) {
        const char *hdr = "Name,Path,Size,Type,Modified,Attributes\n";
        g_output_stream_write_all(G_OUTPUT_STREAM(out), hdr, strlen(hdr),
                                  NULL, NULL, NULL);
        guint n = g_list_model_get_n_items(G_LIST_MODEL(win->results));
        for (guint i = 0; i < n; i++) {
            EverythingResultRow *r = (EverythingResultRow *)g_list_model_get_item(
                G_LIST_MODEL(win->results), i);
            char *mt = format_time(r->mtime);
            char *line = g_strdup_printf("\"%s\",\"%s\",%llu,\"%s\",\"%s\",\"%s\"\n",
                r->name, r->path, (unsigned long long)r->size,
                r->type_label, mt, r->attrs);
            g_output_stream_write_all(G_OUTPUT_STREAM(out), line, strlen(line),
                                      NULL, NULL, NULL);
            g_free(line); g_free(mt);
        }
        g_object_unref(out);
    }
    if (err) {
        char *msg = g_strdup_printf("Export failed: %s", err->message);
        gtk_label_set_text(win->status_label, msg);
        g_free(msg); g_error_free(err);
    }
    g_object_unref(f);
    g_object_unref(win);
}

static void
action_export_csv(GSimpleAction *a, GVariant *v, gpointer user_data)
{
    (void)a; (void)v;
    EverythingWindow *win = EVERYTHING_WINDOW(user_data);
    /* GTK 4.10+ replaced GtkFileChooserNative with the async GtkFileDialog. */
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Export results as CSV");
    gtk_file_dialog_set_initial_name(dlg, "everything-results.csv");
    GFile *initial = g_file_new_for_path(g_get_home_dir());
    gtk_file_dialog_set_initial_folder(dlg, initial);
    g_object_unref(initial);
    gtk_file_dialog_save(dlg, GTK_WINDOW(win), NULL,
                         on_save_response, g_object_ref(win));
    g_object_unref(dlg);
}

/* ------------- class setup ------------- */

static void
everything_window_init(EverythingWindow *win)
{
    gtk_widget_init_template(GTK_WIDGET(win));

    /* GListStore requires G_TYPE_OBJECT-derived items. EverythingResultRow
     * is registered as a GObject for exactly this purpose. */
    win->results = g_list_store_new(EVERYTHING_TYPE_RESULT_ROW);

    win->selection = gtk_single_selection_new(G_LIST_MODEL(win->results));
    gtk_column_view_set_model(win->column_view, GTK_SELECTION_MODEL(win->selection));

    win->sort_col = SORT_BY_NAME;
    win->sort_dir = SORT_ASC;

    /* Build columns. In GTK 4 the column header click → sort is handled
     * by attaching a GtkSorter to each GtkColumnViewColumn; the column
     * view itself coordinates the actual sorting via its sorter model.
     * We don't wire that up here for the initial Flathub submission;
     * sorting is done by re-running the SQL query with the right
     * ORDER BY clause (see schedule_search → do_search_idle). Clicking
     * a column header still cycles the sort via the per-column sorter
     * set below — the SQL re-run picks up the new sort_col/sort_dir. */
    GtkColumnViewColumn *c;
    c = make_column("Name",        name_col_setup,  name_col_bind,  TRUE);
    gtk_column_view_append_column(win->column_view, c);
    c = make_column("Path",        text_col_setup,  path_col_bind,  TRUE);
    gtk_column_view_append_column(win->column_view, c);
    c = make_column("Size",        text_col_setup,  size_col_bind,  FALSE);
    gtk_column_view_append_column(win->column_view, c);
    c = make_column("Type",        text_col_setup,  type_col_bind,  FALSE);
    gtk_column_view_append_column(win->column_view, c);
    c = make_column("Modified",    text_col_setup,  mtime_col_bind, FALSE);
    gtk_column_view_append_column(win->column_view, c);
    c = make_column("Attributes",  text_col_setup,  attrs_col_bind, FALSE);
    gtk_column_view_append_column(win->column_view, c);

    /* Menu (GMenu) */
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Rebuild index", "win.rebuild");
    g_menu_append(menu, "Export results as CSV…", "win.export-csv");
    GMenu *edit = g_menu_new();
    g_menu_append(edit, "Copy path",      "win.copy-path");
    g_menu_append(edit, "Copy name",      "win.copy-name");
    g_menu_append(edit, "Copy full name", "win.copy-full");
    g_menu_append(edit, "Open path",      "win.open-path");
    GMenu *root = g_menu_new();
    g_menu_append_submenu(root, "Tools", G_MENU_MODEL(menu));
    g_menu_append_submenu(root, "Edit",  G_MENU_MODEL(edit));
    gtk_menu_button_set_menu_model(win->menu_btn, G_MENU_MODEL(root));

    /* Install actions. */
    GSimpleActionGroup *grp = g_simple_action_group_new();
    GSimpleAction *act;
    act = g_simple_action_new("rebuild",    NULL); g_signal_connect(act, "activate", G_CALLBACK(action_rebuild),    win); g_action_map_add_action(G_ACTION_MAP(grp), G_ACTION(act));
    act = g_simple_action_new("export-csv", NULL); g_signal_connect(act, "activate", G_CALLBACK(action_export_csv), win); g_action_map_add_action(G_ACTION_MAP(grp), G_ACTION(act));
    act = g_simple_action_new("copy-path",  NULL); g_signal_connect(act, "activate", G_CALLBACK(action_copy_path),  win); g_action_map_add_action(G_ACTION_MAP(grp), G_ACTION(act));
    act = g_simple_action_new("copy-name",  NULL); g_signal_connect(act, "activate", G_CALLBACK(action_copy_name),  win); g_action_map_add_action(G_ACTION_MAP(grp), G_ACTION(act));
    act = g_simple_action_new("copy-full",  NULL); g_signal_connect(act, "activate", G_CALLBACK(action_copy_full),  win); g_action_map_add_action(G_ACTION_MAP(grp), G_ACTION(act));
    act = g_simple_action_new("open-path",  NULL); g_signal_connect(act, "activate", G_CALLBACK(action_open_path),  win); g_action_map_add_action(G_ACTION_MAP(grp), G_ACTION(act));
    gtk_widget_insert_action_group(GTK_WIDGET(win), "win", G_ACTION_GROUP(grp));

    /* Keyboard shortcut: Ctrl+R to rebuild. */
    GtkShortcutController *sc = GTK_SHORTCUT_CONTROLLER(
        gtk_shortcut_controller_new());
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(sc),
                                                GTK_PHASE_CAPTURE);
    GtkShortcut *s = gtk_shortcut_new(
        gtk_keyval_trigger_new(GDK_KEY_r, GDK_CONTROL_MASK),
        gtk_named_action_new("win.rebuild"));
    gtk_shortcut_controller_add_shortcut(sc, s);
    gtk_widget_add_controller(GTK_WIDGET(win), GTK_EVENT_CONTROLLER(sc));
}

static void
everything_window_constructed(GObject *obj)
{
    EverythingWindow *win = EVERYTHING_WINDOW(obj);
    G_OBJECT_CLASS(everything_window_parent_class)->constructed(obj);
    if (win->idx) {
        everything_index_set_state_cb(win->idx, on_index_state, win);
        everything_index_set_changed_cb(win->idx, on_index_changed, win);
        everything_index_set_progress_cb(win->idx, on_index_progress, win);
        everything_index_ensure(win->idx, NULL);
        schedule_search(win);
    }
}

static void
everything_window_set_property(GObject *obj, guint prop_id,
                               const GValue *val, GParamSpec *pspec)
{
    EverythingWindow *win = EVERYTHING_WINDOW(obj);
    switch (prop_id) {
        case PROP_INDEX:
            win->idx = (EverythingIndex *)g_value_get_pointer(val);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
everything_window_get_property(GObject *obj, guint prop_id,
                               GValue *val, GParamSpec *pspec)
{
    EverythingWindow *win = EVERYTHING_WINDOW(obj);
    switch (prop_id) {
        case PROP_INDEX:
            g_value_set_pointer(val, win->idx);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void
everything_window_dispose(GObject *obj)
{
    EverythingWindow *win = EVERYTHING_WINDOW(obj);
    if (win->search_debounce_id) {
        g_source_remove(win->search_debounce_id);
        win->search_debounce_id = 0;
    }
    /* GListStore owns its items (GObjects with their own finalize).
     * Clearing the store drops the refs and frees them. */
    g_clear_object(&win->results);
    gtk_widget_dispose_template(GTK_WIDGET(win), EVERYTHING_TYPE_WINDOW);
    G_OBJECT_CLASS(everything_window_parent_class)->dispose(obj);
}

static void
everything_window_class_init(EverythingWindowClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);

    oc->set_property = everything_window_set_property;
    oc->get_property = everything_window_get_property;
    oc->constructed  = everything_window_constructed;
    oc->dispose      = everything_window_dispose;

    props[PROP_INDEX] = g_param_spec_pointer("index", "Index",
        "EverythingIndex backend", G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
    g_object_class_install_properties(oc, N_PROPS, props);

    gtk_widget_class_set_template_from_resource(wc,
        "/org/voidtools/Everything/ui/everything-window.ui");

    gtk_widget_class_bind_template_child(wc, EverythingWindow, header_bar);
    gtk_widget_class_bind_template_child(wc, EverythingWindow, menu_btn);
    gtk_widget_class_bind_template_child(wc, EverythingWindow, search_entry);
    gtk_widget_class_bind_template_child(wc, EverythingWindow, rebuild_btn);
    gtk_widget_class_bind_template_child(wc, EverythingWindow, scroller);
    gtk_widget_class_bind_template_child(wc, EverythingWindow, column_view);
    gtk_widget_class_bind_template_child(wc, EverythingWindow, status_label);
    gtk_widget_class_bind_template_child(wc, EverythingWindow, count_label);

    gtk_widget_class_bind_template_callback(wc, on_search_changed);
    gtk_widget_class_bind_template_callback(wc, on_search_activate);
    gtk_widget_class_bind_template_callback(wc, on_rebuild_clicked);
    gtk_widget_class_bind_template_callback(wc, on_row_activated);
}

/* ------------- public API ------------- */

EverythingWindow *
everything_window_new(GtkApplication *app, EverythingIndex *idx)
{
    return g_object_new(EVERYTHING_TYPE_WINDOW,
                        "application", app,
                        "index",       idx,
                        NULL);
}

void
everything_window_refresh(EverythingWindow *win)
{
    g_return_if_fail(EVERYTHING_IS_WINDOW(win));
    schedule_search(win);
}

void
everything_window_rebuild_index(EverythingWindow *win)
{
    g_return_if_fail(EVERYTHING_IS_WINDOW(win));
    GError *err = NULL;
    if (!everything_index_rebuild(win->idx, &err)) {
        char *msg = g_strdup_printf("Rebuild failed: %s",
                                    err ? err->message : "?");
        gtk_label_set_text(win->status_label, msg);
        g_free(msg);
        g_clear_error(&err);
    }
}
