// Port of the CSV, compute-app and supergfxctl tests in panel/tests/test_gpu.py (canned text, fake runner).
#include <algorithm>
#include <string>
#include <vector>

#include "check.h"
#include "smi_csv.h"

namespace {

using Argv = std::vector<std::string>;

const char *kGpuCsv =
    "NVIDIA GeForce RTX 3070 Ti Laptop GPU, 615.78.08, 1312, 8192, 12, 47, 18.52, 125.00, 1230, 6000, 4, 16, "
    "[N/A], P0, Disabled\n";
const char *kAppsCsv = "4242, /usr/bin/python3, 512\n3103, /usr/lib/chromium/chrome, 220\n";

// Fake runner: canned stdout, exit status and stderr, optionally recording each argv.
Runner fake_run(std::string out, int rc = 0, std::vector<Argv> *record = nullptr) {
  return [=](const Argv &argv, double, const Argv &) {
    if (record) record->push_back(argv);
    CmdResult r;
    r.argv = argv;
    r.rc = rc;
    r.out = out;
    r.err = rc == 0 ? "" : "boom\nGPU is lost";
    return r;
  };
}

Runner missing_run() {
  return [](const Argv &argv, double, const Argv &) {
    CmdResult r;
    r.argv = argv;
    r.missing = true;
    return r;
  };
}

}  // namespace

