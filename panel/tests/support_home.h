// Header-only port of the install.json, libdir and vapoursynth.toml parts of panel/tests/support.py make_home,
// plus write(). The rest of make_home (repo build files, mpv, vlcrc, scheme) and the fake-bin helpers are not ported yet.
#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace support {

struct Home {
  std::string cfg;   // <home>/.config
  std::string vfx;   // the nvvsr_libdir entry for libnvVFXVideoSuperRes.so
  std::string wheel; // the nvvsr_libdir entry for the wheel's bundled libraries
};

// Writes text to path, creating parent directories. Returns path.
inline std::string write_file(const std::string &path, const std::string &text) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot write " + path);
  f << text;
  return path;
}

// A fake HOME with a wheel venv, a libdir file, a vapoursynth.toml and (when `repo` is given) an install.json
// that records the repo's build outputs. Paths are the ones install/install.sh would write.
inline Home make_home(const std::string &home, const std::string &repo = "") {
  Home h;
  h.cfg = home + "/.config";
  h.vfx = home + "/venv/site-packages/nvvfx/libs";
  h.wheel = home + "/venv/site-packages/nvidia_vfx.libs";
  const std::string share = home + "/.local/share/nvvsr";
  const std::string mpv = home + "/.local/opt/mpv-vsr/bin/mpv";
  write_file(h.vfx + "/libnvVFXVideoSuperRes.so", "");
  write_file(h.wheel + "/libcudart.so.12", "");
  write_file(home + "/venv/site-packages/nvidia_vfx-0.2.0.0.dist-info/METADATA",
             "Metadata-Version: 2.2\nName: nvidia-vfx\nVersion: 0.2.0.0\n");
  write_file(h.cfg + "/nvvsr/libdir", h.vfx + ":" + h.wheel + "\n");
  write_file(h.cfg + "/vapoursynth/vapoursynth.toml",
             "\"/usr/lib/libvsscript.so\" = [\"/usr/bin/python\",\"/usr/lib/libpython3.so\",\"1\"]\n");

  std::string rec = "\"lib\": \"" + home + "/.local/lib/nvvsr\", \"mpv_wrapper\": \"" + home +
                    "/.local/bin/mpv-sr\", \"private_mpv\": \"" + mpv + "\"";
  if (!repo.empty()) {
    rec += ", \"libnvvsr\": \"" + repo + "/build/core/libnvvsr.so\", \"vapoursynth_plugin\": \"" + repo +
           "/vapoursynth/build/libvsnvvsr.so\", \"vlc_plugin\": \"" + repo + "/vlc/build/libnvvsr_plugin.so\"";
  }
  write_file(share + "/install.json", std::string(R"({
  "schema": 1, "version": "0.1.0", "installed_at": "2026-10-26T10:00:00+00:00",
  "updated_at": "2026-10-26T10:00:00+00:00", "git_commit": null, "git_dirty": false,
  "repo": ")") + (repo.empty() ? "/nonexistent/repo" : repo) + R"(", "prefix": ")" + home + R"(/.local", "home": ")" +
             home + R"(",
  "nvvsr_libdir": ")" + h.vfx + ":" + h.wheel + R"(", "paths": {)" + rec + R"(},
  "private_mpv": {"path": ")" + mpv + R"(", "vapoursynth": true, "built_by_installer": false},
  "components": {"core": {"status": "installed", "detail": ""},
                 "chrome_handoff": {"status": "skipped", "detail": "not requested"}},
  "entries": [], "future_unknown_key": {"x": 1}})");
  return h;
}

}  // namespace support
