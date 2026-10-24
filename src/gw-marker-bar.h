#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GW_TYPE_MARKER_BAR (gw_marker_bar_get_type())
G_DECLARE_FINAL_TYPE(GwMarkerBar, gw_marker_bar, GW, MARKER_BAR, GtkDrawingArea)

GtkWidget *gw_marker_bar_new(void);

G_END_DECLS
