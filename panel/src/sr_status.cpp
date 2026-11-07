#include "sr_status.h"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <sstream>
#include <stdexcept>

#include "paths.h"
#include "proc.h"

namespace fs = std::filesystem;

namespace {

const char *const kSentinel = "libnvVFXVideoSuperRes.so";

bool is_file(const std::string &p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

bool is_dir(const std::string &p) {
  std::error_code ec;
  return fs::is_directory(p, ec);
}

bool is_exe(const std::string &p) { return is_file(p) && access(p.c_str(), X_OK) == 0; }

// The first existing candidate (executable when `exe`): the recorded path, then the dev-checkout files. "" if none.
std::string first_found(const std::string &recorded, const std::vector<std::string> &dev,
                        const std::optional<std::string> &repo, bool exe) {
  std::vector<std::string> cands;
  if (!recorded.empty()) cands.push_back(recorded);
  if (repo)
    for (const auto &d : dev) cands.push_back(*repo + "/" + d);
  for (const auto &c : cands)
    if (exe ? is_exe(c) : is_file(c)) return c;
  return "";
}

std::string tag(const std::string &p, const std::optional<std::string> &repo) {
  if (p.empty()) return "";
  if (repo && p.rfind(*repo + "/", 0) == 0) return p + " (development build)";
  return p;
}

Check mk(const std::string &id, const std::string &label, std::optional<bool> ok, const std::string &detail,
         const std::string &hint) {
  const bool good = ok == true;
  return Check{id, label, ok, good ? detail : "", good ? "" : hint};
}

Check libdir_check() {
  const std::string label = "NVIDIA VFX library path (~/.config/nvvsr/libdir)";
  const auto dirs = read_libdir();
  if (!dirs)
    return mk("libdir", label, false, "",
              "Write the nvvfx/libs and nvidia_vfx.libs directories of the VFX wheel, colon separated.");
  if (std::any_of(dirs->begin(), dirs->end(), [](const std::string &d) { return !is_dir(d); }))
    return mk("libdir", label, false, "", "A directory listed in the file does not exist; fix it or reinstall the wheel.");
  const bool has_sentinel = std::any_of(dirs->begin(), dirs->end(), [](const std::string &d) {
    return is_file(d + "/" + kSentinel);
  });
  if (!has_sentinel)
    return mk("libdir", label, false, "",
              std::string(kSentinel) + " is not in the listed directories (use the nvvfx/libs directory of <prefix>/vfx or third_party/nvidia-vfx).");
  std::string joined;
  for (const auto &d : *dirs) joined += (joined.empty() ? "" : ":") + d;
  return mk("libdir", label, true, joined, "");
}

Check mpv_check(const std::string &mpv, std::optional<bool> mpv_vs) {
  const std::string label = "Private mpv with the VapourSynth filter";
  if (mpv.empty())
    return mk("mpv", label, false, "",
              "The distro mpv has no --vf=vapoursynth; build the private mpv (install/build-mpv-vsr.sh).");
  if (mpv_vs && !*mpv_vs)
    return mk("mpv", label, false, "", mpv + " has no vapoursynth filter; rebuild it with VapourSynth.");
  return mk("mpv", label, true, mpv + (mpv_vs ? "" : " (filter list not checked)"), "");
}

std::string trim(const std::string &s) {
  const char *ws = " \t\r\n\f\v";
  const size_t a = s.find_first_not_of(ws);
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

// The last non-blank line, trimmed.
std::string last_line(const std::string &s) {
  std::istringstream in(s);
  std::string line, last;
  while (std::getline(in, line)) {
    const std::string t = trim(line);
    if (!t.empty()) last = t;
  }
  return last;
}

// The last 300 characters of the trimmed text (what the Python panel showed).
std::string tail(const std::string &s) {
  const std::string t = trim(s);
  return t.size() > 300 ? t.substr(t.size() - 300) : t;
}

// The probe's environment: the inherited one, with NVVSR_LIBDIR set or removed.
std::vector<std::string> probe_env(const std::optional<std::string> &libdir) {
  const bool set = libdir && !libdir->empty();
  std::map<std::string, std::string> extra;
  if (set) extra["NVVSR_LIBDIR"] = *libdir;
  auto env = clean_env(extra);
  if (!set)
    env.erase(std::remove_if(env.begin(), env.end(),
                             [](const std::string &kv) { return kv.rfind("NVVSR_LIBDIR=", 0) == 0; }),
              env.end());
  return env;
}

// nvvsr_probe next to the library (build tree), in <prefix>/bin beside <prefix>/lib (install), then ~/.local/bin, then PATH.
std::optional<std::string> probe_tool(const std::string &lib_path) {
  const fs::path dir = fs::path(lib_path).parent_path();
  if (!dir.empty())
    for (const fs::path &d : {dir, dir.parent_path() / "bin"})
      if (is_exe((d / "nvvsr_probe").string())) return (d / "nvvsr_probe").string();
  const std::string local = home() + "/.local/bin/nvvsr_probe";
  if (is_exe(local)) return local;
  if (on_path("nvvsr_probe")) return std::string("nvvsr_probe");
  return std::nullopt;
}

}  // namespace

const Check &SrStatus::check(const std::string &id) const {
  for (const auto &c : checks)
    if (c.id == id) return c;
  throw std::logic_error("no SR check named " + id);
}

bool SrStatus::ok(const std::string &id) const { return check(id).ok == true; }

bool SrStatus::mpv_ready() const {
  return ok("libnvvsr") && ok("libdir") && ok("vsplugin") && ok("mpv") && ok("vsscript");
}

bool SrStatus::vlc_ready() const { return ok("libnvvsr") && ok("libdir") && ok("vlc"); }

std::string SrStatus::headline() const {
  if (mpv_ready() && vlc_ready()) return "Ready for mpv and VLC";
  if (mpv_ready()) return "Ready for mpv";
  if (vlc_ready()) return "Ready for VLC";
  if (mode == "missing") return "Not installed";
  return "Incomplete installation";
}

SrStatus compute_status(const InstallInfo &info, const std::optional<std::string> &repo,
                        std::optional<bool> mpv_vs) {
  auto recorded = [&info](const char *key) {
    const auto it = info.paths.find(key);
    return it == info.paths.end() ? std::string() : it->second;
  };
  const std::string libnvvsr = first_found(recorded("libnvvsr"), {"build/core/libnvvsr.so"}, repo, false);
  const std::string vsplugin =
      first_found(recorded("vapoursynth_plugin"), {"vapoursynth/build/libvsnvvsr.so"}, repo, false);
  const std::string vlc = first_found(recorded("vlc_plugin"), {"vlc/build/libnvvsr_plugin.so"}, repo, false);
  std::string mpv = first_found(recorded("private_mpv"), {}, std::nullopt, true);
  if (mpv.empty()) mpv = first_found(private_mpv(), {}, std::nullopt, true);
  const std::string wrapper = first_found(recorded("mpv_wrapper"), {}, std::nullopt, true);
  const std::string wrapper_found =
      wrapper.empty() ? first_found(home() + "/.local/bin/mpv-sr", {}, std::nullopt, true) : wrapper;

  const bool installed = info.present && !info.error;
  const bool any_found = !libnvvsr.empty() || !vsplugin.empty() || !vlc.empty();
  const VsscriptEntry vs = vsscript_entry();

  SrStatus st;
  st.mode = installed ? "installed" : any_found ? "dev" : "missing";
  st.version = info.version;
  st.repo = repo;
  st.checks = {
      mk("libnvvsr", "libnvvsr (core library)", !libnvvsr.empty(), tag(libnvvsr, repo),
         "Build it (cmake -S core -B build/core) or run install/install.sh."),
      libdir_check(),
      mk("vsplugin", "VapourSynth plugin (nvvsr.Upscale)", !vsplugin.empty(), tag(vsplugin, repo),
         "Build it (cmake -S vapoursynth -B vapoursynth/build) or run install/install.sh."),
      mpv_check(mpv, mpv_vs),
      mk("mpv_sr", "mpv-sr wrapper", !wrapper_found.empty(), wrapper_found,
         "Created by install/install.sh in ~/.local/bin."),
      mk("vsscript", "VapourSynth libvsscript entry (vapoursynth.toml)", vs.present, vs.detail,
         "Add a libvsscript.so entry to ~/.config/vapoursynth/vapoursynth.toml (tools/phase0-results.md)."),
      mk("vlc", "VLC module (libnvvsr_plugin.so)", !vlc.empty(), tag(vlc, repo),
         "Build it (cmake -S vlc -B vlc/build) or run install/install.sh."),
  };
  return st;
}

std::optional<bool> mpv_has_vapoursynth(const std::string &mpv_bin) {
  const CmdResult r = run({mpv_bin, "--no-config", "--vf=help"}, 8.0);
  if (!r.rc) return std::nullopt;
  std::istringstream lines(r.out + "\n" + r.err);
  std::string line;
  while (std::getline(lines, line)) {
    const size_t i = line.find_first_not_of(" \t");
    const std::string word = "vapoursynth";
    if (i == std::string::npos || line.compare(i, word.size(), word) != 0) continue;
    const size_t end = i + word.size();
    if (end == line.size() || !(std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_'))
      return true;
  }
  return false;
}

ProbeResult run_probe(const std::string &lib_path, const std::optional<std::string> &libdir, double timeout_s) {
  if (lib_path.empty()) return {false, "libnvvsr was not found", "Build or install it first."};
  const auto tool = probe_tool(lib_path);
  if (!tool)
    return {false, "The check could not start",
            "nvvsr_probe was not found next to libnvvsr, in ~/.local/bin or on PATH; build or install it."};
  const CmdResult r = run({*tool}, timeout_s, probe_env(libdir));
  if (r.timed_out)
    return {false, "The check timed out",
            "The GPU or driver did not answer within " + std::to_string(static_cast<int>(timeout_s)) + " s."};
  if (!r.rc) return {false, "The check could not start", r.error_text()};
  if (*r.rc < 0) return {false, "The check crashed (signal " + std::to_string(-*r.rc) + ")", tail(r.err)};
  const std::string line = last_line(r.out);
  if (*r.rc == 0) return {true, "Super resolution works", line};
  if (line.empty()) {
    const std::string text = tail(r.err.empty() ? r.out : r.err);
    return {false, "The check failed", text.empty() ? "no output" : text};
  }
  return {false, "Not usable", line};
}

std::optional<std::string> libnvvsr_version(const std::string &lib_path) {
  if (lib_path.empty()) return std::nullopt;
  void *lib = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!lib) return std::nullopt;
  // shortcut: dlsym returns void*, cast to the function type (POSIX-sanctioned for dlsym results in practice).
  auto version = reinterpret_cast<const char *(*)()>(dlsym(lib, "nvvsr_version"));
  auto api = reinterpret_cast<int (*)()>(dlsym(lib, "nvvsr_api_version"));
  std::optional<std::string> out;
  if (version && api && version())
    out = std::string(version()) + " (API " + std::to_string(api()) + ")";
  dlclose(lib);
  return out;
}
