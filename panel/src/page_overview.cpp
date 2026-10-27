// The Overview page: GPU name, driver, PCI address and a state chip; stat tiles (memory, utilisation, temperature,
// power); detail rows; the compute-process list; and the "Refresh every" setting. Port of OverviewPage in panel/main.py.
// Battery rule: this page never runs a tool. It renders the polling engine's snapshots (ui_poll.h); while the GPU is
// runtime-suspended it shows an "asleep" card instead of the tiles, and only "Wake and read" queries the GPU, once.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app.h"
#include "gpu_sysfs.h"
#include "smi_csv.h"

namespace {

std::string num(const std::optional<double> &v, int digits, const char *unit = "") {
  if (!v) return "-";
  char b[48];
  std::snprintf(b, sizeof b, "%.*f", digits, *v);
  return b + std::string(unit);
}

std::string mib(const std::optional<double> &v) {
  if (!v) return "-";
  return *v >= 1024 ? num(*v / 1024, 1, " GiB") : num(*v, 0, " MiB");
}

GtkWidget *css_label(const std::string &text, const char *css) {
  GtkWidget *l = gtk_label_new(text.c_str());
  gtk_label_set_xalign(GTK_LABEL(l), 0);
  gtk_label_set_wrap(GTK_LABEL(l), TRUE);
  gtk_widget_add_css_class(l, css);
  return l;
}

void set_class(GtkWidget *w, const char *css, bool on) {
  if (on) gtk_widget_add_css_class(w, css);
  else gtk_widget_remove_css_class(w, css);
}

const char *text_of(GtkWidget *label) { return gtk_label_get_label(GTK_LABEL(label)); }
const char *yes(GtkWidget *w) { return gtk_widget_get_visible(w) ? "true" : "false"; }

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
  gtk_label_set_selectable(GTK_LABEL(value), TRUE);
  gtk_widget_set_valign(value, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix(ADW_ACTION_ROW(add_row(group, title)), value);
  return value;
}

AdwPreferencesGroup *group(const char *title, const char *description = nullptr) {
  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(g, title);
  if (description) adw_preferences_group_set_description(g, description);
  return g;
}

struct Tile {
  GtkWidget *box, *value, *sub, *bar;

  explicit Tile(const char *caption) {
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(box, "nvc-tile");
    value = css_label("-", "nvc-tile-value");
    sub = css_label("", "nvc-tile-sub");
    bar = gtk_progress_bar_new();
    gtk_accessible_update_property(GTK_ACCESSIBLE(bar), GTK_ACCESSIBLE_PROPERTY_LABEL, caption, -1);
    std::string up = caption;
    for (char &c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (GtkWidget *w : {css_label(up, "nvc-tile-caption"), value, bar, sub}) gtk_box_append(GTK_BOX(box), w);
  }

  // level: "nvc-warn", "nvc-bad" or nullptr. A missing fraction is an empty bar.
  void set(const std::string &v, const std::string &s, std::optional<double> fraction, const char *level = nullptr) {
    gtk_label_set_label(GTK_LABEL(value), v.c_str());
    gtk_label_set_label(GTK_LABEL(sub), s.c_str());
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), fraction ? std::max(0.0, std::min(1.0, *fraction)) : 0.0);
    set_class(bar, "nvc-warn", level && std::string(level) == "nvc-warn");
    set_class(bar, "nvc-bad", level && std::string(level) == "nvc-bad");
  }
};

struct Overview {
  App &app;
  std::string pci, driver_cheap;  // sysfs, read once: nothing here may wake the GPU

  GtkWidget *title = nullptr, *sub = nullptr, *chip = nullptr, *asleep_card = nullptr, *wake_btn = nullptr,
            *error = nullptr, *flow = nullptr, *poll_row = nullptr;
  AdwPreferencesGroup *details = nullptr, *procs = nullptr;
  enum { kMem, kUtil, kTemp, kPower, kTiles };
  std::vector<Tile> tiles;
  enum { kGclk, kMclk, kPstate, kPcie, kFan, kHybrid, kSource, kRuntime, kRows };
  GtkWidget *val[kRows] = {};
  std::vector<GtkWidget *> proc_rows;
  std::string proc_sig, proc_text;  // what the list shows: the rebuild is skipped while it is unchanged

