#include "sr_install.h"

#include <json-glib/json-glib.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <sstream>

#include "paths.h"

namespace {

enum class Read { ok, missing, failed };

// The whole file. `missing` only for ENOENT, as Python's FileNotFoundError; a directory is `failed`.
Read slurp(const std::string &path, std::string &out) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) return errno == ENOENT ? Read::missing : Read::failed;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  const bool bad = std::ferror(f);
  std::fclose(f);
  return bad ? Read::failed : Read::ok;
}

std::string trim(const std::string &s) {
  const char *ws = " \t\r\n\f\v";
  const size_t a = s.find_first_not_of(ws);
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

bool is_type(JsonNode *n, JsonNodeType t) { return n && json_node_get_node_type(n) == t; }
bool is_string(JsonNode *n) {
  return is_type(n, JSON_NODE_VALUE) && json_node_get_value_type(n) == G_TYPE_STRING;
}

// The member, or nullptr when absent.
JsonNode *member(JsonObject *o, const char *name) {
  return json_object_has_member(o, name) ? json_object_get_member(o, name) : nullptr;
}

// A member as text: a string as is, "" when absent or null, any other JSON value as its JSON text.
// shortcut: non-string values print as JSON (3, true), not Python's str(), which only shows in odd files.
std::string text_of(JsonObject *o, const char *name) {
  JsonNode *n = member(o, name);
  if (!n || json_node_is_null(n)) return "";
  if (is_string(n)) return json_node_get_string(n);
  char *s = json_to_string(n, FALSE);
  std::string out = s;
  g_free(s);
  return out;
}

// Calls f(name, node) for each member of o; the list is freed afterwards.
template <class F>
void each_member(JsonObject *o, F f) {
  GList *names = json_object_get_members(o);
  for (GList *l = names; l; l = l->next) {
    const char *name = static_cast<const char *>(l->data);
    f(name, json_object_get_member(o, name));
  }
  g_list_free(names);
}

// "a:b::c" -> expanded non-empty parts (empty parts dropped, as in the Python split/filter).
std::vector<std::string> split_dirs(const std::string &line) {
  std::vector<std::string> dirs;
  std::istringstream parts(line);
  std::string p;
  while (std::getline(parts, p, ':'))
    if (!p.empty()) dirs.push_back(expand(p));
  return dirs;
}

// The key of a `key = value` line, quoted ("..." or '...') or bare. False for any other line.
bool parse_key(const std::string &s, std::string &key) {
  std::string rest;
  if (!s.empty() && (s[0] == '"' || s[0] == '\'')) {
    const size_t close = s.find(s[0], 1);
    if (close == std::string::npos) return false;
    key = s.substr(1, close - 1);
    rest = s.substr(close + 1);
  } else {
    size_t n = 0;
    while (n < s.size() && (std::isalnum(static_cast<unsigned char>(s[n])) || s[n] == '_' || s[n] == '-')) ++n;
    if (n == 0) return false;
    key = s.substr(0, n);
    rest = s.substr(n);
  }
  rest = trim(rest);
  return !rest.empty() && rest[0] == '=';
}

bool ends_with(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

InstallInfo read_install_json(const std::string &path_arg) {
  InstallInfo info;
  info.path = path_arg.empty() ? install_json() : path_arg;
  std::string raw;
  switch (slurp(info.path, raw)) {
    case Read::missing:
      return info;
    case Read::failed:
      info.present = true;
      info.error = "unreadable (read error)";
      return info;
    case Read::ok:
      break;
  }
  info.present = true;

  std::unique_ptr<JsonParser, void (*)(gpointer)> parser(json_parser_new(), g_object_unref);
  GError *err = nullptr;
  if (!json_parser_load_from_data(parser.get(), raw.data(), static_cast<gssize>(raw.size()), &err)) {
    g_clear_error(&err);
    info.error = "unreadable (invalid JSON)";
    return info;
  }
  JsonNode *root = json_parser_get_root(parser.get());  // NULL for empty input, which json-glib accepts
  if (!root) {
    info.error = "unreadable (invalid JSON)";
    return info;
  }
  if (!is_type(root, JSON_NODE_OBJECT)) {
    info.error = "not a JSON object";
    return info;
  }
  JsonObject *obj = json_node_get_object(root);

  JsonNode *version = member(obj, "version");
  if (is_string(version)) info.version = json_node_get_string(version);

  JsonNode *paths_node = member(obj, "paths");
  if (is_type(paths_node, JSON_NODE_OBJECT)) {
    each_member(json_node_get_object(paths_node), [&](const char *name, JsonNode *v) {
      if (is_string(v)) info.paths[name] = json_node_get_string(v);
    });
  }

  JsonNode *comps_node = member(obj, "components");
  if (is_type(comps_node, JSON_NODE_OBJECT)) {
    each_member(json_node_get_object(comps_node), [&](const char *name, JsonNode *c) {
      if (!is_type(c, JSON_NODE_OBJECT)) return;
      JsonObject *co = json_node_get_object(c);
      info.components[name] = {text_of(co, "status"), text_of(co, "detail")};
    });
  }
  return info;
}

std::optional<std::vector<std::string>> read_libdir(const std::string &path_arg) {
  const std::string path = path_arg.empty() ? nvvsr_libdir_file() : path_arg;
  std::string raw;
  if (slurp(path, raw) != Read::ok) return std::nullopt;
  std::istringstream lines(raw);
  std::string line;
  while (std::getline(lines, line)) {
    const std::string s = trim(line);
    if (!s.empty() && s[0] != '#') return split_dirs(s);
  }
  return std::nullopt;
}

std::optional<std::string> libdir_env(const std::string &path) {
  const auto dirs = read_libdir(path);
  if (!dirs || dirs->empty()) return std::nullopt;
  std::string out;
  for (const auto &d : *dirs) {
    if (!out.empty()) out += ":";
    out += d;
  }
  return out;
}

// shortcut: a line scan for top-level keys instead of a TOML parser (no TOML library in the panel); it stops at the
// first [table] header, so only top-level keys match. Upgrade if the file ever gets multi-line values.
VsscriptEntry vsscript_entry(const std::string &toml_arg) {
  const std::string path = toml_arg.empty() ? vapoursynth_toml() : toml_arg;
  std::string raw;
  if (slurp(path, raw) != Read::ok) return {false, "no vapoursynth.toml"};
  std::istringstream lines(raw);
  std::string line;
  while (std::getline(lines, line)) {
    const std::string s = trim(line);
    if (!s.empty() && s[0] == '[') break;
    std::string key;
    if (parse_key(s, key) && ends_with(key, "libvsscript.so")) return {true, key};
  }
  return {false, "no libvsscript.so entry"};
}
