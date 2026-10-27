// Where things live. Everything is resolved at call time so tests can point HOME / NVC_SYSFS_ROOT elsewhere.
#pragma once

#include <optional>
#include <string>

std::string home();
std::string sysfs_root();
std::string config_home();
std::string data_home();
std::string state_home();
// Expands a leading ~ against HOME (not the passwd entry).
std::string expand(const std::string &path);

std::string mpv_vsr_conf();
std::string vlcrc();
std::string nvvsr_libdir_file();
std::string panel_settings_file();
std::string install_json();
std::string vapoursynth_toml();
std::string scheme_json();
std::string private_mpv();

// The development checkout this app lives in (or NVC_REPO_DIR), or nullopt.
std::optional<std::string> repo_dir(const std::string &app_dir);
