// The app's own small settings file (<config>/nvvsr/panel.json). No GTK, so the library and its tests use it.
#pragma once

#include <string>

#include "paths.h"

struct Settings {
  static constexpr int kPollChoices[] = {0, 1, 2, 5, 10};  // seconds, 0 = manual
  static constexpr int kDefaultPoll = 2;

  int poll_interval = kDefaultPoll;

  // A missing, broken or out-of-range file leaves the defaults.
  void load(const std::string &path = panel_settings_file());
  // Atomic; writes through a symlinked file (dotfile managers). False when it could not be written.
  bool save(const std::string &path = panel_settings_file()) const;
};
