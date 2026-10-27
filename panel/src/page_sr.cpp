// The Super Resolution page: install status (one row per component, a fix hint when it is missing), the "Check SR
// works" probe, and the editors of the players' own config files (mpv's vsr.conf, VLC's vlcrc) with the live badge
// preview. Refreshing the status reads files and runs `mpv --vf=help` once, never the GPU; the probe touches the GPU
// through nvvsr_probe, so it runs only when the button is pressed. Port of SrPage in panel/main.py.
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "app.h"
#include "conf.h"
#include "paths.h"
#include "proc.h"
#include "sr_install.h"
#include "sr_status.h"
#include "ui_badge.h"
#include "ui_editor.h"

namespace {

constexpr const char *kVlcRunning =
    "VLC is running. It rewrites vlcrc when it exits and would discard these changes. Close VLC first.";

struct StatusData {
  SrStatus status;
  std::string error;  // non-empty: the installation could not be read
};

// Runs work() on a GLib worker thread, then done(result) on the main loop. work must not touch GTK or throw.
template <class R>
void run_async(std::function<R()> work, std::function<void(R)> done) {
  struct Job {
    std::function<R()> work;
    std::function<void(R)> done;
    R result{};
  };
  auto *job = new Job{std::move(work), std::move(done), R{}};
  GTask *task = g_task_new(nullptr, nullptr, +[](GObject *, GAsyncResult *res, gpointer) {
    std::unique_ptr<Job> j(static_cast<Job *>(g_task_get_task_data(G_TASK(res))));
    j->done(std::move(j->result));
  }, nullptr);
  g_task_set_task_data(task, job, nullptr);
  g_task_run_in_thread(task, +[](GTask *t, gpointer, gpointer data, GCancellable *) {
    auto *j = static_cast<Job *>(data);
    j->result = j->work();
    g_task_return_boolean(t, TRUE);
  });
  g_object_unref(task);
}

bool executable(const std::string &path) { return !path.empty() && access(path.c_str(), X_OK) == 0; }

StatusData load_status(const std::string &app_dir) {
  StatusData d;
  try {
    const InstallInfo info = read_install_json();
    const auto rec = info.paths.find("private_mpv");
    const std::string mpv = rec != info.paths.end() && executable(rec->second) ? rec->second : private_mpv();
    d.status = compute_status(info, repo_dir(app_dir), executable(mpv) ? mpv_has_vapoursynth(mpv) : std::nullopt);
  } catch (const std::exception &e) {
    d.error = e.what();
  }
  return d;
}

AdwPreferencesGroup *group(const char *title, const char *description = nullptr) {
  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(g, title);
  if (description) adw_preferences_group_set_description(g, description);
  return g;
}

GtkWidget *chip(const char *text, bool good) {
  GtkWidget *l = gtk_label_new(text);
  gtk_widget_add_css_class(l, "nvc-chip");
  gtk_widget_add_css_class(l, good ? "nvc-chip-ok" : "nvc-chip-warn");
  gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
  return l;
}

// A row with a check or warning icon; paths and hints contain "<" and "~", so markup is off.
GtkWidget *status_row(const std::string &title, const std::string &subtitle, bool good) {
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
  adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
  GtkWidget *icon = gtk_image_new_from_icon_name(good ? "nvc-check-symbolic" : "nvc-warning-symbolic");
  gtk_widget_add_css_class(icon, good ? "nvc-ok" : "nvc-warn");
  adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);
  return row;
}

// The widget the editors name after its option key or button ("osd_size", "nvc-save"; see ui_editor.h).
GtkWidget *find_named(GtkWidget *w, const char *name) {
  if (std::strcmp(gtk_widget_get_name(w), name) == 0) return w;
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
    if (GtkWidget *r = find_named(c, name)) return r;
  return nullptr;
}

GtkWidget *need_named(GtkWidget *w, const char *name) {
  GtkWidget *r = find_named(w, name);
  if (!r) throw std::runtime_error(std::string("widget not found: ") + name);
  return r;
}

// Re-reads the file behind an editor on show/refresh, unless the user has unsaved edits (Revert is the editor's reload).
void reload_if_clean(GtkWidget *editor) {
  GtkWidget *save = find_named(editor, "nvc-save"), *revert = find_named(editor, "nvc-revert");
  if (save && revert && !gtk_widget_get_sensitive(save)) g_signal_emit_by_name(revert, "clicked");
}

struct Row {
  std::string id;  // "headline", "error" or a check id
  GtkWidget *widget;
  bool good;
};

struct Sr {
  explicit Sr(App &a) : app(a) {}

