// Headless driver of the settings editor (ui_editor.cpp) and the badge preview (ui_badge.cpp), run inside cage by
// tests/smoke_editor.sh. Opens one window with the editor for FILE, plays one scenario by driving the widgets by
// name, prints ONE json line with what it saw and quits. The script judges the result and the file; this program
// only reports. Never started on the desktop: it writes FILE (and FILE.bak-nvvsr).
//
//   editor_harness --kind mpv|vlc --scenario save|default|blocked|conflict [--shot PNG] FILE
#include <adwaita.h>
#include <json-glib/json-glib.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <vector>

#include "app.h"
#include "theme.h"
#include "ui_badge.h"
#include "ui_editor.h"

// The editor only needs these two members of App; the real ones live in app.cpp with the whole window shell.
static std::vector<std::string> g_toasts;
App::App() : poll([] { return PollContext{}; }) {}  // App now owns the polling engine: an idle probe is enough here
void App::toast(const std::string &text) { g_toasts.push_back(text); }

namespace {

struct Plan {  // what to edit per kind: a switch and a spin (the value is never the default)
  const char *sw, *sw_on_text, *spin;
  double spin_val;
};
const Plan kMpv = {"enabled", "yes", "osd_size", 20}, kVlc = {"nvvsr-ac-only", "1", "nvvsr-min-height", 480};

GtkWidget *g_content = nullptr;  // badge preview (mpv) + editor
GtkWidget *g_badge = nullptr;
AdwApplicationWindow *g_win = nullptr;
int g_changes = 0, g_saved = 0;

using Pred = std::function<bool(GtkWidget *)>;
GtkWidget *find(GtkWidget *w, const Pred &pred) {
  if (pred(w)) return w;
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
    if (GtkWidget *r = find(c, pred)) return r;
  return nullptr;
}
GtkWidget *need(const std::string &name) {
  GtkWidget *w = find(g_content, [&](GtkWidget *x) { return name == gtk_widget_get_name(x); });
  if (!w) throw std::runtime_error("widget not found: " + name);
  return w;
}

void wait_for(const std::function<bool()> &pred, const char *what, int ms = 5000) {
  const gint64 end = g_get_monotonic_time() + ms * 1000LL;
  while (!pred()) {
    if (g_get_monotonic_time() > end) throw std::runtime_error(std::string("timed out waiting for ") + what);
    g_main_context_iteration(nullptr, FALSE);
    g_usleep(2000);
  }
}
void settle(int ms = 200) {  // let the widgets react to what was just done
  const gint64 end = g_get_monotonic_time() + ms * 1000LL;
  while (g_get_monotonic_time() < end) {
    g_main_context_iteration(nullptr, FALSE);
    g_usleep(2000);
  }
}

void click(GtkWidget *w, const std::string &what) {
  if (!gtk_widget_is_sensitive(w) || !gtk_widget_get_visible(w)) throw std::runtime_error("not clickable: " + what);
  g_signal_emit_by_name(w, "clicked");
  settle();
}
void click(const std::string &name) { click(need(name), name); }

void set_switch(const std::string &key, bool on) { adw_switch_row_set_active(ADW_SWITCH_ROW(need(key)), on); settle(); }
void set_spin(const std::string &key, double v) { adw_spin_row_set_value(ADW_SPIN_ROW(need(key)), v); settle(); }
bool get_switch(const std::string &key) { return adw_switch_row_get_active(ADW_SWITCH_ROW(need(key))); }
double get_spin(const std::string &key) { return adw_spin_row_get_value(ADW_SPIN_ROW(need(key))); }
bool sensitive(const std::string &name) { return gtk_widget_is_sensitive(need(name)); }
bool visible(const std::string &name) {  // what the user can see and press (the reset buttons are faded out, not hidden)
  GtkWidget *w = need(name);
  return gtk_widget_get_visible(w) && gtk_widget_get_opacity(w) > 0.5 && gtk_widget_is_sensitive(w);
}
std::string message() { return gtk_label_get_label(GTK_LABEL(need("nvc-message"))); }
bool message_is_error() { return gtk_widget_has_css_class(need("nvc-message"), "nvc-error"); }

void append_line(const std::string &path, const std::string &line) {  // an edit made behind the editor's back
  std::ofstream(path, std::ios::app) << line << "\n";
}

// The conflict dialog: wait for it, report it, press one of its buttons, wait until it is gone.
void answer_dialog(const char *button, const char *prefix, JsonBuilder *b) {
  AdwDialog *dlg = nullptr;
  wait_for([&] { return (dlg = adw_application_window_get_visible_dialog(g_win)) != nullptr; }, "the conflict dialog");
  json_builder_set_member_name(b, (std::string(prefix) + "_heading").c_str());
  json_builder_add_string_value(b, adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(dlg)));
  json_builder_set_member_name(b, (std::string(prefix) + "_body").c_str());
  json_builder_add_string_value(b, adw_alert_dialog_get_body(ADW_ALERT_DIALOG(dlg)));
  GtkWidget *btn = find(GTK_WIDGET(dlg), [&](GtkWidget *w) {
    const char *l = GTK_IS_BUTTON(w) ? gtk_button_get_label(GTK_BUTTON(w)) : nullptr;
    return l && !strcmp(l, button);
  });
  if (!btn) throw std::runtime_error(std::string("dialog has no button ") + button);
  g_signal_emit_by_name(btn, "clicked");
  wait_for([&] { return adw_application_window_get_visible_dialog(g_win) == nullptr; }, "the dialog to close");
  settle();
}

