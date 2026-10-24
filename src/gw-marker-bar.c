#include <config.h>
#include "gw-marker-bar.h"
#include "globals.h"
#include "analyzer.h"
#include "bookmarks.h"
#include "bsearch.h"
#include "currenttime.h"
#include "entry.h"
#include "menu.h"
#include "signal_list.h"
#include "wavewindow.h"

/*
 * GwMarkerBar:
 *
 * A bar below the waves that shows the time of each marker and the time between neighboring
 * markers. Markers can be dragged in the bar to move them, unless they are locked.
 */

#define TAG_PADDING 3
#define LOCK_ICON_WIDTH 8
#define HIT_DISTANCE 4
#define ARROW_SIZE 4

typedef enum
{
    MARKER_KIND_PRIMARY,
    MARKER_KIND_BASELINE,
    MARKER_KIND_NAMED,
} MarkerKind;

typedef struct
{
    GwMarker *marker;
    MarkerKind kind;
    GwTime position;
    gdouble x;

    // the tag drawn in the last draw, has_tag is FALSE if there was no space for it
    gboolean has_tag;
    gint tag_x;
    gint tag_width;
} MarkerBarItem;

struct _GwMarkerBar
{
    GtkDrawingArea parent_instance;

    GArray *items;
    GwMarker *drag_marker;
    gboolean drag_moved;
};

G_DEFINE_TYPE(GwMarkerBar, gw_marker_bar, GTK_TYPE_DRAWING_AREA)

static GwMarker *rename_marker = NULL;

static gboolean have_dump_file(void)
{
    return GLOBALS->project != NULL && GLOBALS->dump_file != NULL;
}

static gdouble time_to_x(GwTime time)
{
    gdouble pixstep = ((gdouble)GLOBALS->nsperframe) / ((gdouble)GLOBALS->pixelsperframe);

    return ((gdouble)(time - GLOBALS->tims.start)) / pixstep;
}

static GwTime x_to_time(gdouble x)
{
    gdouble pixstep = ((gdouble)GLOBALS->nsperframe) / ((gdouble)GLOBALS->pixelsperframe);
    gdouble offset = ((gdouble)GLOBALS->tims.start) + (x * pixstep);
    GwTime time = (GwTime)offset;

    if (offset - time > 0.5) {
        time++;
    }

    time = MIN(time, GLOBALS->tims.end);
    time = MIN(time, GLOBALS->tims.last);
    time = MAX(time, GLOBALS->tims.start);
    time = MAX(time, GLOBALS->tims.first);

    return time_trunc(time);
}

static gchar *format_time(GwTime time, gboolean is_delta)
{
    char buf[64];

    GwTimeDimension time_dimension = gw_dump_file_get_time_dimension(GLOBALS->dump_file);
    if (!is_delta) {
        time += gw_dump_file_get_global_time_offset(GLOBALS->dump_file);
    }
    reformat_time(buf, time, time_dimension);

    return g_strdup(buf);
}

static GwColor get_marker_color(MarkerBarItem *item, GwWaveformColors *colors)
{
    switch (item->kind) {
        case MARKER_KIND_PRIMARY:
            return colors->marker_primary;
        case MARKER_KIND_BASELINE:
            return colors->marker_baseline;
        default:
            return colors->marker_named;
    }
}

static const gchar *get_marker_name(MarkerBarItem *item)
{
    switch (item->kind) {
        case MARKER_KIND_PRIMARY:
            return "Primary";
        case MARKER_KIND_BASELINE:
            return "Baseline";
        default:
            return gw_marker_get_display_name(item->marker);
    }
}

static void add_item(GwMarkerBar *self, GwMarker *marker, MarkerKind kind)
{
    if (!gw_marker_is_enabled(marker)) {
        return;
    }

    MarkerBarItem item = {
        .marker = marker,
        .kind = kind,
        .position = gw_marker_get_position(marker),
    };
    item.x = time_to_x(item.position);

    g_array_append_val(self->items, item);
}

static void add_named_item(gpointer data, gpointer user_data)
{
    add_item(user_data, data, MARKER_KIND_NAMED);
}

