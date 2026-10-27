#include "smi_q.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <sstream>
#include <stdexcept>

namespace {

bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_word(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && is_space(s[a])) ++a;
  while (b > a && is_space(s[b - 1])) --b;
  return s.substr(a, b - a);
}

std::string lower(std::string s) {
  for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool contains(const std::string &hay, const char *needle) { return hay.find(needle) != std::string::npos; }

size_t skip_space(const std::string &s, size_t i) {
  while (i < s.size() && is_space(s[i])) ++i;
  return i;
}

size_t skip_digits(const std::string &s, size_t i) {
  while (i < s.size() && is_digit(s[i])) ++i;
  return i;
}

// "Key : value" where the colon has whitespace before it and a key in front; the value is trimmed.
bool split_kv(const std::string &line, std::string &key, std::string &value) {
  for (size_t c = 0; c < line.size(); ++c) {
    if (line[c] != ':') continue;
    size_t k = c;
    while (k > 0 && is_space(line[k - 1])) --k;
    if (k == c || k == 0) continue;
    if (c + 1 < line.size() && !is_space(line[c + 1])) continue;  // "Key:value" is a section name
    key = line.substr(0, k);
    value = trim(line.substr(c + 1));
    return true;
  }
  return false;
}

std::string unique_key(const SmiNode &parent, const std::string &key) {
  auto taken = [&](const std::string &k) {
    return std::any_of(parent.children.begin(), parent.children.end(), [&](const auto &e) { return e.first == k; });
  };
  if (!taken(key)) return key;
  int i = 2;
  while (taken(key + "#" + std::to_string(i))) ++i;
  return key + "#" + std::to_string(i);
}

// Leading "<number> W" (word boundary after W), as Python's ^\s*([0-9]+(\.[0-9]+)?)\s*W\b.
std::optional<double> parse_watts(const std::string &v) {
  size_t i = skip_space(v, 0);
  const size_t start = i;
  i = skip_digits(v, i);
  if (i == start) return std::nullopt;
  if (i + 1 < v.size() && v[i] == '.' && is_digit(v[i + 1])) i = skip_digits(v, i + 1);
  const std::string number = v.substr(start, i - start);
  i = skip_space(v, i);
  if (i >= v.size() || v[i] != 'W') return std::nullopt;
  if (i + 1 < v.size() && is_word(v[i + 1])) return std::nullopt;
  return std::strtod(number.c_str(), nullptr);
}

// Leading "<integer> MHz" (word boundary after MHz).
std::optional<int> parse_mhz(const std::string &v) {
  size_t i = skip_space(v, 0);
  const size_t start = i;
  i = skip_digits(v, i);
  if (i == start || i - start > 9) return std::nullopt;  // no clock is this long; keeps stoi safe
  const int mhz = std::stoi(v.substr(start, i - start));
  i = skip_space(v, i);
  if (v.compare(i, 3, "MHz") != 0) return std::nullopt;
  i += 3;
  if (i < v.size() && is_word(v[i])) return std::nullopt;
  return mhz;
}

// Watts of a leaf; sections and "N/A" give nullopt.
std::optional<double> watts(const SmiNode *n) {
  if (!n || n->is_section) return std::nullopt;
  return parse_watts(n->value);
}

const SmiNode *child(const SmiNode &node, const std::string &key) {
  for (const auto &e : node.children)
    if (e.first == key) return &e.second;
  return nullptr;
}

// First sub-section whose lower-case name contains a needle and no exclude.
const SmiNode *section(const SmiNode &node, std::initializer_list<const char *> needles,
                       std::initializer_list<const char *> exclude = {}) {
  for (const auto &[k, v] : node.children) {
    if (!v.is_section) continue;
    const std::string lk = lower(k);
    const bool wanted = std::any_of(needles.begin(), needles.end(), [&](const char *n) { return contains(lk, n); });
    const bool skipped = std::any_of(exclude.begin(), exclude.end(), [&](const char *x) { return contains(lk, x); });
    if (wanted && !skipped) return &v;
  }
  return nullptr;
}

const SmiNode &first_gpu_node(const SmiNode &tree) {
  for (const auto &[k, v] : tree.children)
    if (v.is_section && k.rfind("GPU ", 0) == 0) return v;
  return tree;
}

std::map<std::string, int> clocks_of(const SmiNode &gpu, const char *section_key) {
  static const std::pair<const char *, const char *> kinds[] = {
      {"graphics", "Graphics"}, {"sm", "SM"}, {"memory", "Memory"}, {"video", "Video"}};
  std::map<std::string, int> out;
  const SmiNode *sec = child(gpu, section_key);
  if (!sec || !sec->is_section) return out;
  for (const auto &[label, key] : kinds) {
    const SmiNode *n = child(*sec, key);
    if (!n || n->is_section) continue;
    if (auto mhz = parse_mhz(n->value)) out[label] = *mhz;
  }
  return out;
}

}  // namespace

