// install.json (schema 1) and the libdir / vapoursynth.toml checks behind the panel's SR status.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// install.json written by install/install.sh. Unknown keys are ignored.
struct InstallInfo {
  std::string path;
  bool present = false;
  std::optional<std::string> error;                                      // present but unusable
  std::map<std::string, std::string> paths;                              // the string-valued entries of "paths"
  std::optional<std::string> version;
  std::map<std::string, std::pair<std::string, std::string>> components;  // name -> (status, detail)
};

// An empty path means the default location (paths::install_json()).
InstallInfo read_install_json(const std::string &path = "");

// The directories in the libdir file: first meaningful line, colon separated, expanded. nullopt when unreadable.
std::optional<std::vector<std::string>> read_libdir(const std::string &path = "");
// The NVVSR_LIBDIR value for the probe, or nullopt when there are no directories.
std::optional<std::string> libdir_env(const std::string &path = "");

struct VsscriptEntry {
  bool present;
  std::string detail;  // the matching key, or why there is none
};
VsscriptEntry vsscript_entry(const std::string &toml_path = "");