static gint compare_items(gconstpointer a, gconstpointer b)
{
    const MarkerBarItem *item_a = a;
    const MarkerBarItem *item_b = b;

    if (item_a->position != item_b->position) {
        return item_a->position < item_b->position ? -1 : 1;
    }

    return (gint)item_a->kind - (gint)item_b->kind;
}

/* collects the enabled markers sorted by time */
static void update_items(GwMarkerBar *self)
{
    g_array_set_size(self->items, 0);

    if (!have_dump_file()) {
        return;
    }

    add_item(self, gw_project_get_primary_marker(GLOBALS->project), MARKER_KIND_PRIMARY);
    add_item(self, gw_project_get_baseline_marker(GLOBALS->project), MARKER_KIND_BASELINE);
    gw_named_markers_foreach(gw_project_get_named_markers(GLOBALS->project), add_named_item, self);

    g_array_sort(self->items, compare_items);
}

static gint get_row_height(void)
{
    struct font_engine_font_t *font = GLOBALS->wavefont;

    return font->ascent + font->descent + 2;
}

static void draw_lock_icon(cairo_t *cr, GwColor color, gdouble x, gdouble y)
{
    XXX_gdk_set_color(cr, color);

    cairo_rectangle(cr, x, y + 3.0, 7.0, 5.0);
    cairo_fill(cr);

    cairo_set_line_width(cr, 1.2);
    cairo_arc(cr, x + 3.5, y + 3.5, 2.3, G_PI, 2.0 * G_PI);
    cairo_stroke(cr);
}