  // `nvcontrol --smoke` only: every event rewrites the page's state here, so the file holds the last one.
  std::map<std::string, std::string> *smoke_out = nullptr;
  bool wake_pressed = false, wake_reported = false;

  explicit Overview(App &a) : app(a) {}
  GtkWidget *build();
  void render(const PollEvent &e);
  void set_processes(const std::optional<std::vector<ComputeApp>> &apps);
  void poll_changed();
  void choose_poll(int seconds);
  void smoke_fill();
  void smoke_wake();
};

GtkWidget *Overview::build() {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);

  // hero: name, driver and PCI address, state chip
  GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_widget_add_css_class(hero, "nvc-hero");
  GtkWidget *icon = gtk_image_new_from_icon_name("nvc-gauge-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 40);
  GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(texts, TRUE);
  title = css_label("NVIDIA GPU", "title-3");
  sub = css_label("", "nvc-muted");
  gtk_box_append(GTK_BOX(texts), title);
  gtk_box_append(GTK_BOX(texts), sub);
  chip = gtk_label_new("Checking");
  gtk_widget_add_css_class(chip, "nvc-chip");
  gtk_widget_set_valign(chip, GTK_ALIGN_CENTER);
  for (GtkWidget *w : {icon, texts, chip}) gtk_box_append(GTK_BOX(hero), w);
  gtk_box_append(GTK_BOX(box), hero);

  // asleep card
  asleep_card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_widget_add_css_class(asleep_card, "card");
  gtk_widget_set_visible(asleep_card, FALSE);
  GtkWidget *sleep_icon = gtk_image_new_from_icon_name("nvc-sleep-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(sleep_icon), 32);
  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(text, TRUE);
  gtk_box_append(GTK_BOX(text), css_label("GPU asleep (runtime D3), not polled", "heading"));
  gtk_box_append(GTK_BOX(text), css_label("Reading the GPU wakes it and costs battery, so nothing is queried while it "
                                          "sleeps. Use the button to read it once.", "nvc-hint"));
  wake_btn = gtk_button_new_with_label("Wake and read");
  gtk_widget_set_valign(wake_btn, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(wake_btn, "suggested-action");
  g_signal_connect(wake_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) { static_cast<Overview *>(d)->app.query_now(true); }),
                   this);
  GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_widget_set_hexpand(inner, TRUE);
  gtk_widget_set_margin_top(inner, 16);
  gtk_widget_set_margin_bottom(inner, 16);
  gtk_widget_set_margin_start(inner, 16);
  gtk_widget_set_margin_end(inner, 16);
  for (GtkWidget *w : {sleep_icon, text, wake_btn}) gtk_box_append(GTK_BOX(inner), w);
  gtk_box_append(GTK_BOX(asleep_card), inner);
  gtk_box_append(GTK_BOX(box), asleep_card);

  // stat tiles
  for (const char *caption : {"Memory", "Utilisation", "Temperature", "Power"}) tiles.emplace_back(caption);
  flow = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 1);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 4);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 12);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 12);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
  for (const Tile &t : tiles) {
    gtk_flow_box_append(GTK_FLOW_BOX(flow), t.box);
    gtk_widget_set_focusable(gtk_widget_get_parent(t.box), FALSE);
  }
  gtk_box_append(GTK_BOX(box), flow);
  error = css_label("", "nvc-error");
  gtk_widget_set_visible(error, FALSE);
  gtk_box_append(GTK_BOX(box), error);

  // detail rows
  details = group("Details");
  const char *const titles[kRows] = {"Graphics clock", "Memory clock", "Performance state", "PCIe link", "Fan",
                                     "Hybrid graphics mode (supergfxctl)", "Power source", "Runtime power management"};
  for (int i = 0; i < kRows; ++i) val[i] = kv_row(details, titles[i]);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(details));

  procs = group("Compute processes",
                "Graphics-only clients (OpenGL, Vulkan, WebGPU) are not listed by NVIDIA's compute query.");
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(procs));

  // polling setting: written to panel.json; the polling engine reads app.settings at every tick
  AdwPreferencesGroup *prefs = group("Polling");
  poll_row = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(poll_row), "Refresh every");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(poll_row),
                              "Only while this page is visible and the GPU is awake. Polling keeps the GPU awake.");
  GtkStringList *choices = gtk_string_list_new(nullptr);
  guint selected = 0;
  for (guint i = 0; i < std::size(Settings::kPollChoices); ++i) {
    const int s = Settings::kPollChoices[i];
    gtk_string_list_append(choices, s == 0 ? "Off (manual refresh)" : (std::to_string(s) + (s == 1 ? " second" : " seconds")).c_str());
    if (s == app.settings.poll_interval) selected = i;
  }
  adw_combo_row_set_model(ADW_COMBO_ROW(poll_row), G_LIST_MODEL(choices));
  g_object_unref(choices);  // the row keeps it
  adw_combo_row_set_selected(ADW_COMBO_ROW(poll_row), selected);
  g_signal_connect(poll_row, "notify::selected",
                   G_CALLBACK(+[](GObject *, GParamSpec *, gpointer d) { static_cast<Overview *>(d)->poll_changed(); }), this);
  adw_preferences_group_add(prefs, poll_row);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(prefs));

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

  set_processes(std::nullopt);
  return scroll;
}

