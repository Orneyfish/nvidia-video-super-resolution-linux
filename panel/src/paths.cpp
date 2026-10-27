#include "paths.h"

#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace {

// The variable's value, or "" when unset or empty (matches the Python `or` fallbacks).
std::string env_value(const char *name) {
  const char *v = getenv(name);
  return v ? v : "";
}

// Like os.path.join for two parts.
std::string join(const std::string &a, const std::string &b) {
  if (!b.empty() && b[0] == '/') return b;
  if (a.empty() || a.back() == '/') return a + b;
  return a + "/" + b;
}

// An XDG base directory: the variable when it is an absolute path, else $HOME/<fallback>.
std::string xdg_dir(const char *var, const std::string &fallback) {
  const std::string x = env_value(var);
  return !x.empty() && x[0] == '/' ? x : join(home(), fallback);
}

}  // namespace

std::string home() {
  const std::string h = env_value("HOME");
  if (!h.empty()) return h;
  const passwd *pw = getpwuid(getuid());
  return pw ? pw->pw_dir : "";
}

std::string sysfs_root() {
  const std::string r = env_value("NVC_SYSFS_ROOT");
  return r.empty() ? "/sys" : r;
}

std::string config_home() { return xdg_dir("XDG_CONFIG_HOME", ".config"); }
std::string data_home() { return xdg_dir("XDG_DATA_HOME", join(".local", "share")); }
std::string state_home() { return xdg_dir("XDG_STATE_HOME", join(".local", "state")); }

std::string expand(const std::string &path) {
  if (path == "~") return home();
  if (path.rfind("~/", 0) == 0) return join(home(), path.substr(2));
  return path;
}

std::string mpv_vsr_conf() { return join(join(join(config_home(), "mpv"), "script-opts"), "vsr.conf"); }
std::string vlcrc() { return join(join(config_home(), "vlc"), "vlcrc"); }
std::string nvvsr_libdir_file() { return join(join(config_home(), "nvvsr"), "libdir"); }
std::string panel_settings_file() { return join(join(config_home(), "nvvsr"), "panel.json"); }
std::string install_json() { return join(join(data_home(), "nvvsr"), "install.json"); }
std::string vapoursynth_toml() { return join(join(config_home(), "vapoursynth"), "vapoursynth.toml"); }
std::string scheme_json() { return join(join(state_home(), "caelestia"), "scheme.json"); }
std::string private_mpv() { return join(join(join(join(join(home(), ".local"), "opt"), "mpv-vsr"), "bin"), "mpv"); }

std::optional<std::string> repo_dir(const std::string &app_dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const std::string forced = env_value("NVC_REPO_DIR");
  if (!forced.empty()) {
    if (fs::is_directory(forced, ec)) return forced;
    return std::nullopt;
  }
  // The app lives in <repo>/panel, so the checkout is the parent of the app directory.
  fs::path p = fs::absolute(app_dir).lexically_normal();
  if (p.filename().empty()) p = p.parent_path();
  const fs::path cand = p.parent_path();
  if (fs::is_regular_file(cand / "core" / "include" / "nvvsr.h", ec)) return cand.string();
  return std::nullopt;
}
