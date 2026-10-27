// The About page: version rows (collected on a worker thread), "Copy diagnostics" and the README row. Port of
// AboutPage in panel/main.py. Never runs nvidia-smi: the report reads sysfs only.
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "app.h"
#include "conf.h"
#include "diag.h"
#include "gpu_sysfs.h"
#include "paths.h"
#include "smi_csv.h"
#include "sr_install.h"
#include "sr_status.h"

namespace {

// The nvc project version (panel/CMakeLists.txt).
constexpr const char *kAppVersion = "0.1.0";

// The version rows in display order; collect_versions() supplies the values by these names.
const char *const kVersionRows[] = {"NV Control", "libnvvsr", "NVIDIA kernel driver", "NVIDIA VFX wheel",
                                    "Private mpv", "System mpv", "VLC", "GTK", "libadwaita", "Kernel"};

using Toolkit = std::vector<std::pair<std::string, std::string>>;

struct About {
  App *app = nullptr;
  std::string app_dir, readme;
  Toolkit toolkit;
  std::map<std::string, GtkWidget *> values;  // version row name -> its value label
  GtkWidget *copy_button = nullptr;
  bool versions_busy = false, copy_busy = false;
  // `nvcontrol --smoke` only: where the copied report and the clipboard check go. Set by on_smoke, cleared when the
  // copy finishes (the smoke run ends seconds later, long after the copy).
  std::map<std::string, std::string> *smoke_out = nullptr;
};

About *about = nullptr;  // one window per process: the page state lives as long as the process

// Runs work() on a GLib worker thread, then done(result) on the main loop. work must not touch GTK.
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

std::string version3(guint major, guint minor, guint micro) {
  return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(micro);
}

std::string or_not_found(const std::optional<std::string> &v) { return v && !v->empty() ? *v : "not found"; }

// "  key = value" for every active assignment, or the note the report prints for a missing file.
std::vector<std::string> settings_lines(const std::string &path, const std::vector<Opt> &options) {
  const Loaded loaded = load_settings(path, options);
  if (!loaded.exists) return {"  (file does not exist, defaults apply)"};
  std::vector<std::string> out;
  for (const auto &[key, text] : loaded.raw)
    if (text) out.push_back("  " + key + " = " + *text);
  return out;  // empty: build_report prints "(no overrides, defaults apply)"
}

// Reads sysfs and the install state only; the GPU is not queried (no last poll sample yet: the report says so).
std::string diagnostics(const std::string &app_dir, const Toolkit &toolkit) {
  const SrStatus status = compute_status(read_install_json(), repo_dir(app_dir), std::nullopt);
  const std::vector<PciGpu> gpus = find_nvidia_gpus();
  ReportInput in;
  in.versions = collect_versions(status, kAppVersion, toolkit);
  in.status = status;
  in.runtime = read_runtime(gpus.empty() ? nullptr : &gpus.front());
  in.hybrid = supergfxctl_mode();
  in.power_source = power_source();
  in.mpv_settings = settings_lines(mpv_vsr_conf(), mpv_options());
  in.vlc_settings = settings_lines(vlcrc(), vlc_options());
  return build_report(in);
}

void refresh_versions() {
  if (about->versions_busy) return;
  about->versions_busy = true;
  run_async<Versions>(
      [dir = about->app_dir, tk = about->toolkit] {
        return collect_versions(compute_status(read_install_json(), repo_dir(dir), std::nullopt), kAppVersion, tk);
      },
      [](Versions v) {
        about->versions_busy = false;
        for (const auto &[name, version] : v) {
          const auto it = about->values.find(name);
          if (it != about->values.end()) gtk_label_set_label(GTK_LABEL(it->second), or_not_found(version).c_str());
        }
      });
}

struct SmokeClipboard {
  std::map<std::string, std::string> *out;
  std::string text;
};

void copy_diagnostics() {
  if (about->copy_busy) return;
  about->copy_busy = true;
  run_async<std::string>([dir = about->app_dir, tk = about->toolkit] { return diagnostics(dir, tk); },
                         [](std::string text) {
                           About &a = *about;
                           a.copy_busy = false;
                           GdkClipboard *clip = gdk_display_get_clipboard(gdk_display_get_default());
                           gdk_clipboard_set_text(clip, text.c_str());
                           a.app->toast("Diagnostics copied");
                           if (!a.smoke_out) return;
                           std::map<std::string, std::string> *out = a.smoke_out;
                           a.smoke_out = nullptr;
                           (*out)["report"] = text;
                           (*out)["report_length"] = std::to_string(text.size());
                           gdk_clipboard_read_text_async(
                               clip, nullptr,
                               +[](GObject *src, GAsyncResult *res, gpointer d) {
                                 std::unique_ptr<SmokeClipboard> c(static_cast<SmokeClipboard *>(d));
                                 char *got = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, nullptr);
                                 (*c->out)["clipboard_matches"] = (got && c->text == got) ? "yes" : "no";
                                 g_free(got);
                               },
                               new SmokeClipboard{out, text});
                         });
}

