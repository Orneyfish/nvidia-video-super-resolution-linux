// ViewSwitcherTitle/Bar are deprecated since libadwaita 1.4 but are what the design uses (the breakpoint replacement
// does not follow the narrowed content of the smoke run); drop this when libadwaita 2 removes them.
#define ADW_DISABLE_DEPRECATION_WARNINGS
#include "app.h"

#include <json-glib/json-glib.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <memory>

#include "proc.h"
#include "theme.h"

#ifndef NVC_APP_DIR
#error "NVC_APP_DIR (the panel directory) is set by panel/CMakeLists.txt"
#endif

namespace {

// g_timeout_add for a std::function; return true to keep the timer running.
guint timeout_add(guint ms, std::function<bool()> fn) {
  using Fn = std::function<bool()>;
  return g_timeout_add_full(
      G_PRIORITY_DEFAULT, ms,
      [](gpointer p) -> gboolean { return (*static_cast<Fn *>(p))() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE; },
      new Fn(std::move(fn)), [](gpointer p) { delete static_cast<Fn *>(p); });
}

App &self(gpointer data) { return *static_cast<App *>(data); }

struct PageInfo {
  const char *name, *title, *icon;
  GtkWidget *(*make)(App &);
};
const PageInfo kPages[] = {
    {kPageOverview, "Overview", "nvc-gauge-symbolic", make_overview_page},
    {kPageSr, "Super Resolution", "nvc-sparkle-symbolic", make_sr_page},
    {kPagePower, "Power", "nvc-bolt-symbolic", make_power_page},
    {kPageAbout, "About", "nvc-info-symbolic", make_about_page},
};

void on_startup(GApplication *, gpointer data) {
  GdkDisplay *display = gdk_display_get_default();
  gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(display), (self(data).app_dir + "/icons").c_str());
  gtk_window_set_default_icon_name(kAppId);

  const Scheme scheme = load_scheme();
  adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
                                     scheme.mode == "light" ? ADW_COLOR_SCHEME_FORCE_LIGHT : ADW_COLOR_SCHEME_FORCE_DARK);
  GtkCssProvider *css = gtk_css_provider_new();
  gtk_css_provider_load_from_string(css, build_material_css(scheme.colours).c_str());
  gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
}

void on_activate(GApplication *, gpointer data) {
  App &app = self(data);
  if (!app.window) app.build_window();
  gtk_window_present(GTK_WINDOW(app.window));
  if (app.smoke && !app.smoke_started) {
    app.smoke_started = true;
    app.run_smoke();
  }
}

}  // namespace

App::App() : app_dir(NVC_APP_DIR) { settings.load(); }

int App::run(const std::optional<SmokeRequest> &smoke_request) {
  smoke = smoke_request;
  g_set_prgname(kAppId);  // the Wayland app_id: matches StartupWMClass in the launcher
  g_set_application_name(kAppName);
  application = adw_application_new(kAppId, G_APPLICATION_DEFAULT_FLAGS);
  GtkApplication *gtk_app = GTK_APPLICATION(application);

  const char *const refresh_keys[] = {"F5", "<Control>r", nullptr};
  gtk_application_set_accels_for_action(gtk_app, "win.refresh", refresh_keys);
  for (int i = 0; i < 4; ++i) {
    const std::string action = std::string("win.page::") + kPages[i].name;
    const std::string alt = "<Alt>" + std::to_string(i + 1), ctrl = "<Control>" + std::to_string(i + 1);
    const char *const keys[] = {alt.c_str(), ctrl.c_str(), nullptr};
    gtk_application_set_accels_for_action(gtk_app, action.c_str(), keys);
  }
  const char *const quit_keys[] = {"<Control>q", nullptr};
  gtk_application_set_accels_for_action(gtk_app, "app.quit", quit_keys);
  GSimpleAction *quit = g_simple_action_new("quit", nullptr);
  g_signal_connect(quit, "activate", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer d) {
                     g_application_quit(G_APPLICATION(self(d).application));
                   }), this);
  g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(quit));
  g_object_unref(quit);

  g_signal_connect(application, "startup", G_CALLBACK(on_startup), this);
  g_signal_connect(application, "activate", G_CALLBACK(on_activate), this);

  char arg0[] = "nvcontrol";  // GTK must not see our own options (--smoke)
  char *args[] = {arg0, nullptr};
  const int status = g_application_run(G_APPLICATION(application), 1, args);
  g_object_unref(application);
  application = nullptr;
  return exit_code ? exit_code : status;
}

