// Port of the ConfFile / Opt / parse_value / format_value tests in panel/tests/test_conf.py (ConfFileEdits and Values).
// The load/save tests (SaveChanges, VlcSave) belong to the next module.
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

#include "check.h"
#include "conf.h"

namespace {

template <class F>
bool throws(F f) {
  try {
    f();
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

const Opt &opt(const std::string &key, const std::vector<Opt> &options = mpv_options()) {
  for (const Opt &o : options)
    if (o.key == key) return o;
  throw std::logic_error("no option " + key);
}

Value parsed(const std::string &key, const std::string &text) { return parse_value(opt(key), text); }

std::string slurp(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ---- ConfFileEdits -------------------------------------------------------------------------------------------

void test_roundtrip_is_identity() {
  for (const char *text : {"", "a=1\n", "# c\n\na=1\nb = 2\n\n", "a=1", "x=1\r\ny=2\r\n"})
    CHECK_EQ(ConfFile(text).render(), std::string(text));
}

void test_set_replaces_active_keeps_everything_else() {
  ConfFile c("# top\nquality = high\nunknown=keep me\n#osd=yes\n");
  c.set("quality", "low");
  CHECK_EQ(c.render(), std::string("# top\nquality = low\nunknown=keep me\n#osd=yes\n"));
}

void test_set_uncomments_documented_default_in_place() {
  ConfFile c("# doc\n#quality=auto\n# more doc\n");
  c.set("quality", "high");
  CHECK_EQ(c.render(), std::string("# doc\nquality=high\n# more doc\n"));
}

void test_prose_comment_is_not_taken_for_a_default() {
  ConfFile c("# quality is great\n# Quality: auto | low\n");
  c.set("quality", "low");
  CHECK_EQ(c.render(), std::string("# quality is great\n# Quality: auto | low\nquality=low\n"));
}

void test_last_assignment_wins_like_the_player() {
  ConfFile c("a=1\na=2\n");
  CHECK_EQ(c.get("a").value_or("none"), std::string("2"));
  c.set("a", "3");
  CHECK_EQ(c.render(), std::string("a=1\na=3\n"));
}

void test_unset_restores_documented_default() {
  ConfFile c("quality=high\nother=1\n");
  c.unset("quality", "auto");
  CHECK_EQ(c.render(), std::string("#quality=auto\nother=1\n"));
  ConfFile d("#quality=auto\nquality=high\n");
  d.unset("quality", "auto");
  CHECK_EQ(d.render(), std::string("#quality=auto\n"));
}

void test_section_append_goes_into_existing_section() {
  ConfFile c("[a]\nx=1\n\n[nvvsr]\nfoo=1\n\n[z]\nq=1\n");
  c.set("nvvsr-quality", "high", "nvvsr");
  CHECK_EQ(c.render(), std::string("[a]\nx=1\n\n[nvvsr]\nfoo=1\nnvvsr-quality=high\n\n[z]\nq=1\n"));
}

void test_section_created_when_missing() {
  ConfFile c("[core]\nv=1\n");
  c.set("nvvsr-enable", "0", "nvvsr");
  CHECK_EQ(c.render(), std::string("[core]\nv=1\n\n[nvvsr]\nnvvsr-enable=0\n"));
}

void test_multiline_value_rejected() {
  CHECK(throws([] { ConfFile("").set("a", "x\ny"); }), "a value with a newline throws");
  CHECK(throws([] { ConfFile("").set("a", "x\ry"); }), "a value with a carriage return throws");
}

// Cases beyond the Python tests: the branches the port had to write by hand.
void test_edge_cases() {
  ConfFile c("  indented  =  spaced  \n;semi=1\n[s]\nk=1\n");
  CHECK_EQ(c.get("indented").value_or("none"), std::string(" spaced"));  // one blank after '=' belongs to the separator
  CHECK(!c.get("semi"), "a ;comment is not an assignment");
  CHECK(!c.get("s"), "a [section] header is not an assignment");
  c.set("indented", "x");
  CHECK_EQ(c.render(), std::string("  indented  = x\n;semi=1\n[s]\nk=1\n"));  // indent and separator survive

  ConfFile crlf("a=1\r\n#b=2\r\n");
  crlf.set("b", "3");
  crlf.set("c", "4");
  CHECK_EQ(crlf.render(), std::string("a=1\r\nb=3\r\nc=4\r\n"));

  ConfFile dup("a=1\nx\na=2\na=3\n");
  dup.unset("a");
  CHECK_EQ(dup.render(), std::string("x\n"));  // no default text: every assignment goes
  dup.unset("missing", "d");
  CHECK_EQ(dup.render(), std::string("x\n"));

  ConfFile nofinal("a=1");
  nofinal.set("b", "2");
  CHECK_EQ(nofinal.render(), std::string("a=1\nb=2"));  // no final newline stays that way

  ConfFile sect("[nvvsr] # NVIDIA VSR\nk=1");
  sect.set("n", "2", "nvvsr");
  CHECK_EQ(sect.render(), std::string("[nvvsr] # NVIDIA VSR\nk=1\nn=2"));  // a section at the very end
}

// ---- Values --------------------------------------------------------------------------------------------------

void test_parse_and_validate() {
  CHECK(parsed("ac_only", " No ") == Value(false), "bool, trimmed, any case");
  CHECK(parsed("ac_only", "ON") == Value(true), "on is true");
  CHECK(parsed("pool", "3") == Value(3), "int");
  CHECK(parsed("osd_opacity", "0.7") == Value(0.7), "float");
  CHECK(parsed("osd_opacity", "+.5") == Value(0.5), "float with a leading plus");
  CHECK(parsed("osd_position", "4") == Value(4), "enumint accepts a value outside the suggested choices");
  CHECK(parsed("strength", "") == Value(std::string()), "an empty optional float stays empty");
  CHECK(parsed("quality", "High") == Value(std::string("high")), "enum is lower-cased");
  CHECK(parsed("quality", "stream ing_medium") == Value(std::string("streaming_medium")), "enum drops spaces");
  CHECK(parsed("hwdec", " vaapi ") == Value(std::string("vaapi")), "choice is any trimmed text");
  const char *bad[][2] = {{"pool", "9"},    {"pool", "1.5"},    {"osd_opacity", "0"}, {"ac_only", "maybe"},
                          {"quality", "ultraa"}, {"strength", "2"}, {"min_height", "x"}};
  for (const auto &b : bad)
    CHECK(throws([&] { parsed(b[0], b[1]); }), "%s=%s is rejected", b[0], b[1]);
  CHECK(throws([] { parsed("pool", "nan"); }), "nan is rejected");
  CHECK(throws([] { parsed("pool", "0x2"); }), "hex is rejected");
  CHECK(throws([] { parsed("strength", "+-1"); }), "a double sign is rejected");
  CHECK(throws([] { parsed("osd_opacity", ""); }), "an empty plain float is rejected");
  try {
    parsed("pool", "9");
  } catch (const std::invalid_argument &e) {
    CHECK_EQ(std::string(e.what()), std::string("pool: 9 is outside 1..4"));
  }
}

void test_format() {
  CHECK_EQ(format_value(opt("ac_only"), true), std::string("yes"));
  CHECK_EQ(format_value(opt("nvvsr-ac-only", vlc_options()), false, BoolStyle::ZeroOne), std::string("0"));
  CHECK_EQ(format_value(opt("osd_opacity"), 0.55000001), std::string("0.55"));
  CHECK_EQ(format_value(opt("osd_duration"), 2.0), std::string("2"));
  CHECK_EQ(format_value(opt("pool"), 2.0), std::string("2"));
  CHECK_EQ(format_value(opt("strength"), std::string()), std::string(""));
  CHECK_EQ(format_value(opt("quality"), std::string("high")), std::string("high"));
  CHECK_EQ(default_text(opt("quality")), std::string("auto"));
  CHECK_EQ(default_text(opt("osd")), std::string("yes"));
  CHECK_EQ(default_text(opt("nvvsr-enable", vlc_options()), BoolStyle::ZeroOne), std::string("1"));
  CHECK_EQ(default_text(opt("slop")), std::string("0.02"));
  CHECK(throws([] { format_value(opt("pool"), std::string("x")); }), "a non-numeric text for an int throws");
}

// The defaults here must not drift from mpv/script-opts/vsr.conf and vlc/src/plugin.c.
void test_option_tables_match_the_players_own_files() {
  const std::filesystem::path repo = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();

  CHECK(std::get_if<std::string>(&opt("quality").def) != nullptr, "a string default is stored as std::string");
  for (const auto *table : {&mpv_options(), &vlc_options()}) {
    CHECK(!table->empty(), "table is not empty");
    for (const Opt &o : *table) {
      const bool numeric = o.kind == Kind::Int || o.kind == Kind::Float || o.kind == Kind::EnumInt;
      CHECK(!o.label.empty() && !o.help.empty() && (!numeric || o.lo <= o.hi), "%s is complete", o.key.c_str());
    }
  }

  std::map<std::string, std::string> documented;  // `#key=value` lines of the shipped vsr.conf
  std::istringstream conf(slurp(repo / "mpv" / "script-opts" / "vsr.conf"));
  for (std::string line; std::getline(conf, line);) {
    size_t k = 1;
    while (k < line.size() && ((line[k] >= 'a' && line[k] <= 'z') || line[k] == '_' || (line[k] >= '0' && line[k] <= '9'))) ++k;
    if (line.size() > k && line[0] == '#' && k > 1 && line[k] == '=') documented[line.substr(1, k - 1)] = line.substr(k + 1);
  }
  CHECK(!documented.empty(), "read %zu documented defaults from the repo vsr.conf", documented.size());
  for (const Opt &o : mpv_options()) {
    const auto it = documented.find(o.key);
    CHECK(it != documented.end(), "%s is documented in vsr.conf", o.key.c_str());
    if (it == documented.end()) continue;
    bool same = false;
    try {
      same = parse_value(o, it->second) == o.def;
    } catch (const std::invalid_argument &) {
    }
    CHECK(same, "%s default matches vsr.conf (%s)", o.key.c_str(), it->second.c_str());
  }

  const std::string src = slurp(repo / "vlc" / "src" / "plugin.c");
  CHECK(!src.empty(), "read vlc/src/plugin.c");
  for (const Opt &o : vlc_options()) {
    const std::string needle = "NV_CFG \"" + o.key.substr(std::string("nvvsr-").size()) + "\", ";
    const size_t at = src.find(needle);
    CHECK(at != std::string::npos, "%s is declared in plugin.c", o.key.c_str());
    if (at == std::string::npos) continue;
    const size_t from = at + needle.size();
    std::string raw = src.substr(from, src.find(',', from) - from);  // the default is the first argument after the name
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') raw = raw.substr(1, raw.size() - 2);
    Value want = raw;
    if (raw == "true" || raw == "false") want = raw == "true";
    else if (!raw.empty() && raw.find_first_not_of("0123456789") == std::string::npos) want = std::stoi(raw);
    CHECK(o.def == want, "%s default matches plugin.c (%s)", o.key.c_str(), raw.c_str());
  }
}

}  // namespace

int main() {
  test_roundtrip_is_identity();
  test_set_replaces_active_keeps_everything_else();
  test_set_uncomments_documented_default_in_place();
  test_prose_comment_is_not_taken_for_a_default();
  test_last_assignment_wins_like_the_player();
  test_unset_restores_documented_default();
  test_section_append_goes_into_existing_section();
  test_section_created_when_missing();
  test_multiline_value_rejected();
  test_edge_cases();
  test_parse_and_validate();
  test_format();
  test_option_tables_match_the_players_own_files();
  return finish("test_conf_core");
}
