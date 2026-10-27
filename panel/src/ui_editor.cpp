#include "ui_editor.h"

#include <adwaita.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#include "app.h"
#include "paths.h"

namespace {

using Values = std::map<std::string, Value>;

double num(const Value &v) {
  if (const bool *b = std::get_if<bool>(&v)) return *b ? 1 : 0;
  if (const int *i = std::get_if<int>(&v)) return *i;
  if (const double *d = std::get_if<double>(&v)) return *d;
  return 0;
}

// 2 == 2.0 == true-ish, but "" never equals a number (same rule save_changes uses).
bool same(const Value &a, const Value &b) {
  const bool sa = std::holds_alternative<std::string>(a), sb = std::holds_alternative<std::string>(b);
  if (sa || sb) return sa && sb && std::get<std::string>(a) == std::get<std::string>(b);
  return num(a) == num(b);
}

std::string stripped(const char *s) {
  std::string t = s ? s : "";
  t.erase(0, t.find_first_not_of(" \t"));
  t.erase(t.find_last_not_of(" \t") + 1);
  return t;
}

std::string short_path(const std::string &p) {
  const std::string h = home();
  return !h.empty() && p.compare(0, h.size() + 1, h + "/") == 0 ? "~" + p.substr(h.size()) : p;
}

std::string general(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

struct Ctl {
  explicit Ctl(const Opt *o) : opt(o) {}
  const Opt *opt;
  GtkWidget *row = nullptr;
  GtkWidget *reset = nullptr;
  GtkStringList *model = nullptr;  // combo rows: labels, parallel to `values`
  std::vector<std::string> values;
};

struct Editor : std::enable_shared_from_this<Editor> {
  Editor(App &a, ConfEditorSpec s) : app(a), spec(std::move(s)) {}

  App &app;
  ConfEditorSpec spec;  // owns the options the Ctl entries point to: never modified after construction
  GtkWidget *root = nullptr, *group = nullptr, *message = nullptr, *save_btn = nullptr, *revert_btn = nullptr;
  std::vector<Ctl> ctls;
  Loaded loaded;
  Values baseline;  // what the controls read right after the last load; dirty = differs from it
  bool loading = false;

  GtkWidget *make_row(const Opt &o);
  Value read(const Ctl &c) const;
  void write(Ctl &c, const Value &v);
  Values values() const;
  std::string validate(const Values &v) const;
  void update_state();
  void reload();
  void save(bool force);
  void ask_conflict(const std::vector<std::string> &keys);
  void set_message(const std::string &text, bool error);
};

void on_notify(GObject *, GParamSpec *, gpointer d) {
  Editor *e = static_cast<Editor *>(d);
  if (!e->loading) e->update_state();
}
void on_changed(GtkEditable *, gpointer d) { on_notify(nullptr, nullptr, d); }

// ---------------------------------------------------------------------------------------------------------------
// rows
// ---------------------------------------------------------------------------------------------------------------

GtkWidget *Editor::make_row(const Opt &o) {
  Ctl c(&o);
  switch (o.kind) {
    case Kind::Bool:
      c.row = adw_switch_row_new();
      g_signal_connect(c.row, "notify::active", G_CALLBACK(on_notify), this);
      break;
    case Kind::Enum:
    case Kind::EnumInt:
    case Kind::Choice:
      c.row = adw_combo_row_new();
      c.model = gtk_string_list_new(nullptr);
      for (const ChoiceItem &ci : o.choices) {
        gtk_string_list_append(c.model, ci.label.c_str());
        c.values.push_back(ci.value);
      }
      adw_combo_row_set_model(ADW_COMBO_ROW(c.row), G_LIST_MODEL(c.model));
      g_object_unref(c.model);  // the row keeps it
      g_signal_connect(c.row, "notify::selected", G_CALLBACK(on_notify), this);
      break;
    case Kind::Int:
    case Kind::Float: {
      const bool is_int = o.kind == Kind::Int;
      const double lo = std::max(o.lo, -1e9), hi = std::min(o.hi, 1e9), step = is_int ? 1 : (hi <= 1 ? 0.05 : 0.5);
      c.row = adw_spin_row_new(gtk_adjustment_new(num(o.def), lo, hi, step, step * 10, 0), 1,
                               is_int ? 0 : std::max(o.digits, 1));
      g_signal_connect(c.row, "notify::value", G_CALLBACK(on_notify), this);
      break;
    }
    case Kind::OptFloat:
    case Kind::Str:
      c.row = adw_entry_row_new();
      g_signal_connect(c.row, "changed", G_CALLBACK(on_changed), this);
      break;
  }
  std::string title = o.label;
  if (o.kind == Kind::OptFloat) title += " (" + general(o.lo) + " to " + general(o.hi) + ", empty = default)";
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(c.row), title.c_str());
  gtk_widget_set_name(c.row, o.key.c_str());
  gtk_widget_set_tooltip_text(c.row, o.help.c_str());
  gtk_accessible_update_property(GTK_ACCESSIBLE(c.row), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, o.help.c_str(), -1);

  c.reset = gtk_button_new_with_label("Default");
  gtk_widget_add_css_class(c.reset, "flat");
  gtk_widget_set_valign(c.reset, GTK_ALIGN_CENTER);
  gtk_widget_set_name(c.reset, ("default-" + o.key).c_str());
  const std::string def = default_text(o, spec.bool_style);
  gtk_widget_set_tooltip_text(c.reset, ("Back to the default" + (def.empty() ? std::string(" (empty)") : " (" + def + ")")).c_str());
  gtk_accessible_update_property(GTK_ACCESSIBLE(c.reset), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 ("Reset " + o.label + " to its default").c_str(), -1);
  g_object_set_data(G_OBJECT(c.reset), "nvc-opt", const_cast<Opt *>(&o));
  g_signal_connect(c.reset, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer d) {
                     Editor *e = static_cast<Editor *>(d);
                     const Opt *opt = static_cast<const Opt *>(g_object_get_data(G_OBJECT(b), "nvc-opt"));
                     for (Ctl &x : e->ctls)
                       if (x.opt == opt) e->write(x, opt->def);
                   }), this);
  // faded out, not hidden: a button that appears would push the row's own controls sideways under the pointer
  gtk_widget_set_sensitive(c.reset, FALSE);
  gtk_widget_set_opacity(c.reset, 0);
  if (ADW_IS_ENTRY_ROW(c.row)) adw_entry_row_add_suffix(ADW_ENTRY_ROW(c.row), c.reset);
  else adw_action_row_add_suffix(ADW_ACTION_ROW(c.row), c.reset);

  GtkWidget *row = c.row;
  ctls.push_back(std::move(c));
  return row;
}