GdkTexture *render(GtkWidget *w) {
  GdkPaintable *p = gtk_widget_paintable_new(w);
  GtkSnapshot *s = gtk_snapshot_new();
  gdk_paintable_snapshot(p, s, gtk_widget_get_width(w), gtk_widget_get_height(w));
  GskRenderNode *node = gtk_snapshot_free_to_node(s);
  GdkTexture *t = node ? gsk_renderer_render_texture(gtk_native_get_renderer(gtk_widget_get_native(w)), node, nullptr)
                       : nullptr;
  if (node) gsk_render_node_unref(node);
  g_object_unref(p);
  return t;
}

struct Lit {
  double cx = -1, cy = -1;  // centre of the badge pixels, 0..1 of the preview
  int count = 0, peak = 0;  // number of badge pixels, brightest channel value seen
};
// The badge is white on a gradient that never gets brighter than ~100 per channel; the outer 3 px are the card border.
Lit lit_pixels(GtkWidget *w) {
  Lit r;
  GdkTexture *t = render(w);
  if (!t) return r;
  const int tw = gdk_texture_get_width(t), th = gdk_texture_get_height(t), stride = tw * 4;
  std::vector<guchar> px(static_cast<size_t>(stride) * th);
  gdk_texture_download(t, px.data(), stride);  // premultiplied BGRA (little endian) in the default format
  double sx = 0, sy = 0;
  for (int y = 3; y < th - 3; ++y)
    for (int x = 3; x < tw - 3; ++x) {
      const guchar *p = &px[static_cast<size_t>(y) * stride + x * 4];
      const int v = std::min(p[0], std::min(p[1], p[2]));
      r.peak = std::max(r.peak, v);
      if (v > 130) { ++r.count; sx += x; sy += y; }
    }
  if (r.count) { r.cx = sx / r.count / tw; r.cy = sy / r.count / th; }
  g_object_unref(t);
  return r;
}

void put_lit(JsonBuilder *b, const char *key, const Lit &l) {
  json_builder_set_member_name(b, key);
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "cx"); json_builder_add_double_value(b, l.cx);
  json_builder_set_member_name(b, "cy"); json_builder_add_double_value(b, l.cy);
  json_builder_set_member_name(b, "count"); json_builder_add_int_value(b, l.count);
  json_builder_set_member_name(b, "peak"); json_builder_add_int_value(b, l.peak);
  json_builder_end_object(b);
}

void badge_checks(JsonBuilder *b) {
  update_badge_preview(g_badge, true, 11, 1.0, 9); settle(); put_lit(b, "badge_pos9", lit_pixels(g_badge));
  update_badge_preview(g_badge, true, 11, 1.0, 1); settle(); put_lit(b, "badge_pos1", lit_pixels(g_badge));
  update_badge_preview(g_badge, true, 11, 1.0, 5); settle(); put_lit(b, "badge_pos5", lit_pixels(g_badge));
  update_badge_preview(g_badge, true, 40, 1.0, 9); settle(); put_lit(b, "badge_size40", lit_pixels(g_badge));
  update_badge_preview(g_badge, true, 11, 0.3, 9); settle(); put_lit(b, "badge_opacity03", lit_pixels(g_badge));
  update_badge_preview(g_badge, true, 11, 1.0, 9); settle(); put_lit(b, "badge_size11", lit_pixels(g_badge));
  json_builder_set_member_name(b, "badge_aspect");  // width / height of the preview
  json_builder_add_double_value(b, static_cast<double>(gtk_widget_get_width(g_badge)) / gtk_widget_get_height(g_badge));
}