static void draw_arrow_head(cairo_t *cr, GwColor color, gdouble x, gdouble y, gdouble direction)
{
    XXX_gdk_set_color(cr, color);

    cairo_move_to(cr, x, y);
    cairo_line_to(cr, x + direction * ARROW_SIZE, y - ARROW_SIZE / 2.0);
    cairo_line_to(cr, x + direction * ARROW_SIZE, y + ARROW_SIZE / 2.0);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static gboolean tag_overlaps(GwMarkerBar *self, gint tag_x, gint tag_width)
{
    for (guint i = 0; i < self->items->len; i++) {
        MarkerBarItem *item = &g_array_index(self->items, MarkerBarItem, i);
        if (item->has_tag && tag_x < item->tag_x + item->tag_width + 2 &&
            item->tag_x < tag_x + tag_width + 2) {
            return TRUE;
        }
    }

    return FALSE;
}

static void draw_tag(GwMarkerBar *self,
                     cairo_t *cr,
                     GwWaveformColors *colors,
                     gint width,
                     MarkerBarItem *item)
{
    struct font_engine_font_t *font = GLOBALS->wavefont;
    gint row_height = get_row_height();
    gboolean locked = gw_marker_is_locked(item->marker);
    gint icon_width = locked ? LOCK_ICON_WIDTH + 2 : 0;

    // try the name and time first, then only the name
    gchar *time = format_time(item->position, FALSE);
    gchar *labels[] = {
        g_strdup_printf("%s %s", get_marker_name(item), time),
        g_strdup(get_marker_name(item)),
    };
    g_free(time);

    for (guint l = 0; l < G_N_ELEMENTS(labels); l++) {
        gint text_width = font_engine_string_measure(font, labels[l]);
        gint tag_width = text_width + icon_width + 2 * TAG_PADDING;
        gint tag_x = (gint)item->x - tag_width / 2;
        tag_x = CLAMP(tag_x, 0, MAX(width - tag_width, 0));

        if (tag_overlaps(self, tag_x, tag_width)) {
            continue;
        }

        GwColor color = get_marker_color(item, colors);
        XXX_gdk_draw_rectangle(cr, color, TRUE, tag_x, 1, tag_width, row_height);
        if (locked) {
            draw_lock_icon(cr, colors->timebar_background, tag_x + TAG_PADDING, 2 + 1);
        }
        XXX_font_engine_draw_string(cr,
                                    font,
                                    &colors->timebar_background,
                                    tag_x + TAG_PADDING + icon_width,
                                    1 + font->ascent,
                                    labels[l]);

        item->has_tag = TRUE;
        item->tag_x = tag_x;
        item->tag_width = tag_width;
        break;
    }

    g_free(labels[0]);
    g_free(labels[1]);
}

/* draws the tags of the named markers first, the primary and baseline markers are already shown
 * in the time display above the waves and only get a tag if there is space left */
static void draw_tags(GwMarkerBar *self, cairo_t *cr, GwWaveformColors *colors, gint width)
{
    for (guint i = 0; i < self->items->len; i++) {
        g_array_index(self->items, MarkerBarItem, i).has_tag = FALSE;
    }

    for (gint pass = 0; pass < 2; pass++) {
        for (guint i = 0; i < self->items->len; i++) {
            MarkerBarItem *item = &g_array_index(self->items, MarkerBarItem, i);

            if ((item->kind == MARKER_KIND_NAMED) != (pass == 0) || item->x < 0 ||
                item->x >= width) {
                continue;
            }

            draw_tag(self, cr, colors, width, item);
        }
    }
}

/* draws the time between each pair of neighboring markers */
static void draw_deltas(GwMarkerBar *self, cairo_t *cr, GwWaveformColors *colors, gint width)
{
    struct font_engine_font_t *font = GLOBALS->wavefont;
    gint row_height = get_row_height();
    gint row_y = row_height + 3;
    gdouble line_y = row_y + row_height / 2.0;

    for (guint i = 0; i + 1 < self->items->len; i++) {
        MarkerBarItem *a = &g_array_index(self->items, MarkerBarItem, i);
        MarkerBarItem *b = &g_array_index(self->items, MarkerBarItem, i + 1);

        if (a->position == b->position || b->x < 0 || a->x >= width) {
            continue;
        }

        gdouble x0 = MAX(a->x, 0.0);
        gdouble x1 = MIN(b->x, (gdouble)width - 1);

        XXX_gdk_draw_line(cr, colors->timebar_text, x0, line_y, x1, line_y);
        if (a->x >= 0 && x1 - x0 > 2 * ARROW_SIZE) {
            draw_arrow_head(cr, colors->timebar_text, a->x + 1, line_y, 1.0);
        }
        if (b->x < width && x1 - x0 > 2 * ARROW_SIZE) {
            draw_arrow_head(cr, colors->timebar_text, b->x, line_y, -1.0);
        }

        gchar *delta = format_time(b->position - a->position, TRUE);
        gint text_width = font_engine_string_measure(font, delta);

        if (text_width + 2 * TAG_PADDING + 2 * ARROW_SIZE <= x1 - x0) {
            gint text_x = (gint)((x0 + x1) / 2.0) - text_width / 2;

            XXX_gdk_draw_rectangle(cr,
                                   colors->timebar_background,
                                   TRUE,
                                   text_x - TAG_PADDING,
                                   row_y,
                                   text_width + 2 * TAG_PADDING,
                                   row_height);
            XXX_font_engine_draw_string(cr,
                                        font,
                                        &colors->timebar_text,
                                        text_x,
                                        row_y + font->ascent,
                                        delta);
        }

        g_free(delta);
    }
}

static gboolean gw_marker_bar_draw(GtkWidget *widget, cairo_t *cr)
{
    GwMarkerBar *self = GW_MARKER_BAR(widget);

    gint page_num = gtk_notebook_get_current_page(GTK_NOTEBOOK(GLOBALS->notebook));
    set_GLOBALS((*GLOBALS->contexts)[page_num]);

    GwWaveformColors *colors = gw_color_theme_get_waveform_colors(GLOBALS->color_theme);
    if (GLOBALS->black_and_white) {
        colors = gw_waveform_colors_new_black_and_white();
    }

    gint width = gtk_widget_get_allocated_width(widget);
    gint height = gtk_widget_get_allocated_height(widget);

    XXX_gdk_draw_rectangle(cr, colors->timebar_background, TRUE, 0, 0, width, height);
    XXX_gdk_draw_line(cr, colors->grid, 0, 0, width, 0);

    update_items(self);

    if (GLOBALS->wavefont != NULL) {
        cairo_set_line_width(cr, GLOBALS->cr_line_width);

        for (guint i = 0; i < self->items->len; i++) {
            MarkerBarItem *item = &g_array_index(self->items, MarkerBarItem, i);
            if (item->x >= 0 && item->x < width) {
                XXX_gdk_draw_line(cr, get_marker_color(item, colors), item->x, 0, item->x, height);
            }
        }

        draw_deltas(self, cr, colors, width);
        draw_tags(self, cr, colors, width);
    }

    if (GLOBALS->black_and_white) {
        g_free(colors);
    }

    return FALSE;
}

/* returns the marker under the pointer, preferring tags over marker lines */
static MarkerBarItem *find_item(GwMarkerBar *self, gdouble x, gdouble y)
{
    if (y <= get_row_height() + 1) {
        for (guint i = 0; i < self->items->len; i++) {
            MarkerBarItem *item = &g_array_index(self->items, MarkerBarItem, i);
            if (item->has_tag && x >= item->tag_x && x < item->tag_x + item->tag_width) {
                return item;
            }
        }
    }

    MarkerBarItem *closest = NULL;
    for (guint i = 0; i < self->items->len; i++) {
        MarkerBarItem *item = &g_array_index(self->items, MarkerBarItem, i);
        gdouble distance = ABS(item->x - x);
        if (distance <= HIT_DISTANCE && (closest == NULL || distance < ABS(closest->x - x))) {
            closest = item;
        }
    }

    return closest;
}

static void redraw_markers(void)
{
    update_time_box();
    gtk_widget_queue_draw(GLOBALS->wavearea);
    gtk_widget_queue_draw(GLOBALS->marker_bar);
}

static void move_marker(GwMarker *marker, GwTime time)
{
    gw_marker_set_position(marker, time);
    gw_marker_set_enabled(marker, TRUE);

    redraw_markers();
}

typedef struct
{
    gdouble x;
    GwTime time;
    gdouble distance;
} SnapCandidate;

static void consider_snap_time(SnapCandidate *snap, GwTime time)
{
    if (time < GLOBALS->tims.start || time > GLOBALS->tims.end || time > GLOBALS->tims.last) {
        return;
    }

    gdouble distance = ABS(time_to_x(time) - snap->x);
    if (distance < snap->distance) {
        snap->time = time;
        snap->distance = distance;
    }
}

static void consider_snap_marker(SnapCandidate *snap, GwMarker *marker, GwMarker *ignore)
{
    if (marker != ignore && gw_marker_is_enabled(marker)) {
        consider_snap_time(snap, gw_marker_get_position(marker));
    }
}

/* Snaps the time at x to the closest edge of the visible traces or, if snap_to_markers is set, to
 * the closest marker other than ignore. A click in the waves snaps to the trace under the pointer
 * instead, both use the cursor_snap distance. */
static GwTime snap_time(GwTime time, gdouble x, gboolean snap_to_markers, GwMarker *ignore)
{
    if (GLOBALS->cursor_snap <= 0) {
        return time;
    }

    SnapCandidate snap = {.x = x, .time = time, .distance = GLOBALS->cursor_snap + 0.5};

    GwSignalList *signal_list = GW_SIGNAL_LIST(GLOBALS->signalarea);
    gint num_traces = gw_signal_list_get_num_traces_displayable(signal_list);
    GwTrace *t = gw_signal_list_get_trace(signal_list, 0);

    for (gint i = 0; t != NULL && i < num_traces; i++, t = GiveNextTrace(t)) {
        if (t->flags & (TR_BLANK | TR_EXCLUDE | TR_ANALOG_BLANK_STRETCH)) {
            continue;
        }

        // the edges before and after the time
        if (t->vector) {
            GwVectorEnt *v = bsearch_vector(t->n.vec, time - t->shift);
            if (v != NULL) {
                consider_snap_time(&snap, v->time + t->shift);
                if (v->next != NULL) {
                    consider_snap_time(&snap, v->next->time + t->shift);
                }
            }
        } else {
            GwHistEnt *h = bsearch_node(t->n.nd, time - t->shift);
            if (h != NULL) {
                consider_snap_time(&snap, h->time + t->shift);
                if (h->next != NULL) {
                    consider_snap_time(&snap, h->next->time + t->shift);
                }
            }
        }
    }

    if (snap_to_markers) {
        consider_snap_marker(&snap, gw_project_get_primary_marker(GLOBALS->project), ignore);
        consider_snap_marker(&snap, gw_project_get_baseline_marker(GLOBALS->project), ignore);

        GwNamedMarkers *markers = gw_project_get_named_markers(GLOBALS->project);
        for (guint i = 0; i < gw_named_markers_get_number_of_markers(markers); i++) {
            consider_snap_marker(&snap, gw_named_markers_get(markers, i), ignore);
        }
    }

    return snap.time;
}

static gboolean is_primary_marker(GwMarker *marker)
{
    return marker == gw_project_get_primary_marker(GLOBALS->project);
}

static void add_named_marker_at(GwTime time)
{
    GwNamedMarkers *markers = gw_project_get_named_markers(GLOBALS->project);
    if (gw_named_markers_find(markers, time) != NULL) {
        return;
    }

    GwMarker *marker = gw_named_markers_find_first_disabled(markers);
    if (marker != NULL) {
        move_marker(marker, time);
    }
}

/* context menu */

static void on_lock_toggled(GtkCheckMenuItem *menu_item, gpointer user_data)
{
    gw_marker_set_locked(GW_MARKER(user_data), gtk_check_menu_item_get_active(menu_item));
    redraw_markers();
}

static void rename_marker_cleanup(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    if (rename_marker != NULL) {
        // an empty name restores the default name
        gw_marker_set_alias(rename_marker, GLOBALS->entrybox_text);
        rename_marker = NULL;
        redraw_markers();
    }

    if (GLOBALS->entrybox_text != NULL) {
        free_2(GLOBALS->entrybox_text);
        GLOBALS->entrybox_text = NULL;
    }
}

static void on_rename_activate(GtkMenuItem *menu_item, gpointer user_data)
{
    (void)menu_item;

    GwMarker *marker = GW_MARKER(user_data);
    const gchar *alias = gw_marker_get_alias(marker);
    gchar *title = g_strdup_printf("Rename Marker %s", gw_marker_get_name(marker));

    rename_marker = marker;
    entrybox(title, 300, alias != NULL ? alias : "", NULL, 48, G_CALLBACK(rename_marker_cleanup));
    rename_marker = NULL;

    g_free(title);
}

static void on_delete_activate(GtkMenuItem *menu_item, gpointer user_data)
{
    (void)menu_item;

    GwMarker *marker = GW_MARKER(user_data);

    if (is_primary_marker(marker)) {
        delete_unnamed_marker(NULL, 0, NULL);
    } else {
        gw_marker_set_enabled(marker, FALSE);
        if (marker != gw_project_get_baseline_marker(GLOBALS->project)) {
            gw_marker_set_alias(marker, NULL);
            gw_marker_set_locked(marker, FALSE);
        }
    }

    redraw_markers();
}

static void on_move_primary_activate(GtkMenuItem *menu_item, gpointer user_data)
{
    (void)menu_item;

    GwMarker *marker = GW_MARKER(user_data);
    move_marker(gw_project_get_primary_marker(GLOBALS->project), gw_marker_get_position(marker));
    button_press_release_common();
}

static void on_add_named_activate(GtkMenuItem *menu_item, gpointer user_data)
{
    (void)menu_item;

    add_named_marker_at((GwTime)GPOINTER_TO_SIZE(user_data));
}

static void on_add_bookmark_activate(GtkMenuItem *menu_item, gpointer user_data)
{
    (void)menu_item;
    (void)user_data;

    menu_add_bookmark(NULL, 0, NULL);
}

static GtkWidget *append_menu_item(GtkWidget *menu,
                                   const gchar *label,
                                   GCallback callback,
                                   gpointer user_data)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_signal_connect(item, "activate", callback, user_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    return item;
}

static void popup_menu(GwMarkerBar *self, GdkEventButton *event, MarkerBarItem *item)
{
    (void)self;

    GtkWidget *menu = gtk_menu_new();

    if (item != NULL && item->kind == MARKER_KIND_NAMED) {
        GwMarker *marker = item->marker;
        gboolean locked = gw_marker_is_locked(marker);

        GtkWidget *lock = gtk_check_menu_item_new_with_label("Locked");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(lock), locked);
        g_signal_connect(lock, "toggled", G_CALLBACK(on_lock_toggled), marker);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), lock);

        append_menu_item(menu, "Rename...", G_CALLBACK(on_rename_activate), marker);
        append_menu_item(menu,
                         "Move Primary Marker Here",
                         G_CALLBACK(on_move_primary_activate),
                         marker);
        GtkWidget *delete =
            append_menu_item(menu, "Delete Marker", G_CALLBACK(on_delete_activate), marker);
        gtk_widget_set_sensitive(delete, !locked);
    } else if (item != NULL && item->kind == MARKER_KIND_PRIMARY) {
        append_menu_item(menu, "Drop Named Marker Here", G_CALLBACK(drop_named_marker), NULL);
        append_menu_item(menu,
                         "Delete Primary Marker",
                         G_CALLBACK(on_delete_activate),
                         item->marker);
    } else if (item != NULL && item->kind == MARKER_KIND_BASELINE) {
        append_menu_item(menu,
                         "Move Primary Marker Here",
                         G_CALLBACK(on_move_primary_activate),
                         item->marker);
        append_menu_item(menu,
                         "Delete Baseline Marker",
                         G_CALLBACK(on_delete_activate),
                         item->marker);
    } else {
        GwTime time = x_to_time(event->x);
        append_menu_item(menu,
                         "Add Named Marker Here",
                         G_CALLBACK(on_add_named_activate),
                         GSIZE_TO_POINTER((gsize)time));
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    append_menu_item(menu, "Bookmark This View...", G_CALLBACK(on_add_bookmark_activate), NULL);

    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(self), NULL);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

