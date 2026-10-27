// The Power page: power limits, clocks and clock limiters from `nvidia-smi -q`, persistence mode, and terminal
// commands that change them. Battery rule: while the GPU is runtime-suspended nothing here calls nvidia-smi (it would
// wake the GPU); the page shows an "asleep" card and reads only when the user presses "Wake and read". Privileged
// commands are only shown and handed to App::run_in_terminal: the user reviews them and types the sudo password there.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app.h"
#include "gpu_sysfs.h"
#include "proc.h"
#include "smi_csv.h"
#include "smi_q.h"

namespace {

std::string watts(const std::optional<double> &w) {
  if (!w) return "-";
  char b[48];
  std::snprintf(b, sizeof b, "%g W", *w);
  return b;
}

std::string clock_text(const DetailInfo &d, const char *key) {
  const auto cur = d.clocks.find(key);
  if (cur == d.clocks.end()) return "-";
  std::string s = std::to_string(cur->second) + " MHz";
  const auto mx = d.max_clocks.find(key);
  if (mx != d.max_clocks.end() && mx->second) s += " (max " + std::to_string(mx->second) + " MHz)";
  return s;
}

GtkWidget *css_label(const std::string &text, const char *css) {
  GtkWidget *l = gtk_label_new(text.c_str());
  gtk_label_set_xalign(GTK_LABEL(l), 0);
  gtk_label_set_wrap(GTK_LABEL(l), TRUE);
  gtk_widget_add_css_class(l, css);
  return l;
}

GtkWidget *cmd_label() {
  GtkWidget *l = css_label("-", "nvc-cmd");
  gtk_label_set_selectable(GTK_LABEL(l), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(l), 28);
  gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
  return l;
}

GtkWidget *button(const char *label, bool suggested) {
  GtkWidget *b = gtk_button_new_with_label(label);
  gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
  if (suggested) gtk_widget_add_css_class(b, "suggested-action");
  return b;
}

GtkWidget *add_row(AdwPreferencesGroup *group, const char *title, const char *subtitle = nullptr) {
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  adw_preferences_group_add(group, row);
  return row;
}

// A "title ... value" row; returns the value label.
GtkWidget *kv_row(AdwPreferencesGroup *group, const char *title) {
  GtkWidget *value = css_label("-", "nvc-muted");
  gtk_label_set_xalign(GTK_LABEL(value), 1);
  gtk_label_set_justify(GTK_LABEL(value), GTK_JUSTIFY_RIGHT);
  gtk_label_set_selectable(GTK_LABEL(value), TRUE);
  gtk_widget_set_valign(value, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix(ADW_ACTION_ROW(add_row(group, title)), value);
  return value;
}

AdwPreferencesGroup *group(const char *title = nullptr, const char *description = nullptr) {
  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  if (title) adw_preferences_group_set_title(g, title);
  if (description) adw_preferences_group_set_description(g, description);
  return g;
}

const char *text_of(GtkWidget *label) { return gtk_label_get_label(GTK_LABEL(label)); }

struct Page;
struct Loaded {
  Page *page;
  PowerQuery q;
  std::optional<bool> persistence;
};

struct Page {
  App &app;
  std::optional<PowerInfo> info;
  std::optional<bool> persistence;
  bool busy = false;
  std::map<std::string, std::string> *smoke_out = nullptr;  // set by on_smoke; filled once the data is in

  GtkWidget *asleep_card = nullptr, *error = nullptr, *limit_error = nullptr;
  enum { kLimit, kDefault, kRange, kDraw, kGclk, kMclk, kLimiters, kPersist, kRows };
  GtkWidget *val[kRows] = {};
  GtkWidget *limit_row = nullptr, *limit_cmd = nullptr, *limit_btn = nullptr, *default_btn = nullptr;
  GtkWidget *persist_cmd = nullptr, *persist_btn = nullptr;

  explicit Page(App &a) : app(a) {}
  GtkWidget *build();
  void refresh(bool force);
  void wake() { refresh(true); }
  void finished(const Loaded &r);
  void show(const DetailInfo &d);
  std::optional<std::vector<std::string>> limit_command();
  void update_commands();
  void use_default();
  void run_limit();
  void run_persist();
  void smoke_report();
};

template <void (Page::*Method)()>
void on_click(GtkWidget *w, Page *p) {
  g_signal_connect(w, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) { (static_cast<Page *>(d)->*Method)(); }), p);
}