Value Editor::read(const Ctl &c) const {
  const Opt &o = *c.opt;
  switch (o.kind) {
    case Kind::Bool: return adw_switch_row_get_active(ADW_SWITCH_ROW(c.row)) != FALSE;
    case Kind::Int: return static_cast<int>(std::lround(adw_spin_row_get_value(ADW_SPIN_ROW(c.row))));
    case Kind::Float: return std::round(adw_spin_row_get_value(ADW_SPIN_ROW(c.row)) * 1e4) / 1e4;
    case Kind::Enum:
    case Kind::EnumInt:
    case Kind::Choice: {
      const guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(c.row));
      if (i >= c.values.size()) return o.def;
      if (o.kind == Kind::EnumInt) return std::atoi(c.values[i].c_str());
      return c.values[i];
    }
    case Kind::OptFloat: {
      const std::string t = stripped(gtk_editable_get_text(GTK_EDITABLE(c.row)));
      if (t.empty()) return std::string();
      try {
        return parse_value(o, t);
      } catch (const std::invalid_argument &) {
        return t;  // not a number: validate() reports it before anything is written
      }
    }
    case Kind::Str: return stripped(gtk_editable_get_text(GTK_EDITABLE(c.row)));
  }
  return o.def;
}

void Editor::write(Ctl &c, const Value &v) {
  const Opt &o = *c.opt;
  switch (o.kind) {
    case Kind::Bool: adw_switch_row_set_active(ADW_SWITCH_ROW(c.row), num(v) != 0); break;
    case Kind::Int:
    case Kind::Float: adw_spin_row_set_value(ADW_SPIN_ROW(c.row), num(v)); break;
    case Kind::Enum:
    case Kind::EnumInt:
    case Kind::Choice: {
      const std::string t = std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : format_value(o, v);
      auto it = std::find(c.values.begin(), c.values.end(), t);
      if (it == c.values.end()) {  // a value in the file that is not one of the presets stays selectable
        gtk_string_list_append(c.model, t.c_str());
        c.values.push_back(t);
        it = c.values.end() - 1;
      }
      adw_combo_row_set_selected(ADW_COMBO_ROW(c.row), static_cast<guint>(it - c.values.begin()));
      break;
    }
    case Kind::OptFloat:
    case Kind::Str:
      gtk_editable_set_text(GTK_EDITABLE(c.row),
                            (std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : format_value(o, v)).c_str());
      break;
  }
}

