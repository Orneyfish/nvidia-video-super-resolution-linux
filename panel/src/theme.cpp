#include "theme.h"

#include <json-glib/json-glib.h>

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <memory>

#include "paths.h"

namespace {

// Port of FALLBACK_MATERIAL_COLORS in panel/nvc/theme.py.
const Palette kFallback = {
    {"surface", "#1a1112"}, {"surfaceDim", "#1a1112"}, {"surfaceBright", "#413735"},
    {"surfaceContainerLowest", "#140c0d"}, {"surfaceContainerLow", "#231819"}, {"surfaceContainer", "#271c1d"},
    {"surfaceContainerHigh", "#322627"}, {"surfaceContainerHighest", "#3d3132"},
    {"onSurface", "#f8e0e0"}, {"onSurfaceVariant", "#bba6a6"},
    {"primary", "#f7b6b9"}, {"onPrimary", "#603337"}, {"primaryContainer", "#7a4a4d"}, {"onPrimaryContainer", "#ffdadb"},
    {"secondary", "#e6bdbe"}, {"onSecondary", "#55393a"}, {"secondaryContainer", "#6d4f50"},
    {"onSecondaryContainer", "#ffdadb"},
    {"tertiary", "#e6bdbe"}, {"onTertiary", "#55393a"}, {"tertiaryContainer", "#6d4f50"}, {"onTertiaryContainer", "#ffdadb"},
    {"outline", "#847172"}, {"outlineVariant", "#524344"},
    {"error", "#f97758"}, {"onError", "#450900"}, {"errorContainer", "#852c0a"}, {"onErrorContainer", "#ffdad2"},
    {"success", "#8fd6a0"}, {"onSuccess", "#00391a"},
};

// "#rrggbb" or "rrggbb", nothing else.
bool is_hex_colour(const std::string &s) {
  const std::size_t start = !s.empty() && s[0] == '#' ? 1 : 0;
  if (s.size() - start != 6) return false;
  for (std::size_t i = start; i < s.size(); ++i)
    if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
  return true;
}

bool is_string(JsonNode *n) {
  return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING;
}

}  // namespace

const Palette &fallback_palette() { return kFallback; }

Scheme load_scheme(const std::string &path) {
  std::unique_ptr<JsonParser, void (*)(gpointer)> parser(json_parser_new(), g_object_unref);
  GError *err = nullptr;
  if (!json_parser_load_from_file(parser.get(), path.c_str(), &err)) {
    g_clear_error(&err);
    return {kFallback, "dark"};
  }
  JsonNode *root = json_parser_get_root(parser.get());
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return {kFallback, "dark"};
  JsonObject *data = json_node_get_object(root);

  // caelestia nests the palette under "colours"; a flat file is accepted as well
  JsonObject *src = data;
  if (json_object_has_member(data, "colours") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(data, "colours")))
    src = json_object_get_object_member(data, "colours");

  Palette file;
  GList *names = json_object_get_members(src);
  for (GList *l = names; l; l = l->next) {
    const char *name = static_cast<const char *>(l->data);
    JsonNode *v = json_object_get_member(src, name);
    if (is_string(v) && is_hex_colour(json_node_get_string(v))) file[name] = json_node_get_string(v);
  }
  g_list_free(names);
  if (!file.count("surface") || !file.count("primary")) return {kFallback, "dark"};

  Scheme out{kFallback, "dark"};
  for (const auto &[name, hex] : file) out.colours[name] = hex[0] == '#' ? hex : "#" + hex;
  JsonNode *mode = json_object_has_member(data, "mode") ? json_object_get_member(data, "mode") : nullptr;
  if (is_string(mode) && std::string(json_node_get_string(mode)) == "light") out.mode = "light";
  return out;
}