  App &app;
  AdwPreferencesGroup *install = nullptr;
  GtkWidget *check_btn = nullptr, *spinner = nullptr, *result = nullptr, *preview = nullptr;
  GtkWidget *mpv_editor = nullptr, *vlc_editor = nullptr;
  std::vector<Row> rows;
  std::optional<SrStatus> status;
  bool busy = false, probing = false;
  // `nvcontrol --smoke` only: smoke_out is filled once the status is in, probe_out once the probe finished.
  std::map<std::string, std::string> *smoke_out = nullptr, *probe_out = nullptr;

  GtkWidget *build();
  void refresh();
  void show_status(const StatusData &d);
  void probe();
  void probe_done(const ProbeResult &r);
  void smoke_report();
};

GtkWidget *Sr::build() {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);

  install = group("Installation");
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(install));

  AdwPreferencesGroup *check = group("Check");
  GtkWidget *check_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(check_row), "Check that super resolution works");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(check_row),
                              "Loads the NVIDIA libraries and checks the GPU. This wakes the GPU for a moment.");
  spinner = gtk_spinner_new();
  gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
  check_btn = gtk_button_new_with_label("Check SR works");
  gtk_widget_set_valign(check_btn, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(check_btn, "suggested-action");
  gtk_widget_set_sensitive(check_btn, FALSE);  // until the status says where libnvvsr is
  g_signal_connect(check_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) { static_cast<Sr *>(d)->probe(); }), this);
  adw_action_row_add_suffix(ADW_ACTION_ROW(check_row), spinner);
  adw_action_row_add_suffix(ADW_ACTION_ROW(check_row), check_btn);
  adw_preferences_group_add(check, check_row);
  result = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(result), 0);
  gtk_label_set_wrap(GTK_LABEL(result), TRUE);
  gtk_label_set_selectable(GTK_LABEL(result), TRUE);
  gtk_widget_set_margin_top(result, 6);
  gtk_widget_set_visible(result, FALSE);
  adw_preferences_group_add(check, result);  // non-row children go under the list
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(check));

  AdwPreferencesGroup *pv = group("Badge preview", "A 1280x720 frame with the badge drawn at 2x size.");
  preview = make_badge_preview();
  GtkWidget *pv_clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(pv_clamp), 520);
  adw_clamp_set_child(ADW_CLAMP(pv_clamp), preview);
  adw_preferences_group_add(pv, pv_clamp);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(pv));

  ConfEditorSpec mpv;
  mpv.path = mpv_vsr_conf();
  mpv.options = mpv_options();
  mpv.title = "mpv (vsr.conf)";
  mpv.on_change = [this](const std::map<std::string, Value> &v) {
    update_badge_preview(preview, std::get<bool>(v.at("osd")), std::get<int>(v.at("osd_size")),
                         std::get<double>(v.at("osd_opacity")), std::get<int>(v.at("osd_position")));
  };
  mpv_editor = make_conf_editor(app, mpv);
  gtk_box_append(GTK_BOX(box), mpv_editor);

  ConfEditorSpec vlc;
  vlc.path = vlcrc();
  vlc.options = vlc_options();
  vlc.section = "nvvsr";
  vlc.bool_style = BoolStyle::ZeroOne;
  vlc.title = "VLC (vlcrc)";
  // VLC rewrites vlcrc when it exits, so an edit made while it runs would be lost.
  vlc.blocked = []() -> std::string { return run({"pgrep", "-x", "vlc"}, 3.0).ok() ? kVlcRunning : ""; };
  vlc_editor = make_conf_editor(app, vlc);
  gtk_box_append(GTK_BOX(box), vlc_editor);

  AdwPreferencesGroup *browsers = group("Browsers");
  GtkWidget *soon = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(soon), "Browser (in-page WebGPU)");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(soon), "Coming soon");
  gtk_widget_set_sensitive(soon, FALSE);
  adw_preferences_group_add(browsers, soon);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(browsers));

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
  return scroll;
}

// ---- status ----------------------------------------------------------------------------------------------------

void Sr::refresh() {
  reload_if_clean(mpv_editor);
  reload_if_clean(vlc_editor);
  if (busy) return;
  busy = true;
  run_async<StatusData>([dir = app.app_dir] { return load_status(dir); }, [this](StatusData d) {
    busy = false;
    show_status(d);
    if (smoke_out) smoke_report();
  });
}