int main() {
  // test_csv_full_row
  {
    auto s = parse_gpu_csv(kGpuCsv);
    CHECK(s.has_value(), "full GPU row parses");
    if (s) {
      CHECK_EQ(s->name, std::string("NVIDIA GeForce RTX 3070 Ti Laptop GPU"));
      CHECK_EQ(s->driver, std::string("615.78.08"));
      CHECK(s->mem_used_mib == 1312 && s->mem_total_mib == 8192 && s->util_pct == 12 && s->temp_c == 47,
            "memory, utilisation and temperature");
      CHECK(s->power_w == 18.52 && s->power_limit_w == 125 && s->clock_gr_mhz == 1230 && s->clock_mem_mhz == 6000,
            "power and clocks");
      CHECK(s->pcie_gen == 4 && s->pcie_width == 16, "pcie gen and width");
      CHECK(!s->fan_pct.has_value(), "[N/A] fan is absent");
      CHECK(s->pstate == std::optional<std::string>("P0"), "pstate");
      CHECK(s->persistence == std::optional<bool>(false), "Disabled persistence is false");
    }
  }

  // test_csv_twelve_documented_columns_and_na
  {
    std::vector<std::string> twelve(kQueryFields.begin(), kQueryFields.begin() + 12);
    auto s = parse_gpu_csv(
        "NVIDIA X, 615.78.08, 1, 8192, [N/A], [Not Supported], N/A, 125.00, 210, 405, 1, 16\n", twelve);
    CHECK(s.has_value(), "twelve-column row parses");
    if (s) {
      CHECK(!s->util_pct.has_value(), "[N/A] utilisation is absent");
      CHECK(!s->temp_c.has_value(), "[Not Supported] temperature is absent");
      CHECK(!s->power_w.has_value(), "N/A power draw is absent");
      CHECK(s->pcie_gen == 1, "pcie gen read with twelve columns");
      CHECK(!s->persistence.has_value(), "missing persistence column is absent");
    }
  }

  // test_csv_garbage
  CHECK(!parse_gpu_csv("").has_value(), "empty text is not a GPU");
  CHECK(!parse_gpu_csv("No devices were found\n").has_value(), "one-field text is not a GPU");

  // test_csv_quoted_name_keeps_its_comma (csv.reader quoting)
  {
    auto s = parse_gpu_csv("\"NVIDIA, Laptop\", 615.78.08, 1, 8192, 3, 40, 9, 125, 200, 400, 1, 8, 30, P8, enabled\n");
    CHECK(s && s->name == "NVIDIA, Laptop", "quoted name keeps its comma");
    CHECK(s && s->persistence == std::optional<bool>(true), "enabled persistence is true");
  }

  // test_compute_apps
  {
    auto apps = parse_compute_apps(kAppsCsv);
    CHECK(apps.size() == 2, "two compute apps (got %zu)", apps.size());
    if (apps.size() == 2) {
      CHECK(apps[0].pid == 4242 && apps[0].name == "/usr/bin/python3" && apps[0].mem_mib == 512,
            "first app");
      CHECK(apps[1].pid == 3103 && apps[1].name == "/usr/lib/chromium/chrome" && apps[1].mem_mib == 220,
            "second app");
    }
    CHECK(parse_compute_apps("No running compute processes found\n").empty(), "no running processes is empty");
    auto comma = parse_compute_apps("1, name with, comma, 7\n");
    CHECK(!comma.empty() && comma[0].name == "name with, comma", "name may contain commas");
    // unparsable lines: non-numeric pid, no comma, pid only; the good line still parses
    auto mixed = parse_compute_apps("abc, /bin/x, 5\njust text\n77\n9, , [N/A]\n");
    CHECK(mixed.size() == 1, "only the numeric-pid line survives (got %zu)", mixed.size());
    if (mixed.size() == 1) {
      CHECK(mixed[0].pid == 9 && mixed[0].name == "?" && !mixed[0].mem_mib.has_value(),
            "empty name becomes ? and [N/A] memory is absent");
    }
  }

  // test_query_functions_use_one_call_each_and_report_errors
  {
    std::vector<Argv> calls;
    auto q = query_gpu(fake_run(kGpuCsv, 0, &calls));
    CHECK(q.value.has_value() && q.error.empty(), "query_gpu succeeds");
    CHECK(calls.size() == 1, "query_gpu makes one call (got %zu)", calls.size());
    if (!calls.empty()) {
      CHECK_EQ(calls[0][0], std::string("nvidia-smi"));
      CHECK(std::find(calls[0].begin(), calls[0].end(), "--format=csv,noheader,nounits") != calls[0].end(),
            "query_gpu asks for csv,noheader,nounits");
    }
    q = query_gpu(fake_run("", 9));
    CHECK(!q.value.has_value() && q.error == "GPU is lost", "failing query_gpu reports stderr last line");
    q = query_gpu(missing_run());
    CHECK(!q.value.has_value() && q.error == "nvidia-smi not found", "missing nvidia-smi is reported");
    q = query_gpu(fake_run("No devices were found\n"));
    CHECK(!q.value.has_value() && q.error == "nvidia-smi returned no GPU", "unparsable output is reported");

    std::vector<Argv> app_calls;
    auto apps = query_compute_apps(fake_run(kAppsCsv, 0, &app_calls));
    CHECK(apps.value && apps.value->size() == 2 && apps.error.empty(), "query_compute_apps succeeds");
    CHECK(app_calls.size() == 1 && app_calls[0][0] == "nvidia-smi", "query_compute_apps makes one nvidia-smi call");
    auto failed = query_compute_apps(fake_run("", 1));
    CHECK(!failed.value.has_value() && failed.error == "GPU is lost", "failing query_compute_apps reports the error");
    auto none = query_compute_apps(fake_run("No running compute processes found\n"));
    CHECK(none.value && none.value->empty() && none.error.empty(), "no processes is an empty success");
  }

  // test_supergfxctl
  CHECK(supergfxctl_mode(fake_run("Hybrid\n")) == std::optional<std::string>("Hybrid"), "supergfxctl mode");
  CHECK(supergfxctl_mode(fake_run("  Integrated  \nextra line\n")) == std::optional<std::string>("Integrated"),
        "only the first line counts");
  CHECK(!supergfxctl_mode(fake_run("", 1)).has_value(), "failing supergfxctl is nullopt");
  CHECK(!supergfxctl_mode(fake_run("  \n")).has_value(), "blank supergfxctl output is nullopt");
  CHECK(!supergfxctl_mode(missing_run()).has_value(), "missing supergfxctl is nullopt");

  return finish("test_smi_csv");
}
