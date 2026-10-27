#include "ui_badge.h"

#include <algorithm>

namespace {

struct Badge {
  GtkWidget *area = nullptr;
  bool shown = true;
  int size = 11;
  double opacity = 0.55;
  int position = 9;
};

void draw(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer data) {
  const Badge &b = *static_cast<const Badge *>(data);
  cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, w, h);
  cairo_pattern_add_color_stop_rgb(grad, 0, 0.10, 0.14, 0.20);
  cairo_pattern_add_color_stop_rgb(grad, 1, 0.30, 0.34, 0.38);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_set_source(cr, grad);
  cairo_fill(cr);
  cairo_pattern_destroy(grad);

  cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  if (!b.shown) {
    cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
    cairo_set_font_size(cr, 12);
    cairo_move_to(cr, 10, h / 2.0);
    cairo_show_text(cr, "Badge off");
    return;
  }
  const double scale = w / 1280.0, margin = 14 * scale * 2;
  cairo_set_font_size(cr, b.size * scale * 2);
  cairo_text_extents_t ext;
  cairo_text_extents(cr, "SR", &ext);
  const int p = b.position >= 1 && b.position <= 9 ? b.position : 9;
  const int col = (p - 1) % 3, row = (p - 1) / 3;  // col 0 left..2 right, row 0 bottom..2 top
  const double x = col == 0 ? margin - ext.x_bearing
                 : col == 1 ? (w - ext.width) / 2 - ext.x_bearing
                            : w - margin - ext.width - ext.x_bearing;
  const double y = row == 0 ? h - margin : row == 1 ? (h + ext.height) / 2 : margin + ext.height;
  cairo_move_to(cr, x, y);
  cairo_text_path(cr, "SR");
  cairo_set_source_rgba(cr, 0, 0, 0, b.opacity);
  cairo_set_line_width(cr, std::max(1.0, scale * 2));
  cairo_stroke_preserve(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, b.opacity);
  cairo_fill(cr);
}

}  // namespace

GtkWidget *make_badge_preview() {
  Badge *b = new Badge;
  b->area = gtk_drawing_area_new();
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(b->area), 120);
  gtk_widget_set_hexpand(b->area, TRUE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(b->area), draw, b, nullptr);
  // GtkAspectFrame does not do height-for-width, so the height follows the allocated width one layout pass later
  g_signal_connect(b->area, "resize", G_CALLBACK(+[](GtkDrawingArea *da, int w, int, gpointer) {
                     if (gtk_drawing_area_get_content_height(da) != w * 9 / 16)
                       gtk_drawing_area_set_content_height(da, w * 9 / 16);
                   }), nullptr);
  GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(frame), b->area);
  gtk_widget_add_css_class(frame, "card");
  gtk_widget_set_overflow(frame, GTK_OVERFLOW_HIDDEN);  // rounded corners clip the drawing
  gtk_widget_set_margin_top(frame, 6);
  gtk_widget_set_margin_bottom(frame, 6);
  gtk_widget_set_tooltip_text(frame, "Preview of the on-screen badge");
  g_object_set_data_full(G_OBJECT(frame), "nvc-badge", b, [](gpointer p) { delete static_cast<Badge *>(p); });
  return frame;
}

void update_badge_preview(GtkWidget *preview, bool shown, int size, double opacity, int position) {
  Badge *b = static_cast<Badge *>(g_object_get_data(G_OBJECT(preview), "nvc-badge"));
  if (!b) return;
  b->shown = shown;
  b->size = size;
  b->opacity = opacity;
  b->position = position;
  gtk_widget_queue_draw(b->area);
}
