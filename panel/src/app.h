// The application shell of NV Control: GtkApplication, main window, view stack with the four pages, toasts, terminal
// launcher, window visibility. Page contents live in page_*.cpp (see pages.h). Needs GTK; the library `nvc` does not.
#pragma once

#include <adwaita.h>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pages.h"
#include "settings.h"

inline constexpr const char *kAppId = "org.nvvsr.Control";
inline constexpr const char *kAppName = "NV Control";

struct SmokeRequest {
  double seconds = 0;
  std::string out_path;
};

struct App {
  App();

  // --- what pages and the polling engine may use ---
  Settings settings;
  std::string app_dir;                    // <repo>/panel (compiled in): icons/ lives here, repo_dir(app_dir) finds the checkout
  std::map<std::string, PageHooks> hooks; // filled by the page factories, keyed by kPage* names
  AdwApplicationWindow *window = nullptr; // null until the first activate

  void toast(const std::string &text);
  // Opens a terminal (kitty or alacritty, kept open) running `command`; on_done runs on the main loop once the
  // terminal is closed. Privileged commands go through here, the app never runs them itself.
  void run_in_terminal(const std::vector<std::string> &command, std::function<void()> on_done = {});
  // Mapped and not minimised or suspended: nothing should poll while this is false.
  bool window_visible() const;
  std::string current_page() const;       // a kPage* name, "" before the window exists
  void show_page(const std::string &name);
  void refresh_current();                 // what F5 / Ctrl+R / the header button do

  // --- hooks for the polling engine (next task): both are empty for now ---
  void poll_start();                      // once, at the end of window construction
  void poll_stop();                       // on close-request

  // --- shell internals ---
  int run(const std::optional<SmokeRequest> &smoke_request);
  void build_window();
  void page_changed();
  void run_smoke();

  AdwApplication *application = nullptr;
  AdwToastOverlay *toast_overlay = nullptr;
  AdwToolbarView *toolbar_view = nullptr;
  AdwViewStack *stack = nullptr;
  GtkWidget *switcher_bar = nullptr;
  std::optional<SmokeRequest> smoke;
  bool smoke_started = false;
  int exit_code = 0;
};