/* mouse handling */

static gboolean gw_marker_bar_button_press_event(GtkWidget *widget, GdkEventButton *event)
{
    GwMarkerBar *self = GW_MARKER_BAR(widget);

    if (!have_dump_file()) {
        return GDK_EVENT_PROPAGATE;
    }

    update_items(self);
    MarkerBarItem *item = find_item(self, event->x, event->y);

    if (event->button == 1 && event->type == GDK_2BUTTON_PRESS) {
        if (item == NULL) {
            GwTime time = x_to_time(event->x);
            if (!(event->state & GDK_SHIFT_MASK)) {
                time = snap_time(time, event->x, FALSE, NULL);
            }
            add_named_marker_at(time);
        }
    } else if (event->button == 1 && event->type == GDK_BUTTON_PRESS) {
        if (item != NULL && !gw_marker_is_locked(item->marker)) {
            self->drag_marker = item->marker;
            self->drag_moved = FALSE;

            // a named marker that is dragged away from the primary marker no longer follows it
            GwNamedMarkers *markers = gw_project_get_named_markers(GLOBALS->project);
            if (GLOBALS->named_marker_lock_idx >= 0 &&
                gw_named_markers_get(markers, GLOBALS->named_marker_lock_idx) == item->marker) {
                GLOBALS->named_marker_lock_idx = -1;
            }
        }
    } else if (event->button == 3 && event->type == GDK_BUTTON_PRESS) {
        popup_menu(self, event, item);
    }

    return GDK_EVENT_STOP;
}

