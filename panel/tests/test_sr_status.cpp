// C++ port of the compute_status, mpv-has-vapoursynth, probe and version tests in panel/tests/test_sr.py
// (the install.json, libdir and vsscript tests are in test_sr_install.cpp). No GPU: the probe is a fake script.
#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.h"
#include "proc.h"
#include "sr_install.h"
#include "sr_status.h"
#include "support_fakebin.h"

namespace fs = std::filesystem;

namespace {

bool contains(const std::string &s, const std::string &needle) { return s.find(needle) != std::string::npos; }

// A scratch HOME (XDG dirs unset) and repo. HOME, PATH and the probe variables are restored on exit.
struct Scratch {
  std::string root, home, repo, old_path;
  Scratch() {
    std::string tmpl = (fs::temp_directory_path() / "sr_status.XXXXXX").string();
    if (!mkdtemp(tmpl.data())) throw std::runtime_error("mkdtemp failed");
    root = tmpl;
    home = root + "/home";
    repo = root + "/repo";
    fs::create_directories(home);
    const char *p = getenv("PATH");
    old_path = p ? p : "";
    setenv("HOME", home.c_str(), 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_STATE_HOME");
  }
  ~Scratch() {
    setenv("PATH", old_path.c_str(), 1);
    unsetenv("FAKE_PROBE_RC");
    unsetenv("NVVSR_LIBDIR");
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  void prepend_path(const std::string &dir) const { setenv("PATH", (dir + ":" + old_path).c_str(), 1); }
};

// support::make_home plus what test_sr.py's make_home adds: the private mpv and the wrapper (both executable)
// and, with with_repo, the dev build outputs (install.json then records them too).
support::Home install_fixture(const Scratch &s, bool with_repo) {
  const support::Home h = support::make_home(s.home, with_repo ? s.repo : "");
  support::write_exec(s.home + "/.local/opt/mpv-vsr/bin/mpv", "#!/bin/sh\necho 'mpv 0.41.0-vsr (fake)'\n");
  support::write_exec(s.home + "/.local/bin/mpv-sr", "#!/bin/sh\nexec true\n");
  if (with_repo)
    for (const char *rel : {"build/core/libnvvsr.so", "vapoursynth/build/libvsnvvsr.so", "vlc/build/libnvvsr_plugin.so"})
      support::write_file(s.repo + "/" + rel, "");
  return h;
}

const char *const kFakeProbe = R"(#!/bin/sh
# Stand-in for nvvsr_probe: one output line, exit 0 when usable.
if [ -n "$FAKE_PROBE_RC" ]; then echo "no NVIDIA GPU or CUDA device: fake: no CUDA device"; exit 1; fi
if [ -z "$NVVSR_LIBDIR" ]; then echo "no library directory: NVVSR_LIBDIR is not set"; exit 1; fi
echo ok
)";

std::string fake_lib(const Scratch &s) {
  const auto lib = support::build_fake_lib(s.root + "/lib");
  CHECK(lib.has_value(), "fake libnvvsr.so builds with the C compiler");
  return lib.value_or("");
}

// test_installed_everything_ok
void test_installed_everything_ok() {
  const Scratch s;
  install_fixture(s, true);
  const SrStatus st = compute_status(read_install_json(), std::nullopt, true);
  CHECK_EQ(st.mode, std::string("installed"));
  CHECK(st.mpv_ready() && st.vlc_ready(), "mpv and vlc both ready");
  CHECK_EQ(st.headline(), std::string("Ready for mpv and VLC"));
}

// test_dev_mode_uses_repo_build_dirs_without_install_json
void test_dev_mode() {
  const Scratch s;
  install_fixture(s, true);
  fs::remove(s.home + "/.local/share/nvvsr/install.json");
  const SrStatus st = compute_status(read_install_json(), s.repo, std::nullopt);
  CHECK_EQ(st.mode, std::string("dev"));
  CHECK(st.ok("libnvvsr") && st.ok("vsplugin") && st.ok("vlc"), "repo build outputs found");
  CHECK(contains(st.check("libnvvsr").detail, "development build"), "repo build is tagged as development build");
  CHECK(contains(st.check("mpv").detail, "filter list not checked"), "unknown filter list is said so");
}

// test_nothing_installed
void test_nothing_installed() {
  const Scratch s;
  const SrStatus st = compute_status(read_install_json(), std::nullopt, std::nullopt);
  CHECK_EQ(st.mode, std::string("missing"));
  CHECK_EQ(st.headline(), std::string("Not installed"));
  CHECK_EQ(st.checks.size(), static_cast<size_t>(7));
  for (const auto &c : st.checks) {
    CHECK(c.ok != true, "%s is not ok", c.id.c_str());
    CHECK(!c.hint.empty(), "%s says how to fix it", c.id.c_str());
  }
}

// test_each_failure_is_reported_on_its_own
void test_each_failure_is_reported_on_its_own() {
  const Scratch s;
  const support::Home h = install_fixture(s, true);
  fs::remove(s.home + "/.local/opt/mpv-vsr/bin/mpv");
  support::write_file(h.cfg + "/vapoursynth/vapoursynth.toml", "\"/x/other.so\" = [\"a\"]\n");
  fs::remove(h.vfx + "/libnvVFXVideoSuperRes.so");
  const SrStatus st = compute_status(read_install_json(), std::nullopt, std::nullopt);
  std::vector<std::string> failing;
  for (const auto &c : st.checks)
    if (c.ok != true) failing.push_back(c.id);
  CHECK(failing == std::vector<std::string>({"libdir", "mpv", "vsscript"}), "only libdir, mpv and vsscript fail");
  CHECK(!st.mpv_ready(), "mpv is not ready");
  CHECK(!st.check("mpv").hint.empty(), "mpv failure has a hint");
  CHECK_EQ(st.headline(), std::string("Incomplete installation"));
}

// test_mpv_without_vapoursynth_filter
void test_mpv_without_vapoursynth_filter() {
  const Scratch s;
  install_fixture(s, true);
  const SrStatus st = compute_status(read_install_json(), std::nullopt, false);
  CHECK(!st.ok("mpv"), "mpv without the filter fails");
  CHECK(contains(st.check("mpv").hint, "rebuild"), "hint says to rebuild mpv");
}

// test_mpv_filter_detection
void test_mpv_filter_detection() {
  const Scratch s;
  const std::string with = s.root + "/mpv-with";
  const std::string without = s.root + "/mpv-without";
  support::write_exec(with, R"(#!/bin/sh
printf 'Available video filters:\n  format  x\n  vapoursynth  VS\n'
)");
  support::write_exec(without, R"(#!/bin/sh
printf 'Available video filters:\n  format  x\n'
)");
  CHECK(mpv_has_vapoursynth(with) == true, "vapoursynth listed -> true");
  CHECK(mpv_has_vapoursynth(without) == false, "vapoursynth not listed -> false");
  CHECK(!mpv_has_vapoursynth(s.root + "/missing-mpv").has_value(), "mpv that cannot run -> unknown");
}

// test_probe_ok_passes_the_libdir_through_the_environment
void test_probe_ok() {
  const Scratch s;
  const std::string lib = fake_lib(s);
  support::write_exec(s.root + "/bin/nvvsr_probe", kFakeProbe);
  s.prepend_path(s.root + "/bin");
  const ProbeResult r = run_probe(lib, "/some/libdir");
  CHECK(r.ok, "probe succeeds with NVVSR_LIBDIR set");
  CHECK_EQ(r.summary, std::string("Super resolution works"));
  CHECK_EQ(r.detail, std::string("ok"));
}

// test_probe_is_found_in_the_installed_layout (<prefix>/lib/libnvvsr.so, <prefix>/bin/nvvsr_probe, nothing on PATH)
void test_probe_installed_layout() {
  const Scratch s;
  const std::string lib = fake_lib(s);
  support::write_exec(s.root + "/bin/nvvsr_probe", kFakeProbe);
  const ProbeResult r = run_probe(lib, "/some/libdir");
  CHECK(r.ok, "the probe in ../bin of the library directory is found without PATH");
  CHECK_EQ(r.detail, std::string("ok"));
}

// test_probe_failure_shows_status_and_last_error
void test_probe_failure() {
  const Scratch s;
  const std::string lib = fake_lib(s);
  support::write_exec(s.root + "/bin/nvvsr_probe", kFakeProbe);
  s.prepend_path(s.root + "/bin");
  setenv("FAKE_PROBE_RC", "1", 1);
  const ProbeResult r = run_probe(lib, "/some/libdir");
  CHECK(!r.ok, "probe reports failure");
  CHECK_EQ(r.summary, std::string("Not usable"));
  CHECK_EQ(r.detail, std::string("no NVIDIA GPU or CUDA device: fake: no CUDA device"));
}

// test_probe_without_libdir_reports_the_library_message (plus: an inherited NVVSR_LIBDIR is removed)
void test_probe_without_libdir() {
  const Scratch s;
  const std::string lib = fake_lib(s);
  support::write_exec(s.root + "/bin/nvvsr_probe", kFakeProbe);
  s.prepend_path(s.root + "/bin");
  unsetenv("NVVSR_LIBDIR");
  const ProbeResult r = run_probe(lib, std::nullopt);
  CHECK(!r.ok, "probe without a libdir fails");
  CHECK(contains(r.detail, "NVVSR_LIBDIR"), "the probe's message names NVVSR_LIBDIR");
  setenv("NVVSR_LIBDIR", "/leaked", 1);
  const ProbeResult leaked = run_probe(lib, std::nullopt);
  CHECK(!leaked.ok, "an inherited NVVSR_LIBDIR is not passed when no libdir is given");
}

// test_probe_of_a_broken_or_missing_library
void test_probe_broken_or_missing() {
  const Scratch s;
  const std::string bad = s.root + "/bad/bad.so";
  support::write_file(bad, "not an ELF");
  support::write_exec(s.root + "/bad/nvvsr_probe", "#!/bin/sh\nkill -SEGV $$\n");  // a probe that crashes
  const ProbeResult crashed = run_probe(bad, "/x");
  CHECK(!crashed.ok, "a crashing probe is not usable");
  CHECK_EQ(crashed.summary, std::string("The check crashed (signal 11)"));
  CHECK_EQ(run_probe("", std::nullopt).summary, std::string("libnvvsr was not found"));
  s.prepend_path(s.root + "/empty");  // no probe anywhere on PATH
  const ProbeResult none = run_probe(s.root + "/lib/libnvvsr.so", "/x");
  CHECK(!none.ok, "a missing probe is not usable");
  CHECK_EQ(none.summary, std::string("The check could not start"));
}

// test_version_without_gpu
void test_version() {
  const Scratch s;
  const std::string lib = fake_lib(s);
  CHECK_EQ(libnvvsr_version(lib).value_or("none"), std::string("9.9.9-fake (API 1)"));
  CHECK(!libnvvsr_version("/nonexistent.so").has_value(), "missing library -> no version");
  support::write_file(s.root + "/bad.so", "not an ELF");
  CHECK(!libnvvsr_version(s.root + "/bad.so").has_value(), "non-ELF file -> no version");
}

// make_fake_bin port: the fake nvidia-smi answers and logs
void test_fake_bin() {
  const Scratch s;
  const std::string bin = support::make_fake_bin(s.root + "/fakebin", s.root + "/smi.log");
  const CmdResult r = run({bin + "/nvidia-smi", "--query-gpu=name", "--format=csv"}, 10.0);
  CHECK(r.ok() && contains(r.out, "RTX 3070 Ti"), "fake nvidia-smi answers a GPU query");
  CHECK(fs::exists(s.root + "/smi.log"), "fake nvidia-smi logs its call");
}

}  // namespace

int main() {
  test_installed_everything_ok();
  test_dev_mode();
  test_nothing_installed();
  test_each_failure_is_reported_on_its_own();
  test_mpv_without_vapoursynth_filter();
  test_mpv_filter_detection();
  test_probe_ok();
  test_probe_installed_layout();
  test_probe_failure();
  test_probe_without_libdir();
  test_probe_broken_or_missing();
  test_version();
  test_fake_bin();
  return finish("test_sr_status");
}