Values Editor::values() const {
  Values out;
  for (const Ctl &c : ctls) out[c.opt->key] = read(c);
  return out;
}

// What would be refused later anyway, said with the option's name.
std::string Editor::validate(const Values &v) const {
  for (const Ctl &c : ctls) {
    const Opt &o = *c.opt;
    const Value &x = v.at(o.key);
    if (o.kind == Kind::OptFloat && std::holds_alternative<std::string>(x) && !std::get<std::string>(x).empty())
      return o.label + ": enter a number between " + general(o.lo) + " and " + general(o.hi) + ", or leave it empty";
    if (o.kind == Kind::Str && std::get<std::string>(x).find_first_of("\r\n") != std::string::npos)
      return o.label + ": a single line is expected";
  }
  return "";
}

// ---------------------------------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------------------------------

void Editor::update_state() {
  const Values now = values();
  bool dirty = false;
  for (const auto &[k, v] : now) dirty = dirty || !same(v, baseline.at(k));
  gtk_widget_set_sensitive(save_btn, dirty);
  gtk_widget_set_sensitive(revert_btn, dirty);
  for (const Ctl &c : ctls) {
    const bool off_default = !same(now.at(c.opt->key), c.opt->def);
    gtk_widget_set_sensitive(c.reset, off_default);
    gtk_widget_set_opacity(c.reset, off_default ? 1 : 0);
  }
  if (spec.on_change) spec.on_change(now);
}

void Editor::set_message(const std::string &text, bool error) {
  gtk_label_set_label(GTK_LABEL(message), text.c_str());
  gtk_widget_remove_css_class(message, error ? "nvc-hint" : "nvc-error");
  gtk_widget_add_css_class(message, error ? "nvc-error" : "nvc-hint");
}

void Editor::reload() {
  loaded = load_settings(spec.path, spec.options);
  loading = true;
  for (Ctl &c : ctls) write(c, loaded.values.at(c.opt->key));
  loading = false;
  baseline = values();

  const std::string note = short_path(spec.path) + (loaded.exists ? "" : " (not created yet, saving creates it)");
  gchar *markup = g_markup_escape_text(note.c_str(), -1);
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), markup);
  g_free(markup);

  if (!loaded.error.empty()) {
    set_message("Could not read " + short_path(spec.path) + ": " + loaded.error + ". Saving is disabled.", true);
  } else if (!loaded.problems.empty()) {
    std::string t = loaded.problems[0];
    if (loaded.problems.size() > 1) t += "; " + loaded.problems[1];
    set_message(t + " - shown as default", true);
  } else {
    set_message("", false);
  }
  update_state();
}

void Editor::save(bool force) {
  const Values v = values();
  if (const std::string err = validate(v); !err.empty()) return set_message(err, true);
  if (spec.blocked) {
    if (const std::string why = spec.blocked(); !why.empty()) return set_message(why, true);
  }
  const SaveResult r = save_changes(spec.path, spec.options, v, loaded, spec.section, spec.bool_style, force);
  if (!r.conflicts.empty()) {
    set_message("The file changed on disk since it was loaded.", true);
    return ask_conflict(r.conflicts);
  }
  if (!r.ok) return set_message(r.error.empty() ? "Could not save." : r.error, true);
  reload();
  set_message("Saved." + (r.backup.empty() ? "" : " Original kept as " + short_path(r.backup) + "."), false);
  app.toast("Saved " + spec.title);
  if (spec.on_saved) spec.on_saved();
}