static void update_pointer_cursor(GwMarkerBar *self, gdouble x, gdouble y)
{
    GdkWindow *window = gtk_widget_get_window(GTK_WIDGET(self));
    if (window == NULL) {
        return;
    }

    MarkerBarItem *item = self->drag_marker == NULL ? find_item(self, x, y) : NULL;
    gboolean draggable =
        self->drag_marker != NULL || (item != NULL && !gw_marker_is_locked(item->marker));

    GdkCursor *cursor = NULL;
    if (draggable) {
        cursor = gdk_cursor_new_from_name(gdk_window_get_display(window), "ew-resize");
    }
    gdk_window_set_cursor(window, cursor);
    g_clear_object(&cursor);
}

static gboolean gw_marker_bar_motion_notify_event(GtkWidget *widget, GdkEventMotion *event)
{
    GwMarkerBar *self = GW_MARKER_BAR(widget);

    if (!have_dump_file()) {
        return GDK_EVENT_PROPAGATE;
    }

    if (self->drag_marker != NULL) {
        // Shift moves the marker without snapping
        GwTime time = x_to_time(event->x);
        if (!(event->state & GDK_SHIFT_MASK)) {
            time = snap_time(time, event->x, TRUE, self->drag_marker);
        }

        if (time != gw_marker_get_position(self->drag_marker)) {
            self->drag_moved = TRUE;
            move_marker(self->drag_marker, time);
        }
    }

    update_pointer_cursor(self, event->x, event->y);
    gdk_event_request_motions(event);

    return GDK_EVENT_STOP;
}

