// The page interface: one factory per page, plus optional hooks the window calls on it.
//
// A page task overwrites exactly its own page_<name>.cpp. The factory builds the page's widget (it is added to the
// view stack as is, so wrap long content in a GtkScrolledWindow itself) and may register hooks:
//
//   GtkWidget *make_sr_page(App &app) {
//     ...
//     PageHooks &h = app.hooks[kPageSr];
//     h.on_show = [] { ... };
//     return widget;
//   }
//
// Page state lives as long as the process (one window): allocate it with `new`, or free it in the widget's
// "destroy" signal. Never block the GTK main loop in a hook: run subprocesses asynchronously. Source files that need
// GTK are named app.cpp, main.cpp, page_*.cpp or ui_*.cpp; everything else in src/ is the GTK-free library `nvc`.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <string>

struct App;

inline constexpr const char *kPageOverview = "overview";
inline constexpr const char *kPageSr = "sr";
inline constexpr const char *kPagePower = "power";
inline constexpr const char *kPageAbout = "about";

struct PageHooks {
  // The page became the visible one in a visible window (page switch, or the window was mapped on it).
  std::function<void()> on_show;
  // The user pressed F5, Ctrl+R or the refresh button on this page. Unset: on_show is called instead.
  std::function<void()> on_refresh;
  // Only for `nvcontrol --smoke`: called once when the smoke run shows this page. May click buttons; every
  // key/value put in `out` ends up under "results" in the smoke file.
  std::function<void(std::map<std::string, std::string> &out)> on_smoke;
};

GtkWidget *make_overview_page(App &app);
GtkWidget *make_sr_page(App &app);
GtkWidget *make_power_page(App &app);
GtkWidget *make_about_page(App &app);
