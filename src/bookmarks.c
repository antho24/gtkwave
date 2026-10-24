#include <config.h>
#include "globals.h"
#include "bookmarks.h"
#include "currenttime.h"
#include "entry.h"
#include "zoombuttons.h"

#define BOOKMARK_MENU_ITEM_KEY "wave-bookmark-menu-item"

static void wave_bookmark_free(gpointer data)
{
    WaveBookmark *bookmark = data;

    g_free(bookmark->name);
    g_free(bookmark);
}

static GPtrArray *get_bookmarks(void)
{
    if (GLOBALS->bookmarks == NULL) {
        GLOBALS->bookmarks = g_ptr_array_new_with_free_func(wave_bookmark_free);
    }

    return GLOBALS->bookmarks;
}

guint bookmarks_get_count(void)
{
    return GLOBALS->bookmarks != NULL ? GLOBALS->bookmarks->len : 0;
}

WaveBookmark *bookmarks_get(guint index)
{
    if (index >= bookmarks_get_count()) {
        return NULL;
    }

    return g_ptr_array_index(GLOBALS->bookmarks, index);
}

void bookmarks_add(const gchar *name, GwTime start, GwTime end)
{
    WaveBookmark *bookmark = g_new0(WaveBookmark, 1);
    bookmark->name = g_strdup(name);
    bookmark->start = MIN(start, end);
    bookmark->end = MAX(start, end);

    g_ptr_array_add(get_bookmarks(), bookmark);
}

void bookmarks_remove(guint index)
{
    if (index < bookmarks_get_count()) {
        g_ptr_array_remove_index(GLOBALS->bookmarks, index);
    }
}

void bookmarks_clear(void)
{
    if (GLOBALS->bookmarks != NULL) {
        g_ptr_array_set_size(GLOBALS->bookmarks, 0);
    }
}

void bookmarks_go_to(guint index)
{
    WaveBookmark *bookmark = bookmarks_get(index);
    if (bookmark == NULL || GLOBALS->dump_file == NULL) {
        return;
    }

    service_zoom_to_range(bookmark->start, bookmark->end);
}

/* save file lines have the form "[bookmark] <start> <end> <name>" */
void bookmarks_write_save_file(FILE *wave)
{
    for (guint i = 0; i < bookmarks_get_count(); i++) {
        WaveBookmark *bookmark = bookmarks_get(i);

        fprintf(wave,
                "[bookmark] %" GW_TIME_FORMAT " %" GW_TIME_FORMAT " %s\n",
                bookmark->start,
                bookmark->end,
                bookmark->name);
    }
}

void bookmarks_parse_save_file_line(const char *line)
{
    GwTime start = 0;
    GwTime end = 0;
    int name_offset = 0;

    if (sscanf(line, " %" GW_TIME_FORMAT " %" GW_TIME_FORMAT " %n", &start, &end, &name_offset) <
            2 ||
        name_offset == 0) {
        return;
    }

    gchar *name = g_strstrip(g_strdup(line + name_offset));
    bookmarks_add(name, start, end);
    g_free(name);
}

static gchar *format_bookmark_time(GwTime time)
{
    char buf[64];

    GwTime global_time_offset = gw_dump_file_get_global_time_offset(GLOBALS->dump_file);
    GwTimeDimension time_dimension = gw_dump_file_get_time_dimension(GLOBALS->dump_file);
    reformat_time(buf, time + global_time_offset, time_dimension);

    return g_strdup(buf);
}

static gchar *format_bookmark_range(WaveBookmark *bookmark)
{
    gchar *start = format_bookmark_time(bookmark->start);
    gchar *end = format_bookmark_time(bookmark->end);
    gchar *range = g_strdup_printf("%s \xe2\x80\x93 %s", start, end);

    g_free(start);
    g_free(end);

    return range;
}

/* the bookmarks submenu */

static void on_bookmark_menu_item_activate(GtkMenuItem *menu_item, gpointer user_data)
{
    (void)menu_item;

    bookmarks_go_to(GPOINTER_TO_UINT(user_data));
}

