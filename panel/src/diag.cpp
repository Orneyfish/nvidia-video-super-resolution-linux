#include "diag.h"

#include <sys/utsname.h>

#include <json-glib/json-glib.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include "paths.h"
#include "proc.h"

namespace {

std::string trim(const std::string &s) {
  const char *ws = " \t\r\n\f\v";
  const size_t a = s.find_first_not_of(ws);
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

std::string or_text(const std::optional<std::string> &v, const char *fallback) {
  return v && !v->empty() ? *v : fallback;
}

std::string fmt(const std::optional<double> &v) {
  if (!v) return "?";
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", *v);
  return buf;
}

// The object member `name` of `node`, or nullptr (also for a null node).
JsonNode *member(JsonNode *node, const char *name) {
  if (!node || !JSON_NODE_HOLDS_OBJECT(node)) return nullptr;
  JsonObject *o = json_node_get_object(node);
  return json_object_has_member(o, name) ? json_object_get_member(o, name) : nullptr;
}

// The member `name` when it is a non-empty string.
std::optional<std::string> string_member(JsonNode *node, const char *name) {
  JsonNode *v = member(node, name);
  if (!v || !JSON_NODE_HOLDS_VALUE(v) || json_node_get_value_type(v) != G_TYPE_STRING) return std::nullopt;
  const std::string s = json_node_get_string(v);
  if (s.empty()) return std::nullopt;
  return s;
}

std::optional<std::string> tool_version(const std::vector<std::string> &argv, const Runner &runner) {
  const CmdResult r = runner(argv, 10.0, {});
  return r.ok() ? first_line(r.out) : std::nullopt;
}

std::string kernel_release() {
  utsname u{};
  uname(&u);
  return u.release;
}

}  // namespace

std::string wheel_version(const std::string &install_json_path) {
  const std::string path = install_json_path.empty() ? install_json() : install_json_path;
  std::ifstream in(path, std::ios::binary);
  if (!in) return "unknown";
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string raw = buf.str();

  std::unique_ptr<JsonParser, void (*)(gpointer)> parser(json_parser_new(), g_object_unref);
  GError *err = nullptr;
  if (!json_parser_load_from_data(parser.get(), raw.data(), static_cast<gssize>(raw.size()), &err)) {
    g_clear_error(&err);
    return "unknown";
  }
  JsonNode *root = json_parser_get_root(parser.get());
  if (auto v = string_member(root, "wheel_version")) return *v;
  if (auto v = string_member(member(member(root, "components"), "wheel"), "wheel_version")) return *v;
  if (auto v = string_member(member(root, "wheel"), "version")) return *v;  // the shape docs/install-layout.md shows
  return "unknown";
}

std::optional<std::string> first_line(const std::string &text) {
  std::istringstream in(text);
  std::string raw;
  while (std::getline(in, raw)) {
    const std::string line = trim(raw);
    if (line.empty()) continue;
    size_t cut = line.size();
    for (const char *sep : {" Copyright", " ("}) cut = std::min(cut, line.find(sep));
    return line.substr(0, cut);
  }
  return std::nullopt;
}

Versions collect_versions(const SrStatus &status, const std::string &app_version,
                          const std::vector<std::pair<std::string, std::string>> &toolkit, const std::string &root,
                          const Runner &runner) {
  // The detail is the library path, with " (development build)" appended for a checkout build.
  const Check &lib = status.check("libnvvsr");
  const std::string lib_path = lib.ok == true ? lib.detail.substr(0, lib.detail.find(" (development build)")) : "";
  const std::string mpv = private_mpv();
  std::error_code ec;
  const bool have_private = std::filesystem::is_regular_file(mpv, ec);

  Versions v = {
      {"NV Control", std::optional<std::string>(app_version)},
      {"libnvvsr", libnvvsr_version(lib_path)},
      {"NVIDIA kernel driver", driver_version_cheap(root)},
      {"NVIDIA VFX wheel", wheel_version()},
      {"Private mpv", have_private ? tool_version({mpv, "--no-config", "--version"}, runner) : std::nullopt},
      {"System mpv", tool_version({"mpv", "--no-config", "--version"}, runner)},
      {"VLC", tool_version({"vlc", "--version"}, runner)},
      {"Kernel", kernel_release()},
  };
  for (const auto &[name, version] : toolkit) v.emplace_back(name, version);
  return v;
}

std::string redact(std::string text, const std::string &home_arg) {
  const std::string h = home_arg.empty() ? home() : home_arg;
  if (h.empty() || h == "/") return text;
  for (size_t p = 0; (p = text.find(h, p)) != std::string::npos; p += 1) text.replace(p, h.size(), "~");
  return text;
}

std::string build_report(const ReportInput &in) {
  std::vector<std::string> lines = {"NV Control diagnostics", std::string(22, '='), "", "Versions"};
  for (const auto &[name, version] : in.versions) lines.push_back("  " + name + ": " + or_text(version, "not found"));

  lines.push_back("");
  lines.push_back("GPU");
  lines.push_back("  hybrid mode: " + or_text(in.hybrid, "unknown (supergfxctl not available)"));
  lines.push_back("  power source: " + in.power_source);
  const RuntimeInfo &rt = in.runtime;
  lines.push_back("  runtime power: status=" + or_text(rt.status, "unknown") + " control=" +
                  or_text(rt.control, "?") + " state=" + or_text(rt.power_state, "?"));
  if (in.sample) {
    const GpuSample &s = *in.sample;
    lines.push_back("  last reading: " + s.name + ", driver " + s.driver + ", " + fmt(s.mem_used_mib) + "/" +
                    fmt(s.mem_total_mib) + " MiB, util " + fmt(s.util_pct) + " %, " + fmt(s.temp_c) + " C, " +
                    fmt(s.power_w) + "/" + fmt(s.power_limit_w) + " W, pstate " + or_text(s.pstate, "?"));
  } else {
    lines.push_back("  last reading: none (the GPU is not queried while it sleeps)");
  }

  lines.push_back("");
  lines.push_back("Super resolution: " + in.status.headline() + " (" + in.status.mode + ")");
  if (in.status.version) lines.push_back("  installed version: " + *in.status.version);
  for (const Check &c : in.status.checks) {
    const std::string mark = !c.ok ? "unknown" : *c.ok ? "ok" : "MISSING";
    lines.push_back("  [" + mark + "] " + c.label + (c.detail.empty() ? "" : ": " + c.detail));
  }

  auto settings = [&lines](const std::vector<std::string> &src) {
    if (src.empty())
      lines.push_back("  (no overrides, defaults apply)");
    else
      lines.insert(lines.end(), src.begin(), src.end());
  };
  lines.push_back("");
  lines.push_back("mpv settings (vsr.conf overrides)");
  settings(in.mpv_settings);
  lines.push_back("");
  lines.push_back("VLC settings (nvvsr-* in vlcrc)");
  settings(in.vlc_settings);
  lines.insert(lines.end(), in.extra_lines.begin(), in.extra_lines.end());

  // Only whether the file exists: its contents can hold cookies and must never reach the report.
  std::error_code ec;
  const bool handoff = std::filesystem::is_regular_file(config_home() + "/nvvsr/handoff.json", ec);
  lines.push_back("");
  lines.push_back(std::string("Legacy Chrome handoff configured: ") + (handoff ? "yes" : "no") +
                  " (contents not included)");

  std::string text;
  for (const std::string &l : lines) text += l + "\n";
  return redact(text);
}
