// Fake /sys trees for tests (port of make_sysfs in support.py). Header-only.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <unistd.h>  // mkdtemp

// A fresh empty directory under the temp dir, removed (with contents) on destruction.
class TempDir {
 public:
  TempDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "nvc_sysfs_XXXXXX").string();
    if (mkdtemp(&tmpl[0])) path_ = tmpl;
  }
  ~TempDir() {
    std::error_code ec;
    if (!path_.empty()) std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir &) = delete;
  TempDir &operator=(const TempDir &) = delete;
  const std::string &path() const { return path_; }

 private:
  std::string path_;
};

inline void write_file(const std::filesystem::path &p, const std::string &text) {
  std::filesystem::create_directories(p.parent_path());
  std::ofstream(p) << text;
}

// A /sys look-alike under root: one NVIDIA display GPU (+ its HDMI audio function), one Intel GPU, an AC adapter
// and a battery. with_nvidia=false leaves out the NVIDIA devices and the driver version.
inline std::string make_sysfs(const std::string &root, const std::string &runtime_status = "active",
                              const std::string &online = "1", bool with_nvidia = true) {
  namespace fs = std::filesystem;
  const fs::path r(root);
  const fs::path dev = r / "bus" / "pci" / "devices";
  if (with_nvidia) {
    const fs::path g = dev / "0000:01:00.0";
    write_file(g / "vendor", "0x10de\n");
    write_file(g / "class", "0x030000\n");
    write_file(g / "device", "0x24a0\n");
    write_file(g / "boot_vga", "0\n");
    write_file(g / "power" / "runtime_status", runtime_status + "\n");
    write_file(g / "power" / "control", "auto\n");
    write_file(g / "power" / "autosuspend_delay_ms", "5000\n");
    write_file(g / "power_state", (runtime_status == "suspended" ? "D3cold" : "D0") + std::string("\n"));
    write_file(g / "d3cold_allowed", "1\n");
    fs::create_directories(r / "bus" / "pci" / "drivers" / "nvidia");
    fs::create_directory_symlink("../../../../bus/pci/drivers/nvidia", g / "driver");
    const fs::path a = dev / "0000:01:00.1";
    write_file(a / "vendor", "0x10de\n");
    write_file(a / "class", "0x040300\n");
    write_file(r / "module" / "nvidia" / "version", "615.78.08\n");
  }
  const fs::path i = dev / "0000:00:02.0";
  write_file(i / "vendor", "0x8086\n");
  write_file(i / "class", "0x030000\n");
  write_file(r / "class" / "power_supply" / "AC0" / "type", "Mains\n");
  write_file(r / "class" / "power_supply" / "AC0" / "online", online + "\n");
  write_file(r / "class" / "power_supply" / "BAT0" / "type", "Battery\n");
  return root;
}