GtkWidget *Page::build() {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);

  asleep_card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_widget_add_css_class(asleep_card, "card");
  gtk_widget_set_visible(asleep_card, FALSE);
  GtkWidget *icon = gtk_image_new_from_icon_name("nvc-sleep-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(text, TRUE);
  gtk_box_append(GTK_BOX(text), css_label("GPU asleep (runtime D3), not polled", "heading"));
  gtk_box_append(GTK_BOX(text), css_label("The limits come from nvidia-smi, which would wake the GPU.", "nvc-hint"));
  GtkWidget *wake_btn = button("Wake and read", true);
  on_click<&Page::wake>(wake_btn, this);
  GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_widget_set_margin_top(inner, 16);
  gtk_widget_set_margin_bottom(inner, 16);
  gtk_widget_set_margin_start(inner, 16);
  gtk_widget_set_margin_end(inner, 16);
  gtk_widget_set_hexpand(inner, TRUE);
  gtk_box_append(GTK_BOX(inner), icon);
  gtk_box_append(GTK_BOX(inner), text);
  gtk_box_append(GTK_BOX(inner), wake_btn);
  gtk_box_append(GTK_BOX(asleep_card), inner);
  gtk_box_append(GTK_BOX(box), asleep_card);

  error = css_label("", "nvc-error");
  gtk_widget_set_visible(error, FALSE);
  gtk_box_append(GTK_BOX(box), error);

  AdwPreferencesGroup *read = group("Limits and clocks");
  const char *const titles[kRows] = {"Power limit",    "Default power limit", "Allowed range",       "Power draw",
                                     "Graphics clock", "Memory clock",        "Clock limiters active", "Persistence mode"};
  for (int i = 0; i < kRows; ++i) val[i] = kv_row(read, titles[i]);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(read));

  // The spin range stays wide on purpose: a value outside the card's range is refused inline by power_limit_command.
  AdwPreferencesGroup *change = group(
      "Change power limit (needs root)",
      "These open a terminal with the exact command. You review it and type your sudo password there; NV Control "
      "never runs them itself. The limit resets on reboot and the firmware may clamp it.");
  limit_row = adw_spin_row_new_with_range(1, 1000, 1);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(limit_row), "Power limit (W)");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(limit_row), "Wake and read first to learn the allowed range");
  adw_spin_row_set_numeric(ADW_SPIN_ROW(limit_row), TRUE);
  adw_spin_row_set_value(ADW_SPIN_ROW(limit_row), 100);
  gtk_widget_set_sensitive(limit_row, FALSE);
  g_signal_connect(limit_row, "notify::value",
                   G_CALLBACK(+[](GObject *, GParamSpec *, gpointer d) { static_cast<Page *>(d)->update_commands(); }), this);
  adw_preferences_group_add(change, limit_row);
  limit_cmd = cmd_label();
  default_btn = button("Use default", false);
  limit_btn = button("Run in terminal", true);
  on_click<&Page::use_default>(default_btn, this);
  on_click<&Page::run_limit>(limit_btn, this);
  GtkWidget *cmd_row = add_row(change, "Command");
  adw_action_row_add_suffix(ADW_ACTION_ROW(cmd_row), limit_cmd);
  adw_action_row_add_suffix(ADW_ACTION_ROW(cmd_row), default_btn);
  adw_action_row_add_suffix(ADW_ACTION_ROW(cmd_row), limit_btn);
  limit_error = css_label("", "nvc-error");
  gtk_widget_set_margin_top(limit_error, 6);
  gtk_widget_set_visible(limit_error, FALSE);
  adw_preferences_group_add(change, limit_error);  // non-row children go under the list
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(change));

  AdwPreferencesGroup *pers = group();
  GtkWidget *prow = add_row(pers, "Persistence mode",
                            "Keeps the driver loaded between runs. On this hybrid laptop it can stop the GPU from "
                            "sleeping, so leave it off unless you need it.");
  persist_cmd = cmd_label();
  persist_btn = button("Enable in terminal", true);
  on_click<&Page::run_persist>(persist_btn, this);
  adw_action_row_add_suffix(ADW_ACTION_ROW(prow), persist_cmd);
  adw_action_row_add_suffix(ADW_ACTION_ROW(prow), persist_btn);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(pers));

  AdwPreferencesGroup *none = group("Not available here");
  add_row(none, "No fan control", "The laptop firmware controls the fan; NVIDIA exposes no fan control for this GPU.");
  add_row(none, "No clock or voltage offsets", "Offsets need nvidia-settings (NV-CONTROL), which does not work on this laptop.");
  add_row(none, "nvidia-settings is X11-only", "It does not run on Wayland, so NV Control does not use it.");
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(none));

  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 860);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 640);
  gtk_widget_set_margin_top(clamp, 18);
  gtk_widget_set_margin_bottom(clamp, 24);
  gtk_widget_set_margin_start(clamp, 16);
  gtk_widget_set_margin_end(clamp, 16);
  adw_clamp_set_child(ADW_CLAMP(clamp), box);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
  update_commands();
  return scroll;
}