void put(JsonBuilder *b, const char *k, bool v) { json_builder_set_member_name(b, k); json_builder_add_boolean_value(b, v); }
void put(JsonBuilder *b, const char *k, double v) { json_builder_set_member_name(b, k); json_builder_add_double_value(b, v); }
void put(JsonBuilder *b, const char *k, int v) { json_builder_set_member_name(b, k); json_builder_add_int_value(b, v); }
void put(JsonBuilder *b, const char *k, const std::string &v) { json_builder_set_member_name(b, k); json_builder_add_string_value(b, v.c_str()); }

void scenario(const std::string &name, const Plan &p, const std::string &path, bool mpv, JsonBuilder *b) {
  wait_for([] { return gtk_widget_get_mapped(GTK_WIDGET(g_win)) != FALSE; }, "the window");
  settle(400);
  put(b, "save_initial", sensitive("nvc-save"));
  put(b, "revert_initial", sensitive("nvc-revert"));
  put(b, "message_initial", message());

  if (name == "save" || name == "blocked") {
    set_switch(p.sw, !get_switch(p.sw));
    set_spin(p.spin, p.spin_val);
    put(b, "save_after_edit", sensitive("nvc-save"));
    put(b, "default_shown", visible(std::string("default-") + p.spin));
    click("nvc-save");
    put(b, "message", message());
    put(b, "message_error", message_is_error());
    put(b, "save_after_save", sensitive("nvc-save"));
    put(b, "switch_after", get_switch(p.sw));
    put(b, "spin_after", get_spin(p.spin));
  } else if (name == "default") {
    set_spin(p.spin, p.spin_val);
    click("nvc-save");
    put(b, "spin_saved", get_spin(p.spin));
    click(std::string("default-") + p.spin);
    put(b, "save_after_default", sensitive("nvc-save"));
    put(b, "default_hidden", !visible(std::string("default-") + p.spin));
    click("nvc-save");
    put(b, "spin_after", get_spin(p.spin));
    put(b, "save_after_save", sensitive("nvc-save"));
  } else if (name == "conflict") {
    set_switch(p.sw, false);
    append_line(path, std::string(p.sw) + "=" + p.sw_on_text);
    click("nvc-save");
    put(b, "message", message());
    answer_dialog("Reload", "dialog1", b);
    put(b, "switch_after_reload", get_switch(p.sw));
    put(b, "save_after_reload", sensitive("nvc-save"));
    set_spin(p.spin, p.spin_val);
    append_line(path, std::string(p.spin) + "=" + std::to_string(static_cast<int>(p.spin_val) + 5));
    click("nvc-save");
    answer_dialog("Overwrite those keys", "dialog2", b);
    put(b, "spin_after_overwrite", get_spin(p.spin));
    put(b, "save_after_overwrite", sensitive("nvc-save"));
    put(b, "message_after_overwrite", message());
  } else {
    throw std::runtime_error("unknown scenario " + name);
  }
  put(b, "toasts", [&] { std::string s; for (const auto &t : g_toasts) s += (s.empty() ? "" : "|") + t; return s; }());
  put(b, "on_saved", g_saved);
  put(b, "on_change", g_changes > 0);
  if (mpv && name == "save") badge_checks(b);
}

struct Job {
  std::string name, path, shot;
  Plan plan;
  bool mpv;
  int rc = 0;
};

gboolean run_job(gpointer data) {
  Job &j = *static_cast<Job *>(data);
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  put(b, "scenario", j.name);
  try {
    scenario(j.name, j.plan, j.path, j.mpv, b);
    if (!j.shot.empty()) {
      GdkTexture *t = render(g_content);
      if (!t || !gdk_texture_save_to_png(t, j.shot.c_str())) throw std::runtime_error("could not write the screenshot");
      g_object_unref(t);
    }
  } catch (const std::exception &e) {
    put(b, "error", std::string(e.what()));
    j.rc = 1;
  }
  json_builder_end_object(b);
  JsonGenerator *g = json_generator_new();
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  gchar *text = json_generator_to_data(g, nullptr);
  g_print("%s\n", text);
  fflush(stdout);
  g_free(text);
  json_node_unref(root);
  g_object_unref(g);
  g_object_unref(b);
  g_application_quit(g_application_get_default());
  return G_SOURCE_REMOVE;
}

}  // namespace