void open_readme() {
  GError *err = nullptr;
  char *uri = g_filename_to_uri(about->readme.c_str(), nullptr, &err);
  const bool ok = uri && g_app_info_launch_default_for_uri(uri, nullptr, &err);
  if (!ok) about->app->toast("Could not open the README");
  g_clear_error(&err);
  g_free(uri);
}

GtkWidget *group(const char *title) {
  GtkWidget *g = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), title);
  return g;
}

}  // namespace

GtkWidget *make_about_page(App &app) {
  about = new About;
  about->app = &app;
  about->app_dir = app.app_dir;
  about->toolkit = {{"GTK", version3(gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version())},
                    {"libadwaita", version3(adw_get_major_version(), adw_get_minor_version(), adw_get_micro_version())}};

  PageHooks &h = app.hooks[kPageAbout];
  h.on_show = refresh_versions;
  h.on_refresh = refresh_versions;
  h.on_smoke = [](std::map<std::string, std::string> &out) {
    out["version_rows"] = std::to_string(about->values.size());
    about->smoke_out = &out;
    gtk_widget_activate(about->copy_button);  // the same as a click
  };

  GtkWidget *versions = group("Versions");
  for (const char *name : kVersionRows) {
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
    GtkWidget *value = gtk_label_new("...");
    gtk_label_set_selectable(GTK_LABEL(value), TRUE);
    gtk_widget_add_css_class(value, "dim-label");
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), value);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(versions), row);
    about->values[name] = value;
  }

  GtkWidget *actions = group("Diagnostics");
  GtkWidget *copy_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(copy_row), "Copy diagnostics");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(copy_row),
                              "Plain text: versions, GPU state, install status and settings. No cookies or other "
                              "secrets; your home directory is shortened.");
  about->copy_button = gtk_button_new_with_label("Copy diagnostics");
  gtk_widget_set_valign(about->copy_button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(about->copy_button, "suggested-action");
  g_signal_connect(about->copy_button, "clicked", G_CALLBACK(+[](GtkButton *, gpointer) { copy_diagnostics(); }),
                   nullptr);
  adw_action_row_add_suffix(ADW_ACTION_ROW(copy_row), about->copy_button);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), copy_row);

  GtkWidget *readme_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(readme_row), "README");
  const auto repo = repo_dir(app.app_dir);
  std::error_code ec;
  if (repo && std::filesystem::is_regular_file(*repo + "/README.md", ec)) about->readme = *repo + "/README.md";
  const std::string subtitle =
      about->readme.empty() ? "No README.md found next to the application" : redact(about->readme);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(readme_row), subtitle.c_str());
  GtkWidget *open = gtk_button_new_with_label("Open");
  gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive(open, !about->readme.empty());
  g_signal_connect(open, "clicked", G_CALLBACK(+[](GtkButton *, gpointer) { open_readme(); }), nullptr);
  adw_action_row_add_suffix(ADW_ACTION_ROW(readme_row), open);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), readme_row);

  GtkWidget *hint = gtk_label_new(
      "NV Control is an independent tool for this machine. It is not affiliated with or endorsed by NVIDIA.");
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_label_set_xalign(GTK_LABEL(hint), 0);
  gtk_widget_add_css_class(hint, "dim-label");

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  for (GtkWidget *w : {versions, actions, hint}) {
    gtk_widget_set_margin_start(w, 16);
    gtk_widget_set_margin_end(w, 16);
    gtk_box_append(GTK_BOX(box), w);
  }
  gtk_widget_set_margin_top(box, 18);
  gtk_widget_set_margin_bottom(box, 18);

  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 600);
  adw_clamp_set_child(ADW_CLAMP(clamp), box);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
  return scroll;
}