SmiNode parse_smi_q(const std::string &text) {
  SmiNode root;
  root.is_section = true;
  // (indent, node) pairs; pointers stay valid because a node's children are only added while it is the top.
  std::vector<std::pair<int, SmiNode *>> stack = {{-1, &root}};
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    const std::string raw = text.substr(pos, nl - pos);
    pos = nl + 1;
    const std::string line = trim(raw);
    if (line.empty() || line[0] == '=' || line.rfind("Timestamp", 0) == 0) continue;
    int indent = 0;
    while (indent < static_cast<int>(raw.size()) && raw[indent] == ' ') ++indent;
    while (stack.size() > 1 && stack.back().first >= indent) stack.pop_back();
    SmiNode &parent = *stack.back().second;
    std::string key, value;
    if (split_kv(line, key, value)) {
      SmiNode leaf;
      leaf.value = value;
      parent.children.emplace_back(unique_key(parent, key), std::move(leaf));
    } else {
      SmiNode node;
      node.is_section = true;
      parent.children.emplace_back(unique_key(parent, line), std::move(node));
      stack.emplace_back(indent, &parent.children.back().second);
    }
  }
  return root;
}

DetailInfo parse_power_page(const std::string &text) {
  const SmiNode tree = parse_smi_q(text);
  const SmiNode &gpu = first_gpu_node(tree);
  DetailInfo info;

  const SmiNode *pr = section(gpu, {"power readings"}, {"memory", "module"});
  const SmiNode &flat = pr ? *pr : gpu;
  // Driver 615: the ceiling section's keys win over the flat ones (Python merged the two dicts).
  const SmiNode *ceil_node = child(flat, "GPU Ceiling Power Limit");
  const SmiNode *ceiling = ceil_node && ceil_node->is_section ? ceil_node : nullptr;
  auto field = [&](const std::string &name) -> const SmiNode * {
    if (ceiling)
      if (const SmiNode *c = child(*ceiling, name)) return c;
    return child(flat, name);
  };
  auto get = [&](std::initializer_list<const char *> names) -> std::optional<double> {
    for (const char *n : names)
      if (auto w = watts(field(n))) return w;
    return std::nullopt;
  };

  PowerInfo &p = info.power;
  p.draw_w = get({"Instantaneous Power Draw", "Average Power Draw", "Power Draw"});
  p.limit_w = get({"Current Power Limit", "Power Limit"});
  p.requested_w = get({"Requested Power Limit"});
  p.default_w = get({"Default Power Limit"});
  p.min_w = get({"Min Power Limit"});
  p.max_w = get({"Max Power Limit"});
  for (const char *k : {"Power Management", "Power Management Mode"}) {
    const SmiNode *n = field(k);
    if (n && !n->is_section && !n->value.empty()) {
      p.management = n->value;
      break;
    }
  }

  info.clocks = clocks_of(gpu, "Clocks");
  info.max_clocks = clocks_of(gpu, "Max Clocks");

  if (const SmiNode *reasons = section(gpu, {"clocks event reasons", "clocks throttle reasons"})) {
    info.has_limiter_info = true;
    for (const auto &[k, v] : reasons->children)
      if (!v.is_section && lower(v.value) == "active") info.limiters.push_back(k);
  }
  return info;
}

PowerQuery query_power_page(const SmiRunner &runner) {
  const CmdResult r = runner({"nvidia-smi", "-q", "-d", "POWER,CLOCK,PERFORMANCE"}, 10.0, {});
  if (!r.ok()) return {std::nullopt, r.error_text()};
  return {parse_power_page(r.out), ""};
}

std::vector<std::string> power_limit_command(double watts_value, const PowerInfo *info) {
  if (!std::isfinite(watts_value) || watts_value != std::floor(watts_value) || std::fabs(watts_value) > 1e9)
    throw std::invalid_argument("power limit must be a whole number of watts");
  if (watts_value <= 0) throw std::invalid_argument("power limit must be positive");
  const long long w = static_cast<long long>(watts_value);
  if (info) {
    if (info->min_w && w < static_cast<long long>(*info->min_w)) {
      std::ostringstream os;
      os << "below the card minimum of " << *info->min_w << " W";
      throw std::invalid_argument(os.str());
    }
    if (info->max_w && w > static_cast<long long>(*info->max_w)) {
      std::ostringstream os;
      os << "above the card maximum of " << *info->max_w << " W";
      throw std::invalid_argument(os.str());
    }
  }
  return {"sudo", "nvidia-smi", "-pl", std::to_string(w)};
}

std::vector<std::string> persistence_command(bool enable) {
  return {"sudo", "nvidia-smi", "-pm", enable ? "1" : "0"};
}