static gboolean gw_marker_bar_button_release_event(GtkWidget *widget, GdkEventButton *event)
{
    GwMarkerBar *self = GW_MARKER_BAR(widget);

    if (event->button == 1 && self->drag_marker != NULL) {
        if (self->drag_moved && is_primary_marker(self->drag_marker)) {
            button_press_release_common();
        }
        self->drag_marker = NULL;
    }

    return GDK_EVENT_STOP;
}

static gboolean gw_marker_bar_query_tooltip(GtkWidget *widget,
                                            gint x,
                                            gint y,
                                            gboolean keyboard_mode,
                                            GtkTooltip *tooltip)
{
    GwMarkerBar *self = GW_MARKER_BAR(widget);
    (void)keyboard_mode;

    if (!have_dump_file() || self->drag_marker != NULL) {
        return FALSE;
    }

    MarkerBarItem *item = find_item(self, x, y);
    if (item == NULL) {
        gtk_tooltip_set_text(tooltip,
                             "Drag a marker to move it, double-click to add a named marker,\n"
                             "right-click for more options. Markers snap to the nearest edge,\n"
                             "hold Shift to place them freely.");
        return TRUE;
    }

    GString *text = g_string_new(NULL);
    gchar *time = format_time(item->position, FALSE);
    g_string_append_printf(text, "%s: %s", get_marker_name(item), time);
    g_free(time);

    if (gw_marker_is_locked(item->marker)) {
        g_string_append(text, " (locked)");
    }

    for (guint i = 0; i < self->items->len; i++) {
        MarkerBarItem *other = &g_array_index(self->items, MarkerBarItem, i);
        if (other == item) {
            continue;
        }

        GwTime delta = other->position - item->position;
        gchar *delta_str = format_time(delta, TRUE);
        g_string_append_printf(text,
                               "\nto %s: %s%s",
                               get_marker_name(other),
                               delta > 0 ? "+" : "",
                               delta_str);
        g_free(delta_str);
    }

    gtk_tooltip_set_text(tooltip, text->str);
    g_string_free(text, TRUE);

    return TRUE;
}

