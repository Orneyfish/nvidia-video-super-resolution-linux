// Port of the nvidia-smi -q tests in panel/tests/test_gpu.py: test_power_page, test_power_page_older_driver_names,
// test_power_limit_command_validates, test_persistence_command, RealDriverOutput. The Python tests pass None and
// "abc" to power_limit_command; C++ takes a double, so only the numeric bad values are here (99.7 covers "99.7").
// Checks after the port cover the tree, _unique, the unit parsing and query_power_page (no Python test).
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.h"
#include "smi_q.h"

namespace {

// support.py POWER_Q, the canned `nvidia-smi -q` text.
const char *kPowerQ = R"smi(
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
)smi";

std::string read_file(const std::string &path, bool &ok) {
  std::ifstream f(path);
  ok = static_cast<bool>(f);
  std::ostringstream os;
  os << f.rdbuf();
  return os.str();
}

bool throws_invalid(double watts, const PowerInfo *info, std::string *message = nullptr) {
  try {
    power_limit_command(watts, info);
    return false;
  } catch (const std::invalid_argument &e) {
    if (message) *message = e.what();
    return true;
  }
}

}  // namespace

int main() {
  // test_power_page
  {
    const DetailInfo d = parse_power_page(kPowerQ);
    const PowerInfo &p = d.power;
    CHECK(p.draw_w == 18.52, "draw 18.52 W");
    CHECK(p.limit_w == 125.0, "limit 125 W");
    CHECK(p.default_w == 125.0, "default 125 W");
    CHECK(p.min_w == 1.0, "min 1 W");
    CHECK(p.max_w == 150.0, "max 150 W");
    const std::map<std::string, int> want_clocks = {{"graphics", 1230}, {"sm", 1230}, {"memory", 6000}, {"video", 1110}};
    CHECK(d.clocks == want_clocks, "clocks graphics/sm/memory/video");
    CHECK(d.max_clocks.count("graphics") && d.max_clocks.at("graphics") == 2100, "max graphics clock 2100");
    CHECK(d.limiters == std::vector<std::string>({"SW Power Cap"}), "limiters [SW Power Cap]");
    CHECK(d.has_limiter_info, "limiter section found");
  }

  // test_power_page_older_driver_names
  {
    const std::string old =
        "GPU 00000000:01:00.0\n    Power Readings\n        Power Management : Supported\n"
        "        Power Draw : 10.00 W\n        Power Limit : 80.00 W\n        Default Power Limit : 80.00 W\n";
    const PowerInfo p = parse_power_page(old).power;
    CHECK(p.draw_w == 10.0 && p.limit_w == 80.0, "older names: draw 10 W, limit 80 W");
    CHECK(p.management == std::optional<std::string>("Supported"), "older names: management Supported");
    CHECK(!p.max_w.has_value(), "older names: no max limit");
  }

  // test_power_limit_command_validates
  {
    const PowerInfo info = parse_power_page(kPowerQ).power;
    CHECK(power_limit_command(120, &info) == std::vector<std::string>({"sudo", "nvidia-smi", "-pl", "120"}),
          "120 W with card limits");
    CHECK(power_limit_command(99.0, nullptr) == std::vector<std::string>({"sudo", "nvidia-smi", "-pl", "99"}),
          "99.0 W without card limits");
    for (double bad : {0.0, -5.0, 151.0, 99.7, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()})
      CHECK(throws_invalid(bad, &info), "bad limit %g rejected", bad);
    std::string message;
    CHECK(throws_invalid(151.0, &info, &message) && message == "above the card maximum of 150 W",
          "151 W message names the maximum");
    CHECK(throws_invalid(0.5, nullptr, &message) && message == "power limit must be a whole number of watts",
          "0.5 W message says whole number");
    CHECK(throws_invalid(0.0, nullptr, &message) && message == "power limit must be positive", "0 W message");
    PowerInfo low = info;
    low.min_w = 20.0;
    CHECK(throws_invalid(19.0, &low, &message) && message == "below the card minimum of 20 W",
          "below the minimum message");
  }

  // test_persistence_command
  {
    CHECK(persistence_command(true) == std::vector<std::string>({"sudo", "nvidia-smi", "-pm", "1"}), "persistence on");
    CHECK(persistence_command(false) == std::vector<std::string>({"sudo", "nvidia-smi", "-pm", "0"}),
          "persistence off");
  }

  // RealDriverOutput.test_power_limits_nested_under_ceiling_are_found
  {
    const std::string path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "smi_q_power_driver615.txt";
    bool ok = false;
    const std::string text = read_file(path, ok);
    CHECK(ok, "fixture readable: %s", path.c_str());
    const DetailInfo info = parse_power_page(text);
    const PowerInfo &p = info.power;
    CHECK(p.limit_w == 150.0, "driver 615: limit 150 W from the ceiling section");
    CHECK(p.default_w == 115.0, "driver 615: default 115 W from the ceiling section");
    CHECK(p.min_w == 1.0, "driver 615: min 1 W");
    CHECK(p.max_w == 150.0, "driver 615: max 150 W");
    CHECK(!p.requested_w.has_value(), "driver 615: requested limit N/A");
    CHECK(p.draw_w == 25.94, "driver 615: instantaneous draw 25.94 W");
    CHECK(info.max_clocks.count("graphics") && info.max_clocks.at("graphics") == 1785,
          "driver 615: max graphics 1785 MHz");
    CHECK(info.limiters == std::vector<std::string>({"Idle"}), "driver 615: limiters [Idle]");
    CHECK(info.clocks.at("graphics") == 210 && info.clocks.at("memory") == 6001, "driver 615: clocks 210 / 6001 MHz");
  }

  // Extra: tree shape, skipped lines, _unique, key-value split.
  {
    const SmiNode tree = parse_smi_q("GPU 0\n  Clocks\n    Graphics : 210 MHz\n  Power : 5 W\nTop : x\n"
                                     "=====\nTimestamp : now\nName:x\n");
    CHECK(tree.children.size() == 3, "GPU 0, Top, Name:x; = and Timestamp lines skipped (got %zu)",
          tree.children.size());
    const SmiNode &gpu = tree.children[0].second;
    CHECK(tree.children[0].first == "GPU 0" && gpu.is_section, "GPU 0 is a section");
    CHECK(gpu.children.size() == 2 && gpu.children[0].first == "Clocks" && gpu.children[0].second.is_section,
          "Clocks nested under GPU 0");
    CHECK(gpu.children[0].second.children[0].second.value == "210 MHz", "Graphics leaf under Clocks");
    CHECK(gpu.children[1].first == "Power" && gpu.children[1].second.value == "5 W", "Power leaf under GPU 0");
    CHECK(tree.children[1].first == "Top" && tree.children[1].second.value == "x", "Top leaf at root");
    CHECK(tree.children[2].first == "Name:x" && tree.children[2].second.is_section, "no space before colon: section");
  }
  {
    const SmiNode tree = parse_smi_q("A : 1\nA : 2\nA\n  x : 3\nA : 4\nFoo : a : b\n");
    std::vector<std::string> keys;
    for (const auto &e : tree.children) keys.push_back(e.first);
    CHECK(keys == std::vector<std::string>({"A", "A#2", "A#3", "A#4", "Foo"}), "duplicate keys become A#2, A#3, ...");
    CHECK(tree.children[3].second.value == "4" && tree.children[2].second.is_section,
          "A#3 is the section, A#4 the later leaf");
    CHECK(tree.children[4].second.value == "a : b", "value keeps later colons (first colon splits)");
  }

  // Extra: unit parsing (N/A and wrong units give no value).
  {
    const PowerInfo p = parse_power_page("GPU 0\n  Power Readings\n    Current Power Limit : 150 Watts\n"
                                         "    Default Power Limit : N/A\n    Min Power Limit : 1.5 W\n")
                            .power;
    CHECK(!p.limit_w.has_value() && !p.default_w.has_value(), "'150 Watts' and 'N/A' are not watts");
    CHECK(p.min_w == 1.5, "1.5 W parsed");
  }

  // Extra: query_power_page with a fake runner (no Python test).
  {
    std::vector<std::string> seen;
    const SmiRunner fake = [&](const std::vector<std::string> &argv, double, const std::vector<std::string> &) {
      seen = argv;
      CmdResult r;
      r.argv = argv;
      r.rc = 0;
      r.out = kPowerQ;
      return r;
    };
    const PowerQuery q = query_power_page(fake);
    CHECK(q.info && q.info->power.limit_w == 125.0 && q.error.empty(), "query parses the runner output");
    CHECK(seen == std::vector<std::string>({"nvidia-smi", "-q", "-d", "POWER,CLOCK,PERFORMANCE"}),
          "query runs one nvidia-smi -q -d POWER,CLOCK,PERFORMANCE");

    const SmiRunner failing = [](const std::vector<std::string> &argv, double, const std::vector<std::string> &) {
      CmdResult r;
      r.argv = argv;
      r.rc = 9;
      r.err = "boom\nGPU is lost";
      return r;
    };
    const PowerQuery bad = query_power_page(failing);
    CHECK(!bad.info && bad.error == "GPU is lost", "failed query reports the last stderr line");

    const SmiRunner missing = [](const std::vector<std::string> &argv, double, const std::vector<std::string> &) {
      CmdResult r;
      r.argv = argv;
      r.missing = true;
      return r;
    };
    const PowerQuery none = query_power_page(missing);
    CHECK(!none.info && none.error == "nvidia-smi not found", "missing nvidia-smi reported");
  }

  return finish("test_smi_q");
}
