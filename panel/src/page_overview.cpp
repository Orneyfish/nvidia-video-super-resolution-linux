// Placeholder: the Overview page task overwrites this file. The hooks only count calls so `nvcontrol --smoke` shows
// that the window drives them.
#include <string>

#include "app.h"

GtkWidget *make_overview_page(App &app) {
  static int shown = 0, refreshed = 0;
  PageHooks &h = app.hooks[kPageOverview];
  h.on_show = [] { ++shown; };
  h.on_refresh = [] { ++refreshed; };
  h.on_smoke = [](std::map<std::string, std::string> &out) {
    out["overview"] = "shown=" + std::to_string(shown) + " refreshed=" + std::to_string(refreshed);
  };

  GtkWidget *page = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), "nvc-gauge-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(page), "Overview");
  adw_status_page_set_description(ADW_STATUS_PAGE(page), "This page is not built yet.");
  return page;
}
