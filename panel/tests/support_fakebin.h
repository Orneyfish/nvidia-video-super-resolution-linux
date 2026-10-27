// Header-only port of make_fake_bin and build_fake_lib from panel/tests/support.py. Fake tools only: no GPU, no NVIDIA.
#pragma once

#include <sys/stat.h>

#include <filesystem>
#include <optional>
#include <string>

#include "proc.h"
#include "support_home.h"

namespace support {

inline const char *const GPU_CSV =
    "NVIDIA GeForce RTX 3070 Ti Laptop GPU, 615.78.08, 1312, 8192, 12, 47, 18.52, 125.00, 1230, 6000, 4, 16, "
    "[N/A], P0, Disabled\n";
inline const char *const APPS_CSV = "4242, /usr/bin/python3, 512\n3103, /usr/lib/chromium/chrome, 220\n";
inline const char *const POWER_Q = R"(
==============NVSMI LOG==============

Timestamp                                 : Sun Oct 26 12:00:00 2026
Driver Version                            : 615.78.08
CUDA Version                              : 13.2

Attached GPUs                             : 1
GPU 00000000:01:00.0
    Performance State                     : P0
    Clocks Event Reasons
        Idle                              : Not Active
        Applications Clocks Setting       : Not Active
        SW Power Cap                      : Active
        HW Slowdown                       : Not Active
            HW Thermal Slowdown           : Not Active
            HW Power Brake Slowdown       : Not Active
        Sync Boost                        : Not Active
        SW Thermal Slowdown               : Not Active
        Display Clock Setting             : Not Active
    GPU Power Readings
        Average Power Draw                : 18.40 W
        Instantaneous Power Draw          : 18.52 W
        Current Power Limit               : 125.00 W
        Requested Power Limit             : 125.00 W
        Default Power Limit               : 125.00 W
        Min Power Limit                   : 1.00 W
        Max Power Limit                   : 150.00 W
    Power Samples
        Duration                          : 2.38 sec
        Number of Samples                 : 119
        Max                               : 20.00 W
    GPU Memory Power Readings
        Average Power Draw                : N/A
    Clocks
        Graphics                          : 1230 MHz
        SM                                : 1230 MHz
        Memory                            : 6000 MHz
        Video                             : 1110 MHz
    Max Clocks
        Graphics                          : 2100 MHz
        SM                                : 2100 MHz
        Memory                            : 6001 MHz
        Video                             : 1950 MHz
)";

// Writes an executable (owner rwx) text file.
inline void write_exec(const std::string &path, const std::string &text) {
  write_file(path, text);
  chmod(path.c_str(), S_IRWXU);
}

// Fake nvidia-smi / supergfxctl / pgrep / mpv / vlc / kitty / alacritty in bindir. nvidia-smi logs every call to `log`.
inline std::string make_fake_bin(const std::string &bindir, const std::string &log, std::string term_log = "") {
  const std::string data = bindir + "/data";
  write_file(data + "/gpu.csv", GPU_CSV);
  write_file(data + "/apps.csv", APPS_CSV);
  write_file(data + "/power.txt", POWER_Q);
  write_exec(bindir + "/nvidia-smi",
             "#!/bin/sh\necho \"$@\" >> \"" + log + "\"\ncase \"$*\" in\n"
             "  *--query-gpu*) cat \"" + data + "/gpu.csv\" ;;\n"
             "  *--query-compute-apps*) cat \"" + data + "/apps.csv\" ;;\n"
             "  *\"-q -d\"*) cat \"" + data + "/power.txt\" ;;\n"
             "  *) echo \"fake nvidia-smi: unsupported: $*\" >&2; exit 2 ;;\n"
             "esac\n");
  write_exec(bindir + "/supergfxctl", "#!/bin/sh\necho Hybrid\n");
  write_exec(bindir + "/pgrep", "#!/bin/sh\nexit 1\n");
  write_exec(bindir + "/mpv", "#!/bin/sh\necho 'mpv 0.41.0 Copyright (c) fake'\n");
  write_exec(bindir + "/vlc", "#!/bin/sh\necho 'VLC version 3.0.24 Vetinari (fake)'\n");
  if (term_log.empty()) term_log = bindir + "/terminal.log";
  for (const char *t : {"kitty", "alacritty"})
    write_exec(bindir + "/" + t, std::string("#!/bin/sh\necho \"") + t + " $@\" >> \"" + term_log + "\"\n");
  return bindir;
}

// Compiles panel/tests/fake_nvvsr.c into dest_dir/libnvvsr.so. nullopt when there is no compiler or it fails.
inline std::optional<std::string> build_fake_lib(const std::string &dest_dir) {
  namespace fs = std::filesystem;
  const char *cc = nullptr;
  for (const char *c : {"gcc", "cc"})
    if (!cc && on_path(c)) cc = c;
  if (!cc) return std::nullopt;
  fs::create_directories(dest_dir);
  const std::string out = dest_dir + "/libnvvsr.so";
  const std::string src = (fs::path(__FILE__).parent_path() / "fake_nvvsr.c").string();
  const CmdResult r = run({cc, "-shared", "-fPIC", "-o", out, src}, 120.0);
  if (!r.ok()) return std::nullopt;
  return out;
}

}  // namespace support
