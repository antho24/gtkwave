/*
 * Copyright (c) Tony Bybell 1999-2008
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "globals.h"
#include <config.h>
#include <gtk/gtk.h>
#include "symbol.h"
#include "lx2.h"
#include "gw-ghw-file.h"

/* Returns the full hierarchical name of T, NULL if it has none.  */
static const char *trace_full_name(GwTrace *t)
{
    /* analog traces span several rows, find the trace for this one */
    while (t != NULL && (t->flags & TR_ANALOG_BLANK_STRETCH)) {
        t = t->t_prev;
    }

    if (t == NULL) {
        return NULL;
    }

    if (HasWave(t)) {
        return t->vector ? t->n.vec->bvname : t->n.nd->nname;
    }

    /* groups, including VHDL records which aren't built yet */
    if (IsGroupBegin(t)) {
        return t->name;
    }

    return NULL;
}

/* Shows the full name of the trace T under the pointer at the bottom of the
   main window, or clears it if T is NULL.  */
void status_hover_trace(GwTrace *t)
{
    if (GLOBALS->hover_path_label == NULL) {
        return;
    }

    const char *name = trace_full_name(t);
    if (name == NULL) {
        name = "";
    }

    GtkLabel *label = GTK_LABEL(GLOBALS->hover_path_label);
    if (strcmp(gtk_label_get_text(label), name) != 0) {
        gtk_label_set_text(label, name);
    }
}

void status_text(const char *str)
{
    if (GLOBALS->quiet_checkmenu) {
        /* when gtkwave_mlist_t check menuitems are being initialized */
        return;
    }

    int len = strlen(str);
    char ch = len ? str[len - 1] : 0;

    fprintf(stderr, "GTKWAVE | %s%s", str, (ch == '\n') ? "" : "\n");
}
