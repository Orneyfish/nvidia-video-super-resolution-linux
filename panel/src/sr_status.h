// SR install status (what is installed, what is missing, one fix hint each) and the "does it work" probe.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "sr_install.h"

struct Check {
  std::string id;
  std::string label;
  std::optional<bool> ok;  // nullopt: could not be determined
  std::string detail;      // filled only when ok
  std::string hint;        // filled only when not ok
};

struct SrStatus {
  std::string mode;  // installed | dev | missing
  std::optional<std::string> version;
  std::vector<Check> checks;
  std::optional<std::string> repo;

  const Check &check(const std::string &id) const;  // throws std::logic_error for an unknown id
  bool ok(const std::string &id) const;
  bool mpv_ready() const;
  bool vlc_ready() const;
  std::string headline() const;
};

// Verifies every component on disk. Paths come from install.json when recorded, else from the dev checkout `repo`.
// mpv_vs: whether the private mpv has the vapoursynth filter (nullopt: not checked).
SrStatus compute_status(const InstallInfo &info, const std::optional<std::string> &repo,
                        std::optional<bool> mpv_vs);

// True/false from `mpv --no-config --vf=help` (no GPU access); nullopt when mpv cannot run.
std::optional<bool> mpv_has_vapoursynth(const std::string &mpv_bin);

struct ProbeResult {
  bool ok;
  std::string summary;
  std::string detail;  // the probe's single output line
};

// Runs nvvsr_probe in a child process. It is found next to lib_path, in ../bin of lib_path's directory (installed layout), in ~/.local/bin, or on PATH.
// libdir is passed as NVVSR_LIBDIR (nullopt: the variable is removed). Touches the GPU: call only on request.
ProbeResult run_probe(const std::string &lib_path, const std::optional<std::string> &libdir, double timeout_s = 90.0);

// "0.1.0 (API 1)" or nullopt. dlopen/dlsym of libnvvsr.so only: no NVIDIA library, no GPU.
std::optional<std::string> libnvvsr_version(const std::string &lib_path);