void App::build_window() {
  window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(application)));
  gtk_window_set_title(GTK_WINDOW(window), kAppName);
  gtk_window_set_default_size(GTK_WINDOW(window), 960, 700);
  gtk_widget_set_size_request(GTK_WIDGET(window), 340, 420);

  toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  adw_application_window_set_content(window, GTK_WIDGET(toast_overlay));
  toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  adw_toast_overlay_set_child(toast_overlay, GTK_WIDGET(toolbar_view));

  stack = ADW_VIEW_STACK(adw_view_stack_new());
  for (const PageInfo &p : kPages)
    adw_view_stack_add_titled_with_icon(stack, p.make(*this), p.name, p.title, p.icon);

  GtkWidget *switcher_title = adw_view_switcher_title_new();
  adw_view_switcher_title_set_stack(ADW_VIEW_SWITCHER_TITLE(switcher_title), stack);
  adw_view_switcher_title_set_title(ADW_VIEW_SWITCHER_TITLE(switcher_title), kAppName);
  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), switcher_title);
  GtkWidget *refresh = gtk_button_new_from_icon_name("nvc-refresh-symbolic");
  gtk_widget_set_tooltip_text(refresh, "Refresh (F5)");
  gtk_accessible_update_property(GTK_ACCESSIBLE(refresh), GTK_ACCESSIBLE_PROPERTY_LABEL, "Refresh", -1);
  g_signal_connect(refresh, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) { self(d).refresh_current(); }), this);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), refresh);
  adw_toolbar_view_add_top_bar(toolbar_view, header);
  switcher_bar = adw_view_switcher_bar_new();
  adw_view_switcher_bar_set_stack(ADW_VIEW_SWITCHER_BAR(switcher_bar), stack);
  g_object_bind_property(switcher_title, "title-visible", switcher_bar, "reveal", G_BINDING_SYNC_CREATE);
  adw_toolbar_view_add_bottom_bar(toolbar_view, switcher_bar);
  adw_toolbar_view_set_content(toolbar_view, GTK_WIDGET(stack));

  GSimpleAction *page = g_simple_action_new("page", G_VARIANT_TYPE_STRING);
  g_signal_connect(page, "activate", G_CALLBACK(+[](GSimpleAction *, GVariant *name, gpointer d) {
                     self(d).show_page(g_variant_get_string(name, nullptr));
                   }), this);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(page));
  g_object_unref(page);
  GSimpleAction *refresh_action = g_simple_action_new("refresh", nullptr);
  g_signal_connect(refresh_action, "activate", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer d) {
                     self(d).refresh_current();
                   }), this);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(refresh_action));
  g_object_unref(refresh_action);

  g_signal_connect(stack, "notify::visible-child-name",
                   G_CALLBACK(+[](GObject *, GParamSpec *, gpointer d) { self(d).page_changed(); }), this);
  g_signal_connect(window, "map", G_CALLBACK(+[](GtkWidget *, gpointer d) { self(d).page_changed(); }), this);
  g_signal_connect(window, "close-request", G_CALLBACK(+[](GtkWindow *, gpointer d) -> gboolean {
                     self(d).poll_stop();
                     return FALSE;
                   }), this);
  poll_start();
}

void App::poll_start() {}  // polling engine: start the 1 s tick here
void App::poll_stop() {}   // polling engine: stop it here

void App::toast(const std::string &text) {
  if (!toast_overlay) return;
  AdwToast *t = adw_toast_new(text.c_str());
  adw_toast_set_timeout(t, 3);
  adw_toast_overlay_add_toast(toast_overlay, t);
}

