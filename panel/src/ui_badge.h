// A miniature 16:9 frame (a 1280x720 mpv window drawn at 2x so the badge is legible) with the status badge "SR"
// where and how mpv/vsr.lua would draw it: osd_size, osd_opacity, osd_position (numpad layout) and osd on/off.
#pragma once

#include <gtk/gtk.h>

GtkWidget *make_badge_preview();
// position is the numpad corner 1..9 (anything else is treated as 9); shown == false draws "Badge off".
void update_badge_preview(GtkWidget *preview, bool shown, int size, double opacity, int position);
