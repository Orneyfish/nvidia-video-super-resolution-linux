// nvidia-smi CSV queries (--query-gpu, --query-compute-apps) and the supergfxctl mode. Port of nvc/gpu.py.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "proc.h"

struct GpuSample {
  std::string name;
  std::string driver;
  std::optional<double> mem_used_mib, mem_total_mib, util_pct, temp_c, power_w, power_limit_w;
  std::optional<double> clock_gr_mhz, clock_mem_mhz;
  std::optional<int> pcie_gen, pcie_width;
  std::optional<double> fan_pct;
  std::optional<std::string> pstate;
  std::optional<bool> persistence;
};

struct ComputeApp {
  int pid = 0;
  std::string name;
  std::optional<double> mem_mib;
};

// (value, error): on success value is set; on failure error says why.
template <class T>
struct Query {
  std::optional<T> value;
  std::string error;
};

using Runner = std::function<CmdResult(const std::vector<std::string> &argv, double timeout_s,
                                       const std::vector<std::string> &env)>;

// Column order of `--query-gpu=...`.
inline const std::vector<std::string> kQueryFields = {
    "name",        "driver_version",     "memory.used",       "memory.total",     "utilization.gpu",
    "temperature.gpu", "power.draw",     "power.limit",       "clocks.gr",        "clocks.mem",
    "pcie.link.gen.current", "pcie.link.width.current", "fan.speed", "pstate", "persistence_mode",
};

std::vector<std::string> query_gpu_argv();
// First GPU row of `--query-gpu=<fields> --format=csv,noheader,nounits`, or nullopt.
std::optional<GpuSample> parse_gpu_csv(const std::string &text, const std::vector<std::string> &fields = kQueryFields);
// Wakes a suspended GPU: callers check runtime_status first.
Query<GpuSample> query_gpu(const Runner &runner = run);

std::vector<ComputeApp> parse_compute_apps(const std::string &text);
Query<std::vector<ComputeApp>> query_compute_apps(const Runner &runner = run);

// Hybrid mode name ('Hybrid', 'Integrated', ...) or nullopt. Talks to the supergfxd daemon, not to the GPU.
std::optional<std::string> supergfxctl_mode(const Runner &runner = run);