void App::run_in_terminal(const std::vector<std::string> &command, std::function<void()> on_done) {
  const auto argv = terminal_argv(command);
  if (!argv) {
    toast("No terminal emulator found (kitty or alacritty)");
    return;
  }
  std::vector<const gchar *> v;
  for (const std::string &s : *argv) v.push_back(s.c_str());
  v.push_back(nullptr);
  GError *err = nullptr;
  GSubprocess *p = g_subprocess_newv(v.data(), G_SUBPROCESS_FLAGS_NONE, &err);
  if (!p) {
    toast(std::string("Could not open a terminal: ") + err->message);
    g_clear_error(&err);
    return;
  }
  using Done = std::function<void()>;
  g_subprocess_wait_async(
      p, nullptr,
      [](GObject *source, GAsyncResult *res, gpointer d) {
        std::unique_ptr<Done> done(static_cast<Done *>(d));
        GError *e = nullptr;
        g_subprocess_wait_finish(G_SUBPROCESS(source), res, &e);
        g_clear_error(&e);
        if (*done) (*done)();
      },
      new Done(std::move(on_done)));
  g_object_unref(p);  // the pending wait keeps its own reference
}

bool App::window_visible() const {
  if (!window || !gtk_widget_get_mapped(GTK_WIDGET(window))) return false;
  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
  if (!surface || !GDK_IS_TOPLEVEL(surface)) return false;
  return !(gdk_toplevel_get_state(GDK_TOPLEVEL(surface)) & (GDK_TOPLEVEL_STATE_MINIMIZED | GDK_TOPLEVEL_STATE_SUSPENDED));
}

std::string App::current_page() const {
  const char *name = stack ? adw_view_stack_get_visible_child_name(stack) : nullptr;
  return name ? name : "";
}

void App::show_page(const std::string &name) { adw_view_stack_set_visible_child_name(stack, name.c_str()); }

void App::page_changed() {
  if (!window_visible()) return;
  const auto it = hooks.find(current_page());
  if (it != hooks.end() && it->second.on_show) it->second.on_show();
}

void App::refresh_current() {
  const auto it = hooks.find(current_page());
  if (it == hooks.end()) return;
  const PageHooks &h = it->second;
  if (h.on_refresh) h.on_refresh();
  else if (h.on_show) h.on_show();
}

// ------------------------------------------------------------------------------------------------------------
// `nvcontrol --smoke SECONDS OUTFILE`: headless self test. Visits every page through the window actions, presses
// refresh, narrows the window to check the bottom switcher bar, runs each page's on_smoke hook, writes a JSON file
// and quits after about SECONDS.
// ------------------------------------------------------------------------------------------------------------

namespace {

struct Smoke {
  std::vector<std::string> pages, hidden_pages, errors, icons_missing;
  std::map<std::string, std::string> results;
  std::optional<bool> bar_wide, bar_narrow;
  std::string terminal = "not run";
};

void add_strings(JsonBuilder *b, const char *key, const std::vector<std::string> &v) {
  json_builder_set_member_name(b, key);
  json_builder_begin_array(b);
  for (const std::string &s : v) json_builder_add_string_value(b, s.c_str());
  json_builder_end_array(b);
}

void add_bool(JsonBuilder *b, const char *key, const std::optional<bool> &v) {
  json_builder_set_member_name(b, key);
  if (v) json_builder_add_boolean_value(b, *v);
  else json_builder_add_null_value(b);
}

bool write_smoke(const Smoke &s, const std::string &path) {
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  add_strings(b, "pages", s.pages);
  add_strings(b, "hidden_pages", s.hidden_pages);
  add_strings(b, "errors", s.errors);
  add_strings(b, "icons_missing", s.icons_missing);
  add_bool(b, "bar_reveal_wide", s.bar_wide);
  add_bool(b, "bar_reveal_narrow", s.bar_narrow);
  json_builder_set_member_name(b, "terminal");
  json_builder_add_string_value(b, s.terminal.c_str());
  json_builder_set_member_name(b, "results");
  json_builder_begin_object(b);
  for (const auto &[k, v] : s.results) {
    json_builder_set_member_name(b, k.c_str());
    json_builder_add_string_value(b, v.c_str());
  }
  json_builder_end_object(b);
  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  json_generator_set_root(gen, root);
  GError *err = nullptr;
  const bool ok = json_generator_to_file(gen, path.c_str(), &err);
  if (!ok) g_printerr("cannot write %s: %s\n", path.c_str(), err->message);
  g_clear_error(&err);
  json_node_unref(root);
  g_object_unref(gen);
  g_object_unref(b);
  return ok;
}

}  // namespace

