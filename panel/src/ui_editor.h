// A reusable editor for one native key=value config file (mpv's vsr.conf, VLC's vlcrc): one AdwPreferencesGroup of
// rows by option kind, the advanced options in an expander, a "Default" reset per row, Save and Revert, errors
// inline and a toast on success. Reading and writing is conf.h (comment-preserving, atomic, backup on first write).
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "conf.h"

struct App;

struct ConfEditorSpec {
  std::string path;
  std::vector<Opt> options;  // mpv_options() / vlc_options()
  std::string section;       // [section] new keys go into ("" = none)
  BoolStyle bool_style = BoolStyle::YesNo;
  std::string title;         // group title, also the toast: "Saved <title>"
  // Called on the main loop right before every write: a non-empty text means the file must not be written now
  // (shown inline). Keep it fast, e.g. "VLC is running: it rewrites vlcrc on exit".
  std::function<std::string()> blocked;
  std::function<void()> on_saved;  // after a successful save, the editor has already reloaded the file
  // After every edit and every reload, with the typed values currently shown (drives the badge preview).
  std::function<void(const std::map<std::string, Value> &)> on_change;
};

// The returned widget owns the editor state. Rows are named after their option key, the buttons "nvc-save" and
// "nvc-revert", the status line "nvc-message", the reset buttons "default-<key>" (the harness drives them by name).
GtkWidget *make_conf_editor(App &app, const ConfEditorSpec &spec);