int main(int argc, char **argv) {
  static Job job;
  std::string kind;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--kind" && i + 1 < argc) kind = argv[++i];
    else if (a == "--scenario" && i + 1 < argc) job.name = argv[++i];
    else if (a == "--shot" && i + 1 < argc) job.shot = argv[++i];
    else job.path = a;
  }
  if ((kind != "mpv" && kind != "vlc") || job.name.empty() || job.path.empty()) {
    fprintf(stderr, "usage: editor_harness --kind mpv|vlc --scenario save|default|blocked|conflict [--shot PNG] FILE\n");
    return 2;
  }
  job.mpv = kind == "mpv";
  job.plan = job.mpv ? kMpv : kVlc;

  static App app;
  AdwApplication *application = adw_application_new("org.nvvsr.Control.EditorHarness", G_APPLICATION_NON_UNIQUE);
  g_signal_connect(application, "startup", G_CALLBACK(+[](GApplication *, gpointer) {
                     const Scheme scheme = load_scheme();
                     adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
                                                        scheme.mode == "light" ? ADW_COLOR_SCHEME_FORCE_LIGHT : ADW_COLOR_SCHEME_FORCE_DARK);
                     GtkCssProvider *css = gtk_css_provider_new();
                     gtk_css_provider_load_from_string(css, build_material_css(scheme.colours).c_str());
                     gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
                     g_object_unref(css);
                   }), nullptr);
  g_signal_connect(application, "activate", G_CALLBACK(+[](GApplication *a, gpointer) {
                     ConfEditorSpec spec;
                     spec.path = job.path;
                     spec.options = job.mpv ? mpv_options() : vlc_options();
                     spec.section = job.mpv ? "" : "nvvsr";
                     spec.bool_style = job.mpv ? BoolStyle::YesNo : BoolStyle::ZeroOne;
                     spec.title = job.mpv ? "mpv (vsr.conf)" : "VLC (vlcrc)";
                     if (job.name == "blocked")
                       spec.blocked = [] { return std::string("VLC is running. It rewrites vlcrc when it exits and would discard these changes. Close VLC first."); };
                     else
                       spec.blocked = [] { return std::string(); };
                     spec.on_saved = [] { ++g_saved; };
                     spec.on_change = [](const std::map<std::string, Value> &v) {
                       ++g_changes;
                       if (g_badge && v.count("osd_size"))
                         update_badge_preview(g_badge, std::get<bool>(v.at("osd")), std::get<int>(v.at("osd_size")),
                                              std::get<double>(v.at("osd_opacity")), std::get<int>(v.at("osd_position")));
                     };
                     g_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
                     gtk_widget_set_margin_top(g_content, 12);
                     gtk_widget_set_margin_bottom(g_content, 12);
                     gtk_widget_set_margin_start(g_content, 16);
                     gtk_widget_set_margin_end(g_content, 16);
                     if (job.mpv) {
                       g_badge = make_badge_preview();
                       GtkWidget *clamp = adw_clamp_new();
                       adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 520);
                       adw_clamp_set_child(ADW_CLAMP(clamp), g_badge);
                       gtk_box_append(GTK_BOX(g_content), clamp);
                     }
                     gtk_box_append(GTK_BOX(g_content), make_conf_editor(app, spec));
                     GtkWidget *scroll = gtk_scrolled_window_new();
                     gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), g_content);
                     gtk_widget_set_vexpand(scroll, TRUE);
                     GtkWidget *view = adw_toolbar_view_new();
                     adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
                     adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);
                     g_win = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(a)));
                     gtk_window_set_default_size(GTK_WINDOW(g_win), 720, 900);
                     adw_application_window_set_content(g_win, view);
                     gtk_window_present(GTK_WINDOW(g_win));
                     g_timeout_add(300, run_job, &job);
                   }), nullptr);
  char arg0[] = "editor_harness";  // GTK must not see our own options
  char *args[] = {arg0, nullptr};
  const int status = g_application_run(G_APPLICATION(application), 1, args);
  g_object_unref(application);
  return job.rc ? job.rc : status;
}
