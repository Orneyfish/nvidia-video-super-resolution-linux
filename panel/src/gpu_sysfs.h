// Cheap sysfs reads for the GPU: finding the NVIDIA device, runtime PM state, and when nvidia-smi may run.
// Nothing here wakes the GPU. An empty `root` means sysfs_root() (NVC_SYSFS_ROOT or /sys).
#pragma once

#include <optional>
#include <vector>
#include <string>

struct PciGpu {
  std::string addr;
  std::string path;
};

struct RuntimeInfo {
  std::optional<std::string> status;        // active | suspended | suspending | resuming | unsupported | nullopt (unknown)
  std::optional<std::string> control;       // auto | on
  std::optional<std::string> power_state;   // D0 | D3hot | D3cold
};

// NVIDIA display/3D controllers (vendor 0x10de, class 0x03xxxx), sorted by address. Audio functions are skipped.
std::vector<PciGpu> find_nvidia_gpus(const std::string &root = "");
// Runtime PM attributes of the device; a null gpu gives an empty RuntimeInfo.
RuntimeInfo read_runtime(const PciGpu *gpu);
// "asleep" | "awake" | "unknown". Only "asleep" means nvidia-smi must not run.
std::string gpu_state(const std::optional<std::string> &runtime_status);

struct PollInput {
  bool window_visible = true;
  bool page_active = true;
  std::string state = "awake";  // gpu_state() result
  bool in_flight = false;
  std::optional<double> interval_s;  // nullopt or <= 0: manual mode
  std::optional<double> since_last_s;
};
// "hidden" | "asleep" | "wait" | "query". Only "query" may run nvidia-smi.
std::string poll_decision(const PollInput &in);

// Kernel module version from sysfs (may be empty or nullopt).
std::optional<std::string> driver_version_cheap(const std::string &root = "");
// "ac" | "battery" | "unknown"; Mains or online USB supply counts as AC.
std::string power_source(const std::string &root = "");
