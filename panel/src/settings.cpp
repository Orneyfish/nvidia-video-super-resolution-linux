#include "settings.h"

#include <json-glib/json-glib.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <memory>
#include <system_error>

void Settings::load(const std::string &path) {
  poll_interval = kDefaultPoll;
  std::unique_ptr<JsonParser, void (*)(gpointer)> parser(json_parser_new(), g_object_unref);
  GError *err = nullptr;
  if (!json_parser_load_from_file(parser.get(), path.c_str(), &err)) {
    g_clear_error(&err);
    return;
  }
  JsonNode *root = json_parser_get_root(parser.get());
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return;
  JsonNode *n = json_object_get_member(json_node_get_object(root), "poll_interval");
  if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64) return;
  const gint64 v = json_node_get_int(n);
  if (std::find(std::begin(kPollChoices), std::end(kPollChoices), v) != std::end(kPollChoices))
    poll_interval = static_cast<int>(v);
}

bool Settings::save(const std::string &path) const {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path target = fs::weakly_canonical(path, ec);
  if (ec) target = path;
  fs::create_directories(target.parent_path(), ec);
  if (ec) return false;
  const std::string text = "{\n  \"poll_interval\": " + std::to_string(poll_interval) + "\n}\n";
  GError *err = nullptr;
  const bool ok = g_file_set_contents(target.c_str(), text.data(), static_cast<gssize>(text.size()), &err);
  g_clear_error(&err);
  return ok;
}
