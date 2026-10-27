#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "check.h"
#include "gpu_sysfs.h"
#include "support_sysfs.h"

namespace fs = std::filesystem;

namespace {

PollInput poll_base() {
  PollInput in;
  in.interval_s = 2;
  return in;
}

}  // namespace

int main() {
  // Sysfs (port of test_gpu.py Sysfs)
  {
    TempDir t;
    make_sysfs(t.path());
    auto gpus = find_nvidia_gpus(t.path());
    CHECK(gpus.size() == 1 && gpus[0].addr == "0000:01:00.0",
          "finds only the NVIDIA display controller (not HDMI audio, not Intel)");
  }
  {
    TempDir t;
    make_sysfs(t.path(), "active", "1", false);
    CHECK(find_nvidia_gpus(t.path()).empty(), "no NVIDIA GPU gives no devices");
    CHECK(find_nvidia_gpus((fs::path(t.path()) / "missing").string()).empty(), "missing bus gives no devices");
  }
  {
    // root "" resolves to sysfs_root(), i.e. NVC_SYSFS_ROOT
    TempDir t;
    make_sysfs(t.path());
    setenv("NVC_SYSFS_ROOT", t.path().c_str(), 1);
    CHECK(find_nvidia_gpus().size() == 1, "empty root uses NVC_SYSFS_ROOT");
    CHECK(driver_version_cheap() == std::optional<std::string>("615.78.08"), "driver version via NVC_SYSFS_ROOT");
    unsetenv("NVC_SYSFS_ROOT");
  }
  {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"suspended", "asleep"}, {"suspending", "asleep"}, {"active", "awake"},
        {"resuming", "awake"},   {"unsupported", "awake"}};
    for (const auto &[status, state] : cases) {
      TempDir t;
      make_sysfs(t.path(), status);
      const auto gpus = find_nvidia_gpus(t.path());
      const RuntimeInfo rt = read_runtime(&gpus[0]);
      CHECK(rt.status == std::optional<std::string>(status) && gpu_state(rt.status) == state,
            "runtime status and gpu state");
      CHECK(rt.control == std::optional<std::string>("auto"), "runtime control is auto");
    }
    CHECK(gpu_state(std::nullopt) == "unknown", "no runtime status is unknown");
    CHECK(!read_runtime(nullptr).status.has_value(), "no GPU gives no runtime status");
    TempDir t;
    make_sysfs(t.path(), "");
    CHECK(!read_runtime(&find_nvidia_gpus(t.path())[0]).status.has_value(), "empty runtime_status is unknown");
  }
  {
    TempDir t;
    make_sysfs(t.path(), "active", "1");
    CHECK(power_source(t.path()) == "ac", "online mains is AC");
    CHECK(driver_version_cheap(t.path()) == std::optional<std::string>("615.78.08"), "driver version from sysfs");
  }
  {
    TempDir t;
    make_sysfs(t.path(), "active", "0");
    CHECK(power_source(t.path()) == "battery", "offline mains with a battery is battery");
  }
  {
    TempDir t;
    make_sysfs(t.path());
    CHECK(power_source((fs::path(t.path()) / "nope").string()) == "unknown", "no power_supply dir is unknown");
    CHECK(!driver_version_cheap((fs::path(t.path()) / "nope").string()).has_value(), "no driver is no version");
  }

  // Poll policy (port of test_gpu.py PollPolicy)
  CHECK(poll_decision(poll_base()) == "query", "queries when visible, on overview and awake");
  {
    PollInput in = poll_base();
    in.window_visible = false;
    CHECK(poll_decision(in) == "hidden", "hidden window does not query");
    in = poll_base();
    in.page_active = false;
    CHECK(poll_decision(in) == "hidden", "inactive page does not query");
    in = poll_base();
    in.state = "asleep";
    CHECK(poll_decision(in) == "asleep", "never query a sleeping GPU");
    in.window_visible = false;
    CHECK(poll_decision(in) == "hidden", "hidden beats asleep");
    in = poll_base();
    in.state = "unknown";
    CHECK(poll_decision(in) == "query", "unknown state is queried");
  }
  {
    PollInput in = poll_base();
    in.since_last_s = 0.5;
    CHECK(poll_decision(in) == "wait", "rate limit: 0.5 s since last query waits");
    in.since_last_s = 2.0;
    CHECK(poll_decision(in) == "query", "rate limit: 2.0 s since last query queries");
    in = poll_base();
    in.in_flight = true;
    CHECK(poll_decision(in) == "wait", "request in flight waits");
    in = poll_base();
    in.interval_s = 0;
    CHECK(poll_decision(in) == "wait", "interval 0 is manual mode");
    in.interval_s = std::nullopt;
    CHECK(poll_decision(in) == "wait", "no interval is manual mode");
  }

  return finish("test_gpu_sysfs");
}