static void gw_marker_bar_get_preferred_height(GtkWidget *widget,
                                               gint *minimum_height,
                                               gint *natural_height)
{
    (void)widget;

    gint height = 24;
    if (GLOBALS->wavefont != NULL) {
        height = 2 * get_row_height() + 5;
    }

    *minimum_height = height;
    *natural_height = height;
}

static void gw_marker_bar_finalize(GObject *object)
{
    GwMarkerBar *self = GW_MARKER_BAR(object);

    g_array_free(self->items, TRUE);

    G_OBJECT_CLASS(gw_marker_bar_parent_class)->finalize(object);
}

static void gw_marker_bar_class_init(GwMarkerBarClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->finalize = gw_marker_bar_finalize;

    widget_class->draw = gw_marker_bar_draw;
    widget_class->button_press_event = gw_marker_bar_button_press_event;
    widget_class->button_release_event = gw_marker_bar_button_release_event;
    widget_class->motion_notify_event = gw_marker_bar_motion_notify_event;
    widget_class->query_tooltip = gw_marker_bar_query_tooltip;
    widget_class->get_preferred_height = gw_marker_bar_get_preferred_height;
}

static void gw_marker_bar_init(GwMarkerBar *self)
{
    self->items = g_array_new(FALSE, TRUE, sizeof(MarkerBarItem));

    gtk_widget_add_events(GTK_WIDGET(self),
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                              GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK);
    gtk_widget_set_has_tooltip(GTK_WIDGET(self), TRUE);
}

GtkWidget *gw_marker_bar_new(void)
{
    return g_object_new(GW_TYPE_MARKER_BAR, NULL);
}
