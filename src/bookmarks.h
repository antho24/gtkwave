#pragma once

#include <stdio.h>
#include <gtk/gtk.h>
#include <gtkwave.h>

/* A named time range that can be shown again later. */
typedef struct
{
    gchar *name;
    GwTime start;
    GwTime end;
} WaveBookmark;

guint bookmarks_get_count(void);
WaveBookmark *bookmarks_get(guint index);
void bookmarks_add(const gchar *name, GwTime start, GwTime end);
void bookmarks_remove(guint index);
void bookmarks_clear(void);
void bookmarks_go_to(guint index);

void bookmarks_write_save_file(FILE *wave);
void bookmarks_parse_save_file_line(const char *line);

void bookmarks_menu_init(GtkWidget *menu_item);

void menu_add_bookmark(gpointer null_data, guint callback_action, GtkWidget *widget);
void menu_manage_bookmarks(gpointer null_data, guint callback_action, GtkWidget *widget);
