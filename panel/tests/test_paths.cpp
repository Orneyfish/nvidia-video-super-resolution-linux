#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "check.h"
#include "paths.h"

namespace fs = std::filesystem;

namespace {

// Known starting environment: HOME fixed, every override unset.
void reset_env() {
  setenv("HOME", "/h/u", 1);
  for (const char *v : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "NVC_SYSFS_ROOT", "NVC_REPO_DIR"})
    unsetenv(v);
}

}  // namespace

int main() {
  reset_env();
  CHECK_EQ(home(), std::string("/h/u"));
  CHECK_EQ(config_home(), std::string("/h/u/.config"));
  CHECK_EQ(data_home(), std::string("/h/u/.local/share"));
  CHECK_EQ(state_home(), std::string("/h/u/.local/state"));
  CHECK_EQ(sysfs_root(), std::string("/sys"));

  setenv("XDG_CONFIG_HOME", "/x/cfg", 1);
  setenv("XDG_DATA_HOME", "/x/data", 1);
  setenv("XDG_STATE_HOME", "/x/state", 1);
  setenv("NVC_SYSFS_ROOT", "/fake/sys", 1);
  CHECK_EQ(config_home(), std::string("/x/cfg"));
  CHECK_EQ(data_home(), std::string("/x/data"));
  CHECK_EQ(state_home(), std::string("/x/state"));
  CHECK_EQ(sysfs_root(), std::string("/fake/sys"));

  reset_env();
  setenv("XDG_CONFIG_HOME", "relative/cfg", 1);  // relative XDG values are ignored, as in the spec
  CHECK_EQ(config_home(), std::string("/h/u/.config"));
  setenv("XDG_DATA_HOME", "", 1);
  CHECK_EQ(data_home(), std::string("/h/u/.local/share"));
  setenv("NVC_SYSFS_ROOT", "", 1);
  CHECK_EQ(sysfs_root(), std::string("/sys"));

  CHECK_EQ(expand("~"), std::string("/h/u"));
  CHECK_EQ(expand("~/a/b"), std::string("/h/u/a/b"));
  CHECK_EQ(expand("/abs/x"), std::string("/abs/x"));
  CHECK_EQ(expand("~other/x"), std::string("~other/x"));

  reset_env();
  CHECK_EQ(mpv_vsr_conf(), std::string("/h/u/.config/mpv/script-opts/vsr.conf"));
  CHECK_EQ(vlcrc(), std::string("/h/u/.config/vlc/vlcrc"));
  CHECK_EQ(nvvsr_libdir_file(), std::string("/h/u/.config/nvvsr/libdir"));
  CHECK_EQ(panel_settings_file(), std::string("/h/u/.config/nvvsr/panel.json"));
  CHECK_EQ(install_json(), std::string("/h/u/.local/share/nvvsr/install.json"));
  CHECK_EQ(vapoursynth_toml(), std::string("/h/u/.config/vapoursynth/vapoursynth.toml"));
  CHECK_EQ(scheme_json(), std::string("/h/u/.local/state/caelestia/scheme.json"));
  CHECK_EQ(private_mpv(), std::string("/h/u/.local/opt/mpv-vsr/bin/mpv"));
  setenv("XDG_CONFIG_HOME", "/x/cfg", 1);
  CHECK_EQ(panel_settings_file(), std::string("/x/cfg/nvvsr/panel.json"));

  // repo_dir: NVC_REPO_DIR, then the checkout that holds core/include/nvvsr.h above the app directory.
  reset_env();
  const fs::path root = fs::temp_directory_path() / ("nvc-paths-test-" + std::to_string(getpid()));
  fs::create_directories(root / "repo" / "panel");
  fs::create_directories(root / "repo" / "core" / "include");
  { std::ofstream(root / "repo" / "core" / "include" / "nvvsr.h") << "/* test */\n"; }
  const std::string app = (root / "repo" / "panel").string();

  auto found = repo_dir(app);
  CHECK(found.has_value() && *found == (root / "repo").string(), "checkout found above the app dir");
  found = repo_dir(app + "/");
  CHECK(found.has_value() && *found == (root / "repo").string(), "trailing slash on the app dir");

  setenv("NVC_REPO_DIR", (root / "nowhere").c_str(), 1);
  CHECK(!repo_dir(app).has_value(), "forced NVC_REPO_DIR that does not exist gives nothing");
  setenv("NVC_REPO_DIR", (root / "repo").c_str(), 1);
  found = repo_dir("/unrelated/app");
  CHECK(found.has_value() && *found == (root / "repo").string(), "forced NVC_REPO_DIR wins");
  unsetenv("NVC_REPO_DIR");

  fs::remove(root / "repo" / "core" / "include" / "nvvsr.h");
  CHECK(!repo_dir(app).has_value(), "no nvvsr.h means no checkout");
  fs::remove_all(root);

  reset_env();
  return finish("test_paths");
}