// ---- data ----------------------------------------------------------------------------------------------------

void Page::refresh(bool force) {
  if (busy) return;
  const auto gpus = find_nvidia_gpus();
  const RuntimeInfo rt = read_runtime(gpus.empty() ? nullptr : &gpus[0]);
  const bool asleep = gpu_state(rt.status) == "asleep" && !force;
  gtk_widget_set_visible(asleep_card, asleep);
  if (asleep) {
    gtk_widget_set_visible(error, FALSE);
    return;
  }
  busy = true;
  std::thread([this] {
    auto *r = new Loaded{this, {}, std::nullopt};
    try {
      r->q = query_power_page();
      if (r->q.info) {  // persistence mode is not in the -q text we parse
        const auto s = query_gpu();
        if (s.value) r->persistence = s.value->persistence;
      }
    } catch (const std::exception &e) {
      r->q = {std::nullopt, e.what()};
    }
    g_idle_add(+[](gpointer d) -> gboolean {
      std::unique_ptr<Loaded> res(static_cast<Loaded *>(d));
      res->page->finished(*res);
      return G_SOURCE_REMOVE;
    }, r);
  }).detach();
}

void Page::finished(const Loaded &r) {
  busy = false;
  if (!r.q.info) {
    gtk_label_set_label(GTK_LABEL(error), ("nvidia-smi failed: " + r.q.error).c_str());
    gtk_widget_set_visible(error, TRUE);
  } else {
    gtk_widget_set_visible(error, FALSE);
    persistence = r.persistence;
    show(*r.q.info);
  }
  if (smoke_out) smoke_report();
}

void Page::show(const DetailInfo &d) {
  const PowerInfo &p = d.power;
  info = p;
  auto set = [this](int row, const std::string &s) { gtk_label_set_label(GTK_LABEL(val[row]), s.c_str()); };
  set(kLimit, watts(p.limit_w));
  set(kDefault, watts(p.default_w));
  char range[96] = "-";
  if (p.min_w && p.max_w) std::snprintf(range, sizeof range, "%g to %g W", *p.min_w, *p.max_w);
  set(kRange, range);
  set(kDraw, watts(p.draw_w));
  set(kGclk, clock_text(d, "graphics"));
  set(kMclk, clock_text(d, "memory"));
  std::string lim = d.has_limiter_info ? "none" : "not reported";
  for (size_t i = 0; i < d.limiters.size(); ++i) lim = (i ? lim + ", " : std::string()) + d.limiters[i];
  set(kLimiters, lim);
  set(kPersist, persistence ? (*persistence ? "Enabled" : "Disabled") : "unknown");
  if (p.min_w && p.max_w) {
    const double v = p.limit_w ? *p.limit_w : p.default_w ? *p.default_w : *p.max_w;
    adw_spin_row_set_value(ADW_SPIN_ROW(limit_row), std::round(v));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(limit_row), (std::string("Allowed range ") + range).c_str());
    gtk_widget_set_sensitive(limit_row, TRUE);
  }
  update_commands();
}

