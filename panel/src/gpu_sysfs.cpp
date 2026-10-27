#include "gpu_sysfs.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

#include "paths.h"

namespace {

const char *const kNvidiaVendor = "0x10de";

// Stripped file content, or nullopt when it cannot be read. Only used on attributes that never touch the hardware.
std::optional<std::string> read_text(const std::string &path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string s;
  char buf[256];
  while (true) {
    const ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0) {
      close(fd);
      return std::nullopt;  // e.g. EISDIR for a directory
    }
    if (n == 0) break;
    s.append(buf, static_cast<size_t>(n));
  }
  close(fd);
  const char *ws = " \t\r\n\f\v";
  const size_t a = s.find_first_not_of(ws);
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string join(const std::string &a, const std::string &b) { return a + "/" + b; }

// Sorted names in dir; empty when the directory cannot be listed.
std::vector<std::string> list_sorted(const std::string &dir) {
  namespace fs = std::filesystem;
  std::vector<std::string> names;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    names.push_back(it->path().filename().string());
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace

std::vector<PciGpu> find_nvidia_gpus(const std::string &root) {
  const std::string base = join(join(join(root.empty() ? sysfs_root() : root, "bus"), "pci"), "devices");
  std::vector<PciGpu> found;
  for (const std::string &name : list_sorted(base)) {
    const std::string d = join(base, name);
    const std::string vendor = lower(read_text(join(d, "vendor")).value_or(""));
    const std::string klass = lower(read_text(join(d, "class")).value_or(""));
    if (vendor != kNvidiaVendor || klass.rfind("0x03", 0) != 0) continue;
    found.push_back({name, d});
  }
  return found;
}

RuntimeInfo read_runtime(const PciGpu *gpu) {
  RuntimeInfo rt;
  if (!gpu) return rt;
  const std::string pm = join(gpu->path, "power");
  auto status = read_text(join(pm, "runtime_status"));
  if (status && !status->empty()) rt.status = status;
  rt.control = read_text(join(pm, "control"));
  rt.power_state = read_text(join(gpu->path, "power_state"));
  return rt;
}

std::string gpu_state(const std::optional<std::string> &runtime_status) {
  if (!runtime_status) return "unknown";
  const std::string &s = *runtime_status;
  if (s == "suspended" || s == "suspending") return "asleep";
  if (s == "active" || s == "resuming" || s == "unsupported") return "awake";
  return "unknown";
}

std::string poll_decision(const PollInput &in) {
  if (!in.window_visible || !in.page_active) return "hidden";
  if (in.state == "asleep") return "asleep";
  if (in.in_flight || !in.interval_s || *in.interval_s <= 0) return "wait";
  if (in.since_last_s && *in.since_last_s < *in.interval_s * 0.9) return "wait";
  return "query";
}

std::optional<std::string> driver_version_cheap(const std::string &root) {
  return read_text(join(join(join(root.empty() ? sysfs_root() : root, "module"), "nvidia"), "version"));
}

std::string power_source(const std::string &root) {
  const std::string base = join(join(root.empty() ? sysfs_root() : root, "class"), "power_supply");
  bool have_battery = false, online = false;
  for (const std::string &name : list_sorted(base)) {
    const std::string d = join(base, name);
    const std::string t = lower(read_text(join(d, "type")).value_or(""));
    if (t == "battery") {
      have_battery = true;
    } else if (t == "mains" || t == "usb" || t == "usb_c" || t == "usb_pd" || t == "usb_dcp" || t == "usb_cdp" ||
               t == "usb_aca" || t == "ups") {
      if (read_text(join(d, "online")) == "1") online = true;
    }
  }
  if (online) return "ac";
  return have_battery ? "battery" : "unknown";
}
