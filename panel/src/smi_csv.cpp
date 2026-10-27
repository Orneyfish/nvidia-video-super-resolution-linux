#include "smi_csv.h"

#include <charconv>
#include <cmath>
#include <climits>
#include <map>
#include <cctype>

namespace {

const char *kWhitespace = " \t\r\n\f\v";

std::string trim(const std::string &s) {
  const size_t a = s.find_first_not_of(kWhitespace);
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(kWhitespace) - a + 1);
}

std::string upper(std::string s) {
  for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string lower(std::string s) {
  for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// nvidia-smi's "no value" spellings.
bool na(const std::string &text) {
  const std::string t = trim(text);
  const std::string u = upper(t);
  return t.empty() || t[0] == '[' || u == "N/A" || u == "NOT SUPPORTED" || u == "UNKNOWN";
}

std::optional<double> num(const std::string &text) {
  if (na(text)) return std::nullopt;
  std::string t = trim(text);
  if (!t.empty() && t[0] == '+') t.erase(0, 1);  // from_chars rejects a leading '+' that float() accepts
  double v = 0;
  const char *end = t.data() + t.size();
  auto [ptr, ec] = std::from_chars(t.data(), end, v);
  if (ec != std::errc() || ptr != end) return std::nullopt;
  return v;
}

// Truncates like int(float); nullopt when the value is not finite or does not fit an int.
std::optional<int> integer(const std::string &text) {
  const auto v = num(text);
  if (!v || !std::isfinite(*v) || *v <= INT_MIN || *v >= INT_MAX) return std::nullopt;
  return static_cast<int>(*v);
}

// Cells of the first record of `text` as csv.reader(skipinitialspace=True) reads them.
std::vector<std::string> first_csv_row(const std::string &text) {
  std::vector<std::string> cells;
  const size_t n = text.size();
  size_t i = 0;
  while (true) {
    while (i < n && text[i] == ' ') ++i;
    std::string cell;
    if (i < n && text[i] == '"') {
      for (++i; i < n; ++i) {
        if (text[i] != '"') {
          cell += text[i];
        } else if (i + 1 < n && text[i + 1] == '"') {
          cell += '"';
          ++i;
        } else {
          ++i;
          break;
        }
      }
    }
    while (i < n && text[i] != ',' && text[i] != '\n') cell += text[i++];
    cells.push_back(cell);
    if (i >= n || text[i] == '\n') break;
    ++i;  // the comma
  }
  return cells;
}

}  // namespace

std::vector<std::string> query_gpu_argv() {
  std::string fields;
  for (const auto &f : kQueryFields) fields += (fields.empty() ? "" : ",") + f;
  return {"nvidia-smi", "--query-gpu=" + fields, "--format=csv,noheader,nounits"};
}

std::optional<GpuSample> parse_gpu_csv(const std::string &text, const std::vector<std::string> &fields) {
  const std::string t = trim(text);
  if (t.empty()) return std::nullopt;
  const auto cells = first_csv_row(t);
  if (cells.size() < 2) return std::nullopt;
  std::map<std::string, std::string> v;  // missing columns read as "", like dict.get(key, "")
  for (size_t i = 0; i < fields.size() && i < cells.size(); ++i) v[fields[i]] = trim(cells[i]);
  auto get = [&](const char *key) { return v.count(key) ? v[key] : std::string(); };

  GpuSample s;
  s.name = get("name");
  s.driver = get("driver_version");
  s.mem_used_mib = num(get("memory.used"));
  s.mem_total_mib = num(get("memory.total"));
  s.util_pct = num(get("utilization.gpu"));
  s.temp_c = num(get("temperature.gpu"));
  s.power_w = num(get("power.draw"));
  s.power_limit_w = num(get("power.limit"));
  s.clock_gr_mhz = num(get("clocks.gr"));
  s.clock_mem_mhz = num(get("clocks.mem"));
  s.pcie_gen = integer(get("pcie.link.gen.current"));
  s.pcie_width = integer(get("pcie.link.width.current"));
  s.fan_pct = num(get("fan.speed"));
  if (!na(get("pstate"))) s.pstate = get("pstate");
  const std::string persistence = lower(get("persistence_mode"));
  if (persistence == "enabled") s.persistence = true;
  else if (persistence == "disabled") s.persistence = false;
  return s;
}

Query<GpuSample> query_gpu(const Runner &runner) {
  const CmdResult r = runner(query_gpu_argv(), 8.0, {});
  if (!r.ok()) return {std::nullopt, r.error_text()};
  if (auto s = parse_gpu_csv(r.out)) return {std::move(s), ""};
  return {std::nullopt, "nvidia-smi returned no GPU"};
}

std::vector<ComputeApp> parse_compute_apps(const std::string &text) {
  std::vector<ComputeApp> apps;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = trim(text.substr(start, end - start));
    start = end + 1;
    if (line.empty() || lower(line).rfind("no running", 0) == 0) continue;
    const size_t last = line.rfind(',');
    if (last == std::string::npos) continue;  // no "pid, name, mem" row
    const std::string head = line.substr(0, last);
    const size_t comma = head.find(',');
    const std::string pid = trim(comma == std::string::npos ? head : head.substr(0, comma));
    const std::string name = comma == std::string::npos ? "" : trim(head.substr(comma + 1));
    if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
    int value = 0;
    if (std::from_chars(pid.data(), pid.data() + pid.size(), value).ec != std::errc()) continue;  // pid overflows int
    apps.push_back({value, name.empty() ? "?" : name, num(line.substr(last + 1))});
  }
  return apps;
}

Query<std::vector<ComputeApp>> query_compute_apps(const Runner &runner) {
  const CmdResult r = runner({"nvidia-smi", "--query-compute-apps=pid,process_name,used_memory",
                              "--format=csv,noheader,nounits"},
                             8.0, {});
  if (!r.ok()) return {std::nullopt, r.error_text()};
  return {parse_compute_apps(r.out), ""};
}

std::optional<std::string> supergfxctl_mode(const Runner &runner) {
  const CmdResult r = runner({"supergfxctl", "-g"}, 4.0, {});
  if (!r.ok()) return std::nullopt;
  const std::string text = trim(r.out);
  const std::string mode = trim(text.substr(0, text.find_first_of("\r\n")));
  if (mode.empty()) return std::nullopt;
  return mode;
}
