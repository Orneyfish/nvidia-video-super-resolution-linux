// `nvidia-smi -q` parsing and the Power page; port of the -q half of panel/nvc/gpu.py.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "proc.h"

// One `nvidia-smi -q` line: a "Key : value" leaf, or a section whose children keep file order.
// Keys are unique within a section: a repeated key becomes "Key#2", "Key#3", ...
struct SmiNode {
  bool is_section = false;
  std::string value;                                      // leaf only
  std::vector<std::pair<std::string, SmiNode>> children;  // section only
};

SmiNode parse_smi_q(const std::string &text);

struct PowerInfo {
  std::optional<double> draw_w, limit_w, requested_w, default_w, min_w, max_w;
  std::optional<std::string> management;
};

struct DetailInfo {
  PowerInfo power;
  std::map<std::string, int> clocks;      // graphics/sm/memory/video -> MHz
  std::map<std::string, int> max_clocks;  // same keys
  std::vector<std::string> limiters;      // names of the "Active" clock event reasons
  bool has_limiter_info = false;
};

// Parses `nvidia-smi -q -d POWER,CLOCK,PERFORMANCE`. Driver 615 nests the power limits under
// "GPU Ceiling Power Limit"; they are read from there as well.
DetailInfo parse_power_page(const std::string &text);

using SmiRunner = std::function<CmdResult(const std::vector<std::string> &, double, const std::vector<std::string> &)>;

struct PowerQuery {
  std::optional<DetailInfo> info;
  std::string error;  // set when info is empty
};

// Wakes the GPU: only call when it is awake or the user asked.
PowerQuery query_power_page(const SmiRunner &runner = run);

// Throws std::invalid_argument (Python's ValueError) for a non-whole, non-positive or out-of-range limit.
std::vector<std::string> power_limit_command(double watts, const PowerInfo *info = nullptr);
std::vector<std::string> persistence_command(bool enable);
