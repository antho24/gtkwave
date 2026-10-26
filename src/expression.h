#pragma once

#include <stdio.h>
#include <gtk/gtk.h>
#include <gtkwave.h>

/*
 * Expression traces: signals computed from other signals, e.g. "valid and ready",
 * "cnt = 5", "data[7:4]" or "a - b".  The expression is stored in save files and evaluated
 * again when the save file is loaded.
 */

GwNode *expression_create_node(const char *text, GError **error);
gboolean expression_is_node(GwNode *nd);

gboolean expression_add_trace(const char *text, const char *alias, gboolean insert, GError **error);

void expression_write_save_file(FILE *wave, GwTrace *t);
char *expression_save_file_lines(GwTrace *t);
void expression_parse_save_file_line(const char *tag, const char *line);

void menu_insert_expression_trace(gpointer null_data, guint callback_action, GtkWidget *widget);