// ---- terminal commands ---------------------------------------------------------------------------------------

// The command for the value in the spin row, or nullopt (with the reason shown inline) when it is not allowed.
std::optional<std::vector<std::string>> Page::limit_command() {
  gtk_widget_set_visible(limit_error, FALSE);
  if (!info) return std::nullopt;
  const double w = adw_spin_row_get_value(ADW_SPIN_ROW(limit_row));
  try {
    return power_limit_command(w, &*info);
  } catch (const std::invalid_argument &e) {
    char b[192];
    std::snprintf(b, sizeof b, "Cannot use %g W: %s", w, e.what());
    gtk_label_set_label(GTK_LABEL(limit_error), b);
    gtk_widget_set_visible(limit_error, TRUE);
    return std::nullopt;
  }
}

void Page::update_commands() {
  const auto cmd = limit_command();
  gtk_label_set_label(GTK_LABEL(limit_cmd), cmd ? display_command(*cmd).c_str() : "-");
  gtk_widget_set_sensitive(limit_btn, cmd.has_value());
  gtk_widget_set_sensitive(default_btn, info && info->default_w);
  // Until the mode is read the button stays off: its direction (enable or disable) would be a guess.
  const bool enabled = persistence.value_or(false);
  gtk_label_set_label(GTK_LABEL(persist_cmd), display_command(persistence_command(!enabled)).c_str());
  gtk_button_set_label(GTK_BUTTON(persist_btn), enabled ? "Disable in terminal" : "Enable in terminal");
  gtk_widget_set_sensitive(persist_btn, persistence.has_value());
}

void Page::use_default() {
  if (info && info->default_w) adw_spin_row_set_value(ADW_SPIN_ROW(limit_row), std::round(*info->default_w));
}

void Page::run_limit() {
  if (const auto cmd = limit_command()) app.run_in_terminal(*cmd, [this] { refresh(false); });
}

void Page::run_persist() {
  if (persistence) app.run_in_terminal(persistence_command(!*persistence), [this] { refresh(false); });
}

// ---- `nvcontrol --smoke` ---------------------------------------------------------------------------------------

// Reports what the page shows, checks that a limit above the card maximum is refused (and opens no terminal), and
// with NVC_SMOKE_POWER_RUN=W activates the run button for W watts.
void Page::smoke_report() {
  auto &o = *std::exchange(smoke_out, nullptr);
  o["power_asleep_card"] = gtk_widget_get_visible(asleep_card) ? "true" : "false";
  o["power_error"] = gtk_widget_get_visible(error) ? text_of(error) : "";
  if (!info) return;
  const char *const keys[kRows] = {"limit", "default", "range", "draw", "gclk", "mclk", "limiters", "persist"};
  for (int i = 0; i < kRows; ++i) o[std::string("power_") + keys[i]] = text_of(val[i]);
  o["power_persist_cmd"] = text_of(persist_cmd);
  if (info->max_w) {
    adw_spin_row_set_value(ADW_SPIN_ROW(limit_row), *info->max_w + 1);
    o["power_over_max_error"] = gtk_widget_get_visible(limit_error) ? text_of(limit_error) : "";
    o["power_over_max_run_sensitive"] = gtk_widget_get_sensitive(limit_btn) ? "true" : "false";
    g_signal_emit_by_name(limit_btn, "clicked");  // refused: the handler checks the value again
  }
  if (const char *w = g_getenv("NVC_SMOKE_POWER_RUN")) {
    adw_spin_row_set_value(ADW_SPIN_ROW(limit_row), std::strtod(w, nullptr));
    o["power_run_cmd"] = text_of(limit_cmd);
    g_signal_emit_by_name(limit_btn, "clicked");
  }
}

}  // namespace

GtkWidget *make_power_page(App &app) {
  Page *p = new Page(app);  // lives as long as the process (one window)
  PageHooks &h = app.hooks[kPagePower];
  h.on_show = [p] { p->refresh(false); };  // also serves F5: on_refresh is unset
  h.on_smoke = [p](std::map<std::string, std::string> &out) {
    p->smoke_out = &out;
    if (!p->busy) p->smoke_report();
  };
  return p->build();
}
