// C++ port of the diag tests in panel/tests/test_sr.py (wheel version, first_line, report content) and the
// versions and report checks of panel/tests/test_smoke.py. Fake tools and canned data only: no GPU.
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "check.h"
#include "diag.h"
#include "sr_install.h"
#include "sr_status.h"
#include "support_fakebin.h"
#include "support_sysfs.h"

namespace fs = std::filesystem;

namespace {

bool contains(const std::string &s, const std::string &needle) { return s.find(needle) != std::string::npos; }

// A scratch HOME and repo under a fresh temp dir (XDG dirs unset), removed on destruction.
struct Scratch {
  std::string root = (fs::temp_directory_path() / "nvvsr-diag-XXXXXX").string();
  std::string home, repo;

  Scratch() {
    if (!mkdtemp(root.data())) throw std::runtime_error("mkdtemp failed");
    home = root + "/home";
    repo = root + "/repo";
    fs::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    for (const char *v : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME"}) unsetenv(v);
  }
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

// Adds "wheel_version" to the install.json that make_home wrote (its "schema": 1 text is unique).
void add_wheel_version(const std::string &path, const std::string &version) {
  std::ifstream in(path);
  std::stringstream buf;
  buf << in.rdbuf();
  std::string text = buf.str();
  const std::string key = "\"schema\": 1,";
  const size_t at = text.find(key);
  if (at == std::string::npos) throw std::runtime_error("unexpected install.json: " + path);
  text.insert(at + key.size(), " \"wheel_version\": \"" + version + "\",");
  support::write_file(path, text);
}

// The layout install/install.sh leaves behind, every component present: the status reads "Ready for mpv and VLC".
// libnvvsr is the fake library, so its version can be read back.
SrStatus installed_status(const Scratch &s) {
  support::make_home(s.home, s.repo);
  if (!support::build_fake_lib(s.repo + "/build/core")) throw std::runtime_error("no C compiler for the fake libnvvsr");
  support::write_file(s.repo + "/vapoursynth/build/libvsnvvsr.so", "");
  support::write_file(s.repo + "/vlc/build/libnvvsr_plugin.so", "");
  support::write_exec(s.home + "/.local/opt/mpv-vsr/bin/mpv", "#!/bin/sh\n");
  support::write_exec(s.home + "/.local/bin/mpv-sr", "#!/bin/sh\n");
  add_wheel_version(s.home + "/.local/share/nvvsr/install.json", "0.2.0.0");
  return compute_status(read_install_json(), std::nullopt, true);
}

// A fake runner: every tool reports its canned --version line; "vlc" is missing when vlc_installed is false.
Runner fake_tools(bool vlc_installed) {
  return [vlc_installed](const std::vector<std::string> &argv, double, const std::vector<std::string> &) {
    CmdResult r;
    r.argv = argv;
    if (argv[0] == "vlc" && !vlc_installed) {
      r.missing = true;
      return r;
    }
    r.rc = 0;
    r.out = argv[0] == "vlc" ? "VLC version 3.0.24 Vetinari (fake)\n" : "mpv 0.41.0 Copyright (c) fake\n";
    return r;
  };
}

void test_first_line() {
  CHECK(first_line("\nmpv 0.41.0 Copyright (c) 2000\nmore") == std::optional<std::string>("mpv 0.41.0"),
        "mpv version cut before Copyright");
  CHECK(first_line("VLC version 3.0.24 Vetinari (revision x)") == std::optional<std::string>("VLC version 3.0.24 Vetinari"),
        "VLC version cut before the revision");
  CHECK(!first_line(""), "empty output gives nothing");
}

void test_wheel_version() {
  Scratch s;
  support::make_home(s.home, s.repo);
  const std::string json = s.home + "/.local/share/nvvsr/install.json";
  CHECK(wheel_version() == "unknown", "install.json without wheel_version reads as unknown");
  add_wheel_version(json, "0.2.0.0");
  CHECK(wheel_version() == "0.2.0.0", "top-level wheel_version is read from install.json");
  CHECK(wheel_version(s.root + "/nonexistent.json") == "unknown", "missing install.json reads as unknown");
  support::write_file(s.root + "/nested.json", R"({"components": {"wheel": {"wheel_version": "1.2.3"}}})");
  CHECK(wheel_version(s.root + "/nested.json") == "1.2.3", "components.wheel.wheel_version is read");
  support::write_file(s.root + "/doc.json", R"({"wheel": {"name": "nvidia-vfx", "version": "4.5.6"}})");
  CHECK(wheel_version(s.root + "/doc.json") == "4.5.6", "wheel.version (install-layout.md shape) is read");
}

void test_versions() {
  Scratch s;
  const SrStatus st = installed_status(s);
  TempDir sys;
  make_sysfs(sys.path());
  const Versions v = collect_versions(st, "0.1.0", {{"GTK", "4.16.3"}, {"libadwaita", "1.6.0"}}, sys.path(), fake_tools(true));
  auto get = [&v](const std::string &name) {
    for (const auto &[k, val] : v)
      if (k == name) return val.value_or("not found");
    return std::string("no such row");
  };
  CHECK(get("NV Control") == "0.1.0", "panel version");
  CHECK(get("libnvvsr") == "9.9.9-fake (API 1)", "libnvvsr version from the library");
  CHECK(get("NVIDIA VFX wheel") == "0.2.0.0", "wheel version from install.json");
  CHECK(get("NVIDIA kernel driver") == "615.78.08", "kernel driver from sysfs");
  CHECK(get("Private mpv") == "mpv 0.41.0", "private mpv version");
  CHECK(get("System mpv") == "mpv 0.41.0", "system mpv version");
  CHECK(get("VLC") == "VLC version 3.0.24 Vetinari", "VLC version");
  CHECK(get("libadwaita") == "1.6.0", "toolkit versions appended");

  const Versions none = collect_versions(st, "0.1.0", {}, sys.path(), fake_tools(false));
  CHECK(none[6].first == "VLC" && !none[6].second, "a missing VLC has no version");
}

void test_report() {
  Scratch s;
  const SrStatus st = installed_status(s);
  // The handoff file holds secrets: the report may say it exists, never what it contains.
  support::write_file(s.home + "/.config/nvvsr/handoff.json", R"({"use_chrome_cookies": true, "token": "do-not-leak"})");

  ReportInput in;
  in.versions = {{"NV Control", "0.1.0"}, {"VLC", std::nullopt}};
  in.status = st;
  in.runtime.status = "active";
  in.runtime.control = "auto";
  in.runtime.power_state = "D0";
  GpuSample g;
  g.name = "NVIDIA GeForce RTX 3070 Ti Laptop GPU";
  g.driver = "615.78.08";
  g.mem_used_mib = 1312;
  g.mem_total_mib = 8192;
  g.util_pct = 12;
  g.temp_c = 47;
  g.power_w = 18.52;
  g.power_limit_w = 125;
  g.pstate = "P0";
  in.sample = g;
  in.hybrid = "Hybrid";
  in.power_source = "ac";
  in.mpv_settings = {"  quality = high", "  libdir = " + s.home + "/.config/nvvsr/libdir"};
  in.vlc_settings = {"  nvvsr-quality = high"};
  const std::string text = build_report(in);

  CHECK(!contains(text, "do-not-leak"), "handoff.json contents are not in the report");
  CHECK(!contains(text, "use_chrome_cookies"), "handoff.json keys are not in the report");
  CHECK(!contains(text, s.home), "the home path is shortened");
  for (const char *needle : {"NV Control: 0.1.0", "VLC: not found", "Ready for", "hybrid mode: Hybrid", "RTX 3070 Ti",
                             "Legacy Chrome handoff configured: yes (contents not included)", "[ok] libnvvsr",
                             "libdir = ~/.config/nvvsr/libdir", "1312/8192 MiB", "Ready for mpv and VLC (installed)"})
    CHECK(contains(text, needle), "report contains: %s", needle);

  in.sample.reset();
  CHECK(contains(build_report(in), "last reading: none (the GPU is not queried while it sleeps)"),
        "a never-queried GPU says so");
  CHECK(redact(s.home + "/b") == "~/b", "redact shortens the home directory");
}

}  // namespace

int main() {
  test_first_line();
  test_wheel_version();
  test_versions();
  test_report();
  return finish("test_diag");
}