void App::run_smoke() {
  auto st = std::make_shared<Smoke>();
  GtkIconTheme *icons = gtk_icon_theme_get_for_display(gdk_display_get_default());
  for (const char *n : {"nvc-gauge-symbolic", "nvc-sparkle-symbolic", "nvc-bolt-symbolic", "nvc-info-symbolic",
                        "nvc-refresh-symbolic", "nvc-check-symbolic", "nvc-warning-symbolic", "nvc-sleep-symbolic", kAppId})
    if (!gtk_icon_theme_has_icon(icons, n)) st->icons_missing.push_back(n);

  // the accelerators of the header: F5 / Ctrl+R refresh, Alt+N / Ctrl+N pages
  auto expect_accel = [this, st](const std::string &accel, const std::string &action) {
    char **got = gtk_application_get_actions_for_accel(GTK_APPLICATION(application), accel.c_str());
    if (!got || !got[0] || action != got[0]) st->errors.push_back(accel + " is not bound to " + action);
    g_strfreev(got);
  };
  expect_accel("F5", "win.refresh");
  expect_accel("<Control>r", "win.refresh");
  for (int i = 0; i < 4; ++i)
    for (const char *mod : {"<Alt>", "<Control>"})
      expect_accel(mod + std::to_string(i + 1), std::string("win.page::") + kPages[i].name);

  auto visit = [this, st](const char *name) {
    g_action_group_activate_action(G_ACTION_GROUP(window), "page", g_variant_new_string(name));
    st->pages.push_back(current_page());
    if (!window_visible()) st->hidden_pages.push_back(name);
    g_action_group_activate_action(G_ACTION_GROUP(window), "refresh", nullptr);
    const auto it = hooks.find(name);
    if (it != hooks.end() && it->second.on_smoke) it->second.on_smoke(st->results);
  };
  auto bar_reveal = [this] { return adw_view_switcher_bar_get_reveal(ADW_VIEW_SWITCHER_BAR(switcher_bar)) != FALSE; };

  std::vector<std::function<void()>> steps;
  steps.push_back([this, st, visit] {
    toast("smoke test");
    if (g_getenv("NVC_SMOKE_TERMINAL")) {  // tests put a fake terminal on PATH; a real one is never opened here
      st->terminal = "waiting";
      run_in_terminal({"true"}, [st] { st->terminal = "done"; });
    }
    visit(kPageOverview);
  });
  for (const char *name : {kPageSr, kPagePower, kPageAbout}) steps.push_back([visit, name] { visit(name); });
  steps.push_back([this, st, bar_reveal] {  // wide window: the title switcher shows, the bottom bar is hidden
    st->bar_wide = bar_reveal();
    AdwClamp *clamp = ADW_CLAMP(adw_clamp_new());  // 400 px wide content = a narrow window
    adw_clamp_set_maximum_size(clamp, 400);
    adw_clamp_set_tightening_threshold(clamp, 400);
    g_object_ref(toolbar_view);
    adw_toast_overlay_set_child(toast_overlay, nullptr);
    adw_clamp_set_child(clamp, GTK_WIDGET(toolbar_view));
    g_object_unref(toolbar_view);
    adw_toast_overlay_set_child(toast_overlay, GTK_WIDGET(clamp));
  });
  steps.push_back([st, visit, bar_reveal] {
    st->bar_narrow = bar_reveal();
    visit(kPageOverview);
  });

  const guint interval = std::max<guint>(300, static_cast<guint>(smoke->seconds * 1000 / (steps.size() + 1)));
  auto next = std::make_shared<size_t>(0);
  timeout_add(interval, [this, st, steps, next] {
    if (*next < steps.size()) {
      try {
        steps[(*next)++]();
      } catch (const std::exception &e) {
        st->errors.push_back(e.what());
      }
      return true;
    }
    if (!write_smoke(*st, smoke->out_path)) exit_code = 1;
    g_application_quit(G_APPLICATION(application));
    return false;
  });
}