void Editor::ask_conflict(const std::vector<std::string> &keys) {
  std::string names;
  for (const std::string &k : keys)
    for (const Opt &o : spec.options)
      if (o.key == k) names += (names.empty() ? "" : ", ") + o.label;
  const std::string file = short_path(spec.path);
  AdwDialog *d = adw_alert_dialog_new("File changed outside NV Control", nullptr);
  adw_alert_dialog_set_body(ADW_ALERT_DIALOG(d),
                            (file + " was edited since it was loaded (" + names +
                             "). Reload to see the current values, or overwrite just those keys with yours.").c_str());
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "reload", "Reload", "overwrite", "Overwrite those keys", nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "overwrite", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "reload");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "reload");
  // the dialog may outlive the window: the callback only holds a weak reference
  adw_alert_dialog_choose(
      ADW_ALERT_DIALOG(d), root, nullptr,
      [](GObject *dialog, GAsyncResult *res, gpointer data) {
        std::unique_ptr<std::weak_ptr<Editor>> weak(static_cast<std::weak_ptr<Editor> *>(data));
        const char *answer = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), res);
        if (const auto e = weak->lock()) {
          if (answer && std::string(answer) == "overwrite") e->save(true);
          else e->reload();
        }
      },
      new std::weak_ptr<Editor>(shared_from_this()));
}

}  // namespace

GtkWidget *make_conf_editor(App &app, const ConfEditorSpec &spec) {
  auto ed = std::make_shared<Editor>(app, spec);
  Editor *e = ed.get();

  e->group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(e->group), spec.title.c_str());
  GtkWidget *advanced = nullptr;
  for (const Opt &o : e->spec.options) {
    if (!o.advanced) {
      adw_preferences_group_add(ADW_PREFERENCES_GROUP(e->group), e->make_row(o));
      continue;
    }
    if (!advanced) {
      advanced = adw_expander_row_new();
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(advanced), "Advanced");
    }
    adw_expander_row_add_row(ADW_EXPANDER_ROW(advanced), e->make_row(o));
  }
  if (advanced) adw_preferences_group_add(ADW_PREFERENCES_GROUP(e->group), advanced);

  e->message = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(e->message), 0);
  gtk_label_set_wrap(GTK_LABEL(e->message), TRUE);
  gtk_widget_set_hexpand(e->message, TRUE);
  gtk_widget_set_valign(e->message, GTK_ALIGN_CENTER);
  gtk_widget_set_name(e->message, "nvc-message");
  e->revert_btn = gtk_button_new_with_label("Revert");
  gtk_widget_set_name(e->revert_btn, "nvc-revert");
  e->save_btn = gtk_button_new_with_label("Save");
  gtk_widget_set_name(e->save_btn, "nvc-save");
  gtk_widget_add_css_class(e->save_btn, "suggested-action");
  g_signal_connect(e->revert_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
                     static_cast<Editor *>(d)->reload();
                   }), e);
  g_signal_connect(e->save_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
                     static_cast<Editor *>(d)->save(false);
                   }), e);
  GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(foot, 6);
  gtk_box_append(GTK_BOX(foot), e->message);
  gtk_box_append(GTK_BOX(foot), e->revert_btn);
  gtk_box_append(GTK_BOX(foot), e->save_btn);

  e->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_box_append(GTK_BOX(e->root), e->group);
  gtk_box_append(GTK_BOX(e->root), foot);
  g_object_set_data_full(G_OBJECT(e->root), "nvc-editor", new std::shared_ptr<Editor>(ed),
                         [](gpointer p) { delete static_cast<std::shared_ptr<Editor> *>(p); });
  e->reload();
  return e->root;
}