static void remove_bookmark_menu_item(GtkWidget *widget, gpointer user_data)
{
    (void)user_data;

    if (g_object_get_data(G_OBJECT(widget), BOOKMARK_MENU_ITEM_KEY) != NULL) {
        gtk_widget_destroy(widget);
    }
}

static void on_bookmarks_menu_show(GtkWidget *menu, gpointer user_data)
{
    (void)user_data;

    gtk_container_foreach(GTK_CONTAINER(menu), remove_bookmark_menu_item, NULL);

    if (bookmarks_get_count() == 0 || GLOBALS->dump_file == NULL) {
        return;
    }

    GtkWidget *separator = gtk_separator_menu_item_new();
    g_object_set_data(G_OBJECT(separator), BOOKMARK_MENU_ITEM_KEY, GINT_TO_POINTER(1));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), separator);
    gtk_widget_show(separator);

    for (guint i = 0; i < bookmarks_get_count(); i++) {
        WaveBookmark *bookmark = bookmarks_get(i);

        gchar *range = format_bookmark_range(bookmark);
        gchar *label = g_strdup_printf("%s  (%s)", bookmark->name, range);

        GtkWidget *item = gtk_menu_item_new_with_label(label);
        g_object_set_data(G_OBJECT(item), BOOKMARK_MENU_ITEM_KEY, GINT_TO_POINTER(1));
        g_signal_connect(item,
                         "activate",
                         G_CALLBACK(on_bookmark_menu_item_activate),
                         GUINT_TO_POINTER(i));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        gtk_widget_show(item);

        g_free(label);
        g_free(range);
    }
}

/* adds the bookmarks to the submenu that contains menu_item each time it's shown */
void bookmarks_menu_init(GtkWidget *menu_item)
{
    GtkWidget *menu = gtk_widget_get_parent(menu_item);
    if (GTK_IS_MENU(menu)) {
        g_signal_connect(menu, "show", G_CALLBACK(on_bookmarks_menu_show), NULL);
    }
}

/* Time/Bookmarks/Add Bookmark */

static void add_bookmark_cleanup(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    if (GLOBALS->entrybox_text == NULL) {
        return;
    }

    gchar *name = g_strstrip(g_strdup(GLOBALS->entrybox_text));
    free_2(GLOBALS->entrybox_text);
    GLOBALS->entrybox_text = NULL;

    if (name[0] != '\0') {
        GwTime end = MIN(GLOBALS->tims.end, GLOBALS->tims.last);
        bookmarks_add(name, GLOBALS->tims.start, end);
    }

    g_free(name);
}

void menu_add_bookmark(gpointer null_data, guint callback_action, GtkWidget *widget)
{
    (void)null_data;
    (void)callback_action;
    (void)widget;

    if (GLOBALS->dump_file == NULL) {
        return;
    }

    gchar *name = g_strdup_printf("Bookmark %u", bookmarks_get_count() + 1);
    entrybox("Add Bookmark", 300, name, NULL, 128, G_CALLBACK(add_bookmark_cleanup));
    g_free(name);
}

/* Time/Bookmarks/Manage Bookmarks */

enum
{
    COLUMN_NAME,
    COLUMN_RANGE,
    N_COLUMNS,
};

enum
{
    RESPONSE_GO_TO = 1,
    RESPONSE_DELETE,
};

static void fill_bookmark_store(GtkListStore *store)
{
    gtk_list_store_clear(store);

    for (guint i = 0; i < bookmarks_get_count(); i++) {
        WaveBookmark *bookmark = bookmarks_get(i);
        gchar *range = format_bookmark_range(bookmark);

        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, COLUMN_NAME, bookmark->name, COLUMN_RANGE, range, -1);

        g_free(range);
    }
}

static gint get_selected_bookmark(GtkTreeView *view)
{
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;

    GtkTreeSelection *selection = gtk_tree_view_get_selection(view);
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        return -1;
    }

    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
    gint index = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);

    return index;
}