// ---- rendering a snapshot -------------------------------------------------------------------------------------

void Overview::render(const PollEvent &e) {
  const bool asleep = e.gpu_present && e.kind == PollEvent::Kind::Asleep;
  const GpuSample s = e.sample.value_or(GpuSample{});

  const char *chip_text = "State unknown", *chip_css = nullptr;
  if (!e.gpu_present) chip_text = "No NVIDIA GPU", chip_css = "nvc-chip-bad";
  else if (e.state == "asleep") chip_text = "Asleep", chip_css = "nvc-chip-warn";
  else if (e.stale) chip_text = "Stale", chip_css = "nvc-chip-warn";
  else if (e.state == "awake") chip_text = "Awake", chip_css = "nvc-chip-ok";
  gtk_label_set_label(GTK_LABEL(chip), chip_text);
  for (const char *c : {"nvc-chip-ok", "nvc-chip-warn", "nvc-chip-bad"}) set_class(chip, c, chip_css && std::string(c) == chip_css);

  gtk_label_set_label(GTK_LABEL(title), s.name.empty() ? "NVIDIA GPU" : s.name.c_str());
  const std::string driver = s.driver.empty() ? driver_cheap : s.driver;
  std::string hero_sub;
  if (!driver.empty()) hero_sub = "Driver " + driver;
  if (!pci.empty()) hero_sub += (hero_sub.empty() ? "" : " - ") + std::string("PCI ") + pci;
  gtk_label_set_label(GTK_LABEL(sub), hero_sub.empty() ? "No NVIDIA GPU found" : hero_sub.c_str());

  gtk_widget_set_visible(asleep_card, asleep);
  gtk_widget_set_visible(flow, e.gpu_present && !asleep);
  gtk_label_set_label(GTK_LABEL(error), e.error.c_str());
  gtk_widget_set_visible(error, !e.error.empty());
  for (GtkWidget *w : {flow, GTK_WIDGET(details), GTK_WIDGET(procs)}) set_class(w, "nvc-stale", e.stale);

  const bool mem = s.mem_used_mib && s.mem_total_mib && *s.mem_total_mib > 0;
  const std::optional<double> mem_frac = mem ? std::optional<double>(*s.mem_used_mib / *s.mem_total_mib) : std::nullopt;
  tiles[kMem].set(mem ? num(s.mem_used_mib, 0) + " / " + num(s.mem_total_mib, 0, " MiB") : "-",
                  mem_frac ? num(*mem_frac * 100, 0, " % used") : "", mem_frac,
                  mem_frac && *mem_frac > 0.92 ? "nvc-bad" : nullptr);
  tiles[kUtil].set(num(s.util_pct, 0, " %"), "GPU engine", s.util_pct ? std::optional<double>(*s.util_pct / 100) : std::nullopt);
  tiles[kTemp].set(num(s.temp_c, 0, " °C"), "", s.temp_c ? std::optional<double>(*s.temp_c / 100) : std::nullopt,
                   s.temp_c && *s.temp_c >= 85 ? "nvc-bad" : s.temp_c && *s.temp_c >= 75 ? "nvc-warn" : nullptr);
  const bool pw = s.power_w && s.power_limit_w && *s.power_limit_w > 0;
  tiles[kPower].set(num(s.power_w, 1, " W"), s.power_limit_w ? "limit " + num(s.power_limit_w, 0, " W") : "",
                    pw ? std::optional<double>(*s.power_w / *s.power_limit_w) : std::nullopt);

  auto set = [this](int row, const std::string &text) { gtk_label_set_label(GTK_LABEL(val[row]), text.c_str()); };
  set(kGclk, num(s.clock_gr_mhz, 0, " MHz"));
  set(kMclk, num(s.clock_mem_mhz, 0, " MHz"));
  set(kPstate, s.pstate ? *s.pstate : "-");
  set(kPcie, s.pcie_gen && s.pcie_width ? "Gen " + std::to_string(*s.pcie_gen) + " x" + std::to_string(*s.pcie_width) : "-");
  set(kFan, s.fan_pct ? num(s.fan_pct, 0, " %") : "not reported");
  set(kHybrid, e.hybrid ? *e.hybrid : "unknown");
  set(kSource, e.power_source == "ac" ? "AC power" : e.power_source == "battery" ? "Battery" : "unknown");
  std::string runtime = e.runtime.status.value_or("unknown");
  if (e.runtime.control) runtime += ", control: " + *e.runtime.control;
  if (e.runtime.power_state) runtime += ", " + *e.runtime.power_state;
  set(kRuntime, runtime);

  set_processes(e.apps);
  smoke_fill();
  if (smoke_out && wake_pressed && !wake_reported && e.kind == PollEvent::Kind::Sample) {  // the read the button asked for
    wake_reported = true;
    (*smoke_out)["overview_wake_card"] = yes(asleep_card);
    (*smoke_out)["overview_wake_mem"] = text_of(tiles[kMem].value);
  }
}