std::string build_material_css(const Palette &c) {
  // col(name) as in the Python f-string: a missing or malformed value uses the fallback, always "#rrggbb".
  auto col = [&](const std::string &name) {
    auto it = c.find(name);
    if (it != c.end() && is_hex_colour(it->second)) return it->second[0] == '#' ? it->second : "#" + it->second;
    auto fb = kFallback.find(name);
    return fb != kFallback.end() ? fb->second : std::string("#888888");
  };
  auto token = [&](const std::string &t) {
    const std::size_t at = t.find('@');
    if (at == std::string::npos) return col(t);
    // "name@alpha" -> rgba(r, g, b, alpha), the same numbers as Python's hex_to_rgba
    const std::string hex = col(t.substr(0, at));
    const auto ch = [&](std::size_t i) { return std::to_string(std::stoi(hex.substr(1 + 2 * i, 2), nullptr, 16)); };
    return "rgba(" + ch(0) + ", " + ch(1) + ", " + ch(2) + ", " + t.substr(at + 1) + ")";
  };

  static const std::string tpl = R"CSS(
    @define-color accent_color $primary$;
    @define-color accent_bg_color $primary$;
    @define-color accent_fg_color $onPrimary$;
    @define-color destructive_color $error$;
    @define-color destructive_bg_color $error$;
    @define-color destructive_fg_color $onError$;
    @define-color success_color $success$;
    @define-color error_color $error$;
    @define-color window_bg_color $surface$;
    @define-color window_fg_color $onSurface$;
    @define-color view_bg_color $surface$;
    @define-color view_fg_color $onSurface$;
    @define-color headerbar_bg_color $surface$;
    @define-color headerbar_fg_color $onSurface$;
    @define-color headerbar_border_color $outlineVariant$;
    @define-color card_bg_color $surfaceContainer$;
    @define-color card_fg_color $onSurface$;
    @define-color popover_bg_color $surfaceContainerHigh$;
    @define-color popover_fg_color $onSurface$;
    @define-color dialog_bg_color $surfaceContainerHigh$;
    @define-color dialog_fg_color $onSurface$;

    window, window.background { background-color: $surface$; color: $onSurface$; }
    headerbar {
        background-color: $surface$; color: $onSurface$;
        box-shadow: none; border-bottom: 1px solid $outlineVariant$;
    }
    toolbarview > .bottom-bar, toolbarview > .bottom-bar viewswitcherbar { background-color: $surfaceContainer$; }

    viewswitcher button:checked, viewswitcherbar button:checked, viewswitcherbutton:checked {
        background-color: $secondaryContainer$; color: $onSecondaryContainer$;
    }

    button { border-radius: 20px; }
    button.suggested-action { background-color: $primary$; color: $onPrimary$; }
    button.suggested-action:hover { background-color: $primary@0.9$; }
    button.suggested-action:disabled { background-color: $onSurface@0.12$; color: $onSurface@0.38$; }
    button.destructive-action { background-color: $error$; color: $onError$; }

    list.boxed-list, .boxed-list { background-color: $surfaceContainer$; border-radius: 16px;
        border: 1px solid $outlineVariant$; }
    list.boxed-list > row { background-color: transparent; }
    row label.subtitle { color: $onSurfaceVariant$; }
    preferencesgroup > box > box > label.heading { color: $onSurfaceVariant$; }

    .card, box.card { background-color: $surfaceContainer$; color: $onSurface$;
        border-radius: 16px; border: 1px solid $outlineVariant$; box-shadow: none; }
    .nvc-hero { background-color: $surfaceContainerHigh$; border-radius: 20px; padding: 16px; }
    .nvc-tile { background-color: $surfaceContainer$; border: 1px solid $outlineVariant$;
        border-radius: 16px; padding: 14px 16px; }
    .nvc-tile-caption { color: $onSurfaceVariant$; font-size: 82%; font-weight: 600; letter-spacing: 0.4px; }
    .nvc-tile-value { font-size: 140%; font-weight: 700; }
    .nvc-tile-sub { color: $onSurfaceVariant$; font-size: 85%; }
    .nvc-stale { opacity: 0.55; }
    .nvc-muted { color: $onSurfaceVariant$; }
    .nvc-hint { color: $onSurfaceVariant$; font-size: 88%; }
    .nvc-error { color: $error$; }
    .nvc-cmd { font-family: monospace; background-color: $surfaceContainerHighest$; color: $onSurface$;
        border-radius: 8px; padding: 4px 8px; }
    .nvc-chip { border-radius: 999px; padding: 2px 12px; font-size: 85%; font-weight: 600;
        background-color: $surfaceContainerHighest$; color: $onSurfaceVariant$; }
    .nvc-chip-ok { background-color: $success@0.22$; color: $success$; }
    .nvc-chip-warn { background-color: $tertiary@0.22$; color: $tertiary$; }
    .nvc-chip-bad { background-color: $error@0.22$; color: $error$; }
    .nvc-ok { color: $success$; }
    .nvc-warn { color: $tertiary$; }
    .nvc-bad { color: $error$; }
    .nvc-banner { background-color: $errorContainer$; color: $onErrorContainer$;
        border-radius: 12px; padding: 8px 12px; }

    progressbar trough { background-color: $surfaceContainerHighest$; border-radius: 999px; min-height: 8px; }
    progressbar progress { background-color: $primary$; border-radius: 999px; min-height: 8px; }
    progressbar.nvc-warn progress { background-color: $tertiary$; }
    progressbar.nvc-bad progress { background-color: $error$; }

    switch { background-color: $surfaceContainerHighest$; border: 1px solid $outline$; border-radius: 999px; }
    switch:checked { background-color: $primary$; border-color: $primary$; }
    switch slider { background-color: $outline$; border-radius: 999px; }
    switch:checked slider { background-color: $onPrimary$; }
    scale trough { background-color: $surfaceContainerHighest$; border-radius: 999px; min-height: 8px; }
    scale highlight { background-color: $primary$; border-radius: 999px; }
    scale slider { background-color: $primary$; border-radius: 999px; }
    spinbutton, entry { border-radius: 12px; }
    dropdown > button { border-radius: 12px; }
    toast { background-color: $surfaceContainerHighest$; color: $onSurface$; border-radius: 16px; }
    separator { background-color: $outlineVariant$; }
)CSS";

  // Replace each $token$ with its colour; a token has no '$' inside, so the scan is unambiguous.
  std::string out;
  std::size_t pos = 0;
  while (true) {
    const std::size_t open = tpl.find('$', pos);
    if (open == std::string::npos) break;
    const std::size_t close = tpl.find('$', open + 1);
    out.append(tpl, pos, open - pos);
    out += token(tpl.substr(open + 1, close - open - 1));
    pos = close + 1;
  }
  out.append(tpl, pos, std::string::npos);
  return out;
}