void Sr::show_status(const StatusData &d) {
  for (const Row &r : rows) adw_preferences_group_remove(install, r.widget);
  rows.clear();
  gtk_widget_set_sensitive(check_btn, !probing);
  auto add = [this](const std::string &id, GtkWidget *w, bool good) {
    adw_preferences_group_add(install, w);
    rows.push_back({id, w, good});
  };
  if (!d.error.empty()) {
    status.reset();
    add("error", status_row("Could not read the installation", d.error, false), false);
    return;
  }
  const SrStatus &st = status.emplace(d.status);
  const std::string mode = st.mode == "installed" ? "Installed" + (st.version ? " (version " + *st.version + ")" : "")
                         : st.mode == "dev"       ? "Development checkout (build directories)"
                                                  : "Not installed";
  const bool good = st.mpv_ready() || st.vlc_ready();
  GtkWidget *head = status_row(st.headline(), mode, good);
  adw_action_row_add_suffix(ADW_ACTION_ROW(head), chip(st.mpv_ready() ? "mpv ready" : "mpv incomplete", st.mpv_ready()));
  adw_action_row_add_suffix(ADW_ACTION_ROW(head), chip(st.vlc_ready() ? "VLC ready" : "VLC incomplete", st.vlc_ready()));
  add("headline", head, good);
  for (const Check &c : st.checks) {
    const bool ok = c.ok == true;
    add(c.id, status_row(c.label, ok || c.hint.empty() ? c.detail : c.hint, ok), ok);
  }
}

// ---- probe -----------------------------------------------------------------------------------------------------

void Sr::probe() {
  if (probing) return;
  probing = true;
  gtk_widget_set_sensitive(check_btn, FALSE);
  gtk_spinner_start(GTK_SPINNER(spinner));
  gtk_widget_set_visible(result, FALSE);
  std::string lib;  // the status detail is "<path>" or "<path> (development build)"
  if (status && status->ok("libnvvsr")) {
    lib = status->check("libnvvsr").detail;
    const std::string tag = " (development build)";
    if (lib.size() > tag.size() && lib.compare(lib.size() - tag.size(), tag.size(), tag) == 0) lib.resize(lib.size() - tag.size());
  }
  run_async<ProbeResult>(
      [lib] {
        try {
          return run_probe(lib, libdir_env());
        } catch (const std::exception &e) {
          return ProbeResult{false, "The check failed", e.what()};
        }
      },
      [this](ProbeResult r) { probe_done(r); });
}

void Sr::probe_done(const ProbeResult &r) {
  probing = false;
  gtk_spinner_stop(GTK_SPINNER(spinner));
  gtk_widget_set_sensitive(check_btn, TRUE);
  const std::string text = r.summary + (r.detail.empty() ? "" : ": " + r.detail);
  gtk_label_set_label(GTK_LABEL(result), text.c_str());
  gtk_widget_remove_css_class(result, r.ok ? "nvc-error" : "nvc-ok");
  gtk_widget_add_css_class(result, r.ok ? "nvc-ok" : "nvc-error");
  gtk_widget_set_visible(result, TRUE);
  if (probe_out) {
    (*probe_out)["sr_probe"] = text;
    (*probe_out)["sr_probe_ok"] = r.ok ? "true" : "false";
    probe_out = nullptr;
  }
}

// ---- `nvcontrol --smoke` ---------------------------------------------------------------------------------------

// Sets one spin row of an editor, presses Save and returns the editor's message line.
std::string smoke_edit(GtkWidget *editor, const char *spin, double value) {
  adw_spin_row_set_value(ADW_SPIN_ROW(need_named(editor, spin)), value);
  g_signal_emit_by_name(need_named(editor, "nvc-save"), "clicked");
  return gtk_label_get_label(GTK_LABEL(need_named(editor, "nvc-message")));
}

// Reports what the status card shows. NVC_SMOKE_EDIT=1 changes osd_size (mpv) and nvvsr-min-height (VLC) through the
// editors and reports their messages; NVC_SMOKE_PROBE=1 presses "Check SR works". Without them nothing is written and
// the GPU probe never runs.
void Sr::smoke_report() {
  auto &o = *std::exchange(smoke_out, nullptr);
  for (const Row &r : rows)
    o["sr_row_" + r.id] = std::string(r.good ? "ok | " : "missing | ") +
                          adw_preferences_row_get_title(ADW_PREFERENCES_ROW(r.widget)) + " | " +
                          adw_action_row_get_subtitle(ADW_ACTION_ROW(r.widget));
  o["sr_check_button_sensitive"] = gtk_widget_get_sensitive(check_btn) ? "true" : "false";
  if (g_getenv("NVC_SMOKE_EDIT")) {
    o["sr_mpv_message"] = smoke_edit(mpv_editor, "osd_size", 20);
    o["sr_vlc_message"] = smoke_edit(vlc_editor, "nvvsr-min-height", 480);
  }
  if (g_getenv("NVC_SMOKE_PROBE")) {
    probe_out = &o;
    g_signal_emit_by_name(check_btn, "clicked");
  }
}

}  // namespace

GtkWidget *make_sr_page(App &app) {
  Sr *p = new Sr(app);  // lives as long as the process (one window)
  GtkWidget *page = p->build();
  PageHooks &h = app.hooks[kPageSr];
  h.on_show = [p] { p->refresh(); };  // also serves F5: on_refresh is unset
  h.on_smoke = [p](std::map<std::string, std::string> &out) {
    p->smoke_out = &out;
    if (!p->busy) p->smoke_report();
  };
  return page;
}