void Overview::set_processes(const std::optional<std::vector<ComputeApp>> &apps) {
  std::string sig = apps ? "apps" : "none";
  if (apps)
    for (const ComputeApp &a : *apps) sig += "\n" + std::to_string(a.pid) + "\t" + a.name + "\t" + mib(a.mem_mib);
  if (sig == proc_sig && !proc_rows.empty()) return;
  proc_sig = sig;
  for (GtkWidget *r : proc_rows) adw_preferences_group_remove(procs, r);
  proc_rows.clear();
  proc_text.clear();

  auto row = [this](const std::string &title, const char *subtitle, const std::string &value) {
    GtkWidget *r = adw_action_row_new();
    char *markup = g_markup_escape_text(title.c_str(), -1);  // titles are Pango markup
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), markup);
    g_free(markup);
    if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(r), subtitle);
    if (!value.empty()) {
      GtkWidget *v = css_label(value, "nvc-muted");
      gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
      adw_action_row_add_suffix(ADW_ACTION_ROW(r), v);
    }
    adw_preferences_group_add(procs, r);
    proc_rows.push_back(r);
  };
  if (!apps || apps->empty()) {
    proc_text = apps ? "No compute processes" : "Not read yet";
    row(proc_text, nullptr, "");
    gtk_widget_add_css_class(proc_rows.back(), "nvc-muted");
    return;
  }
  for (const ComputeApp &a : *apps) {
    char *base = g_path_get_basename(a.name.c_str());
    const std::string name = a.name.empty() ? "unknown" : base;
    g_free(base);
    row(name, ("PID " + std::to_string(a.pid)).c_str(), mib(a.mem_mib));
    proc_text += (proc_text.empty() ? "" : "; ") + name + " PID " + std::to_string(a.pid) + " " + mib(a.mem_mib);
  }
}