static void on_bookmark_name_edited(GtkCellRendererText *renderer,
                                    gchar *path,
                                    gchar *new_text,
                                    gpointer user_data)
{
    (void)renderer;

    GtkListStore *store = user_data;
    WaveBookmark *bookmark = bookmarks_get(atoi(path));
    gchar *name = g_strstrip(g_strdup(new_text));

    if (bookmark != NULL && name[0] != '\0') {
        g_free(bookmark->name);
        bookmark->name = g_strdup(name);
        fill_bookmark_store(store);
    }

    g_free(name);
}

static void on_bookmark_row_activated(GtkTreeView *view,
                                      GtkTreePath *path,
                                      GtkTreeViewColumn *column,
                                      gpointer user_data)
{
    (void)view;
    (void)column;
    (void)user_data;

    bookmarks_go_to(gtk_tree_path_get_indices(path)[0]);
}

static void on_bookmark_selection_changed(GtkTreeSelection *selection, gpointer user_data)
{
    GtkDialog *dialog = user_data;
    gboolean selected = gtk_tree_selection_get_selected(selection, NULL, NULL);

    gtk_dialog_set_response_sensitive(dialog, RESPONSE_GO_TO, selected);
    gtk_dialog_set_response_sensitive(dialog, RESPONSE_DELETE, selected);
}

void menu_manage_bookmarks(gpointer null_data, guint callback_action, GtkWidget *widget)
{
    (void)null_data;
    (void)callback_action;
    (void)widget;

    if (GLOBALS->dump_file == NULL) {
        return;
    }

    GtkWidget *dialog =
        gtk_dialog_new_with_buttons("Bookmarks",
                                    GTK_WINDOW(GLOBALS->mainwindow),
                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                    "Delete",
                                    RESPONSE_DELETE,
                                    "Go To",
                                    RESPONSE_GO_TO,
                                    "Close",
                                    GTK_RESPONSE_CLOSE,
                                    NULL);
    gtk_container_set_border_width(GTK_CONTAINER(dialog), 6);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, 300);

    GtkListStore *store = gtk_list_store_new(N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING);
    fill_bookmark_store(store);

    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));

    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "editable", TRUE, NULL);
    g_signal_connect(renderer, "edited", G_CALLBACK(on_bookmark_name_edited), store);
    GtkTreeViewColumn *column =
        gtk_tree_view_column_new_with_attributes("Name", renderer, "text", COLUMN_NAME, NULL);
    gtk_tree_view_column_set_expand(column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);

    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Time range",
                                                      renderer,
                                                      "text",
                                                      COLUMN_RANGE,
                                                      NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);

    g_signal_connect(view, "row-activated", G_CALLBACK(on_bookmark_row_activated), NULL);

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    g_signal_connect(selection, "changed", G_CALLBACK(on_bookmark_selection_changed), dialog);
    on_bookmark_selection_changed(selection, dialog);

    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled_window), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled_window, TRUE);
    gtk_container_add(GTK_CONTAINER(scrolled_window), view);

    GtkWidget *hint =
        gtk_label_new("Double-click a bookmark to show its time range, or its name to rename it.\n"
                      "Add bookmarks with Time \xe2\x86\x92 Bookmarks \xe2\x86\x92 Add Bookmark.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_widget_set_margin_top(hint, 6);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_pack_start(GTK_BOX(content), scrolled_window, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), hint, FALSE, FALSE, 0);
    gtk_widget_show_all(content);

    for (;;) {
        gint response = gtk_dialog_run(GTK_DIALOG(dialog));
        gint index = get_selected_bookmark(GTK_TREE_VIEW(view));

        if (response == RESPONSE_GO_TO) {
            if (index >= 0) {
                bookmarks_go_to(index);
            }
        } else if (response == RESPONSE_DELETE) {
            if (index >= 0) {
                bookmarks_remove(index);
                fill_bookmark_store(store);
            }
        } else {
            break;
        }
    }

    gtk_widget_destroy(dialog);
    g_object_unref(store);
}
