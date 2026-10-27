// Versions and the plain-text "Copy diagnostics" report (port of panel/nvc/diag.py). No secrets: no cookies, no
// handoff.json contents, and the home directory is shown as ~. Besides std types, the header uses the panel's data
// headers only (no GTK, no json-glib).
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gpu_sysfs.h"
#include "smi_csv.h"
#include "sr_status.h"

// Name -> version in display order. nullopt (or "") prints as "not found".
using Versions = std::vector<std::pair<std::string, std::optional<std::string>>>;

// The NVIDIA VFX wheel version recorded in install.json ("wheel_version" at the top level, components.wheel, or
// the "wheel" object's "version"), or "unknown". An empty path means the default install.json.
std::string wheel_version(const std::string &install_json_path = "");

// The first non-blank line of a --version output, cut before " Copyright" or " (".
std::optional<std::string> first_line(const std::string &text);

// Every version the report shows. Runs the tools through `runner`, so call it from a worker thread.
// `toolkit` holds extra name/version pairs (GTK, libadwaita), appended after the defaults.
Versions collect_versions(const SrStatus &status, const std::string &app_version,
                          const std::vector<std::pair<std::string, std::string>> &toolkit = {},
                          const std::string &root = "", const Runner &runner = run);

// Replaces every occurrence of `home` (empty: paths::home()) with ~.
std::string redact(std::string text, const std::string &home = "");

struct ReportInput {
  Versions versions;
  SrStatus status;
  RuntimeInfo runtime;
  std::optional<GpuSample> sample;    // last nvidia-smi reading; nullopt: the GPU was never queried
  std::optional<std::string> hybrid;  // supergfxctl mode, nullopt: not available
  std::string power_source;           // ac | battery | unknown
  // Settings lines already formatted by the caller ("  key = value", or a "(file does not exist ...)" note).
  // Empty prints "(no overrides, defaults apply)".
  std::vector<std::string> mpv_settings;
  std::vector<std::string> vlc_settings;
  std::vector<std::string> extra_lines;
};

std::string build_report(const ReportInput &in);