// ---- the refresh setting ----------------------------------------------------------------------------------------

void Overview::poll_changed() {
  const guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(poll_row));
  if (i >= std::size(Settings::kPollChoices)) return;
  app.settings.poll_interval = Settings::kPollChoices[i];
  if (!app.settings.save()) app.toast("Could not save the refresh setting");
}

void Overview::choose_poll(int seconds) {
  for (guint i = 0; i < std::size(Settings::kPollChoices); ++i)
    if (Settings::kPollChoices[i] == seconds) adw_combo_row_set_selected(ADW_COMBO_ROW(poll_row), i);
}

// ---- `nvcontrol --smoke` ----------------------------------------------------------------------------------------

void Overview::smoke_fill() {
  if (!smoke_out) return;
  auto &o = *smoke_out;
  o["overview_title"] = text_of(title);
  o["overview_sub"] = text_of(sub);
  o["overview_chip"] = text_of(chip);
  o["overview_asleep_card"] = yes(asleep_card);
  o["overview_tiles"] = yes(flow);
  o["overview_error"] = gtk_widget_get_visible(error) ? text_of(error) : "";
  o["overview_stale"] = gtk_widget_has_css_class(flow, "nvc-stale") ? "true" : "false";
  const char *const keys[kTiles] = {"mem", "util", "temp", "power"};
  for (int i = 0; i < kTiles; ++i) {
    o[std::string("overview_") + keys[i]] = text_of(tiles[i].value);
    o[std::string("overview_") + keys[i] + "_sub"] = text_of(tiles[i].sub);
    char b[16];
    std::snprintf(b, sizeof b, "%.2f", gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(tiles[i].bar)));
    o[std::string("overview_") + keys[i] + "_bar"] = b;
  }
  const char *const rows[kRows] = {"gclk", "mclk", "pstate", "pcie", "fan", "hybrid", "source", "runtime"};
  for (int i = 0; i < kRows; ++i) o[std::string("overview_") + rows[i]] = text_of(val[i]);
  o["overview_procs"] = proc_text;
  o["overview_poll"] = std::to_string(Settings::kPollChoices[adw_combo_row_get_selected(ADW_COMBO_ROW(poll_row))]);
}

// NVC_SMOKE_WAKE=1 presses "Wake and read" once.
void Overview::smoke_wake() {
  if (!g_getenv("NVC_SMOKE_WAKE") || wake_pressed) return;
  wake_pressed = true;
  (*smoke_out)["overview_pre_wake_card"] = yes(asleep_card);
  g_signal_emit_by_name(wake_btn, "clicked");
}

}  // namespace

GtkWidget *make_overview_page(App &app) {
  Overview *p = new Overview(app);  // lives as long as the process (one window)
  const std::vector<PciGpu> gpus = find_nvidia_gpus();
  if (!gpus.empty()) p->pci = gpus.front().addr;
  p->driver_cheap = driver_version_cheap().value_or("");
  GtkWidget *page = p->build();
  app.add_poll_listener([p](const PollEvent &e) { p->render(e); });
  p->render(app.poll_last());
  // Smoke runs only: NVC_SMOKE_POLL=N picks "Refresh every N s" before the window maps, as if the user had (panel.json gets it).
  if (app.smoke)
    if (const char *s = g_getenv("NVC_SMOKE_POLL")) p->choose_poll(std::atoi(s));

  app.hooks[kPageOverview].on_smoke = [p](std::map<std::string, std::string> &out) {
    p->smoke_out = &out;
    p->smoke_fill();
    p->smoke_wake();
  };
  return page;
}
