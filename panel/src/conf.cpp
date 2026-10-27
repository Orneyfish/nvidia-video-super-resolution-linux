#include "conf.h"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace {

const char *kWs = " \t\n\r\f\v";

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

bool is_key_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

bool blank(const std::string &s) { return std::all_of(s.begin(), s.end(), is_ws); }

// First character after leading whitespace, or '\0'.
char first_char(const std::string &s) {
  const size_t i = s.find_first_not_of(kWs);
  return i == std::string::npos ? '\0' : s[i];
}

std::string strip(const std::string &s) {
  const size_t a = s.find_first_not_of(kWs);
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(kWs) - a + 1);
}

std::string lower(std::string s) {
  for (char &c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// `indent key sep value` of an assignment line: ^(\s*)([A-Za-z0-9_.-]+)(\s*=\s?)(.*?)\s*$
struct Kv {
  std::string indent, key, sep, value;
};

// Comments (#, ;) and [section] headers never match: their first character is not a key character.
std::optional<Kv> match_kv(const std::string &line) {
  const size_t n = line.size();
  size_t i = 0;
  while (i < n && is_ws(line[i])) ++i;
  size_t k = i;
  while (k < n && is_key_char(line[k])) ++k;
  if (k == i) return std::nullopt;
  size_t j = k;
  while (j < n && is_ws(line[j])) ++j;
  if (j >= n || line[j] != '=') return std::nullopt;
  ++j;
  if (j < n && is_ws(line[j])) ++j;
  size_t e = n;
  while (e > j && is_ws(line[e - 1])) --e;
  return Kv{line.substr(0, i), line.substr(i, k - i), line.substr(k, j - k), line.substr(j, e - j)};
}

// printf("%g") in the C locale: GTK sets the user's locale, a decimal comma must never reach a config file.
std::string general(double v) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::general, 6);
  return std::string(buf, r.ptr);
}

// Python round(v, 4).
double round4(double v) {
  char buf[512];
  const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, 4);
  double out = v;
  if (r.ec == std::errc()) std::from_chars(buf, r.ptr, out);
  return out;
}

// Like Python float(): a leading '+' is fine, hex and trailing junk are not.
std::optional<double> to_number(const std::string &t) {
  const char *b = t.data(), *e = b + t.size();
  if (b != e && *b == '+' && (e - b < 2 || (b[1] != '+' && b[1] != '-'))) ++b;
  double v = 0;
  const auto [p, ec] = std::from_chars(b, e, v);
  if (ec != std::errc() || p != e) return std::nullopt;
  return v;
}

bool is_text(const Value &v) { return std::holds_alternative<std::string>(v); }

double as_number(const Value &v) {
  if (const bool *b = std::get_if<bool>(&v)) return *b ? 1 : 0;
  if (const int *i = std::get_if<int>(&v)) return *i;
  if (const double *d = std::get_if<double>(&v)) return *d;
  if (const auto n = to_number(std::get<std::string>(v))) return *n;
  throw std::invalid_argument("'" + std::get<std::string>(v) + "' is not a number");
}

bool truthy(const Value &v) { return is_text(v) ? !std::get<std::string>(v).empty() : as_number(v) != 0; }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// ConfFile
// ---------------------------------------------------------------------------------------------------------------

ConfFile::ConfFile(const std::string &text)
    : eol_(text.find("\r\n") != std::string::npos ? "\r\n" : "\n"), final_eol_(text.empty() || text.back() == '\n') {
  std::string cur;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;  // CRLF is read as LF
    if (text[i] == '\n') {
      lines_.push_back(cur);
      cur.clear();
    } else {
      cur += text[i];
    }
  }
  lines_.push_back(cur);
  if (lines_.back().empty()) lines_.pop_back();  // the text after the last newline
}

std::vector<size_t> ConfFile::active(const std::string &key) const {
  std::vector<size_t> out;
  for (size_t i = 0; i < lines_.size(); ++i)
    if (const auto kv = match_kv(lines_[i]); kv && kv->key == key) out.push_back(i);
  return out;
}

// `#key=` or `# key=` (one optional blank), so prose such as "# quality is great" is not taken for a default.
std::optional<size_t> ConfFile::commented_default(const std::string &key) const {
  for (size_t i = 0; i < lines_.size(); ++i) {
    const std::string &l = lines_[i];
    if (l.empty() || l[0] != '#') continue;
    size_t j = 1;
    if (j < l.size() && is_ws(l[j])) ++j;
    if (l.compare(j, key.size(), key) != 0) continue;
    j += key.size();
    while (j < l.size() && is_ws(l[j])) ++j;
    if (j < l.size() && l[j] == '=') return i;
  }
  return std::nullopt;
}

std::optional<std::string> ConfFile::get(const std::string &key) const {
  const auto idx = active(key);
  if (idx.empty()) return std::nullopt;
  return match_kv(lines_[idx.back()])->value;
}

void ConfFile::set(const std::string &key, const std::string &value, const std::string &section) {
  if (value.find_first_of("\r\n") != std::string::npos) throw std::invalid_argument("value must be a single line");
  const auto idx = active(key);
  if (!idx.empty()) {
    const auto kv = *match_kv(lines_[idx.back()]);
    lines_[idx.back()] = kv.indent + key + kv.sep + value;
    return;
  }
  if (const auto c = commented_default(key)) {
    lines_[*c] = key + "=" + value;
    return;
  }
  append(key + "=" + value, section);
}

void ConfFile::unset(const std::string &key, const std::optional<std::string> &default_text) {
  const auto idx = active(key);
  if (idx.empty()) return;
  for (size_t k = idx.size() - 1; k-- > 0;)  // earlier duplicates, last first so the indices stay valid
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(idx[k]));
  const size_t i = idx.back() - (idx.size() - 1);
  if (default_text && !commented_default(key))
    lines_[i] = "#" + key + "=" + *default_text;
  else
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(i));
}

void ConfFile::append(const std::string &line, const std::string &section) {
  if (section.empty()) {
    lines_.push_back(line);
    return;
  }
  const std::string header = "[" + section + "]";
  // vlcrc headers carry a description: "[nvvsr] # NVIDIA VSR"
  const auto is_header = [&](const std::string &l) {
    if (l.compare(0, header.size(), header) != 0) return false;
    const size_t i = l.find_first_not_of(kWs, header.size());
    return i == std::string::npos || l[i] == '#';
  };
  const auto h = std::find_if(lines_.begin(), lines_.end(), is_header);
  if (h != lines_.end()) {
    auto at = std::find_if(h + 1, lines_.end(), [](const std::string &l) { return first_char(l) == '['; });
    while (at > h + 1 && blank(*(at - 1))) --at;  // before the blank lines that end the section
    lines_.insert(at, line);
    return;
  }
  if (!lines_.empty() && !blank(lines_.back())) lines_.push_back("");
  lines_.push_back(header);
  lines_.push_back(line);
}

std::string ConfFile::render() const {
  std::string out;
  for (size_t i = 0; i < lines_.size(); ++i) {
    if (i) out += eol_;
    out += lines_[i];
  }
  if (final_eol_ && !lines_.empty()) out += eol_;
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Option tables
// ---------------------------------------------------------------------------------------------------------------

namespace {

const std::vector<ChoiceItem> &quality_choices() {
  static const std::vector<ChoiceItem> c = {
      {"auto", "Automatic"},      {"bicubic", "Bicubic (no AI)"},         {"low", "Low"},
      {"medium", "Medium"},       {"high", "High"},                       {"ultra", "Ultra"},
      {"streaming_medium", "Streaming medium"}, {"streaming_ultra", "Streaming ultra"},
  };
  return c;
}

}  // namespace

// Mirrors mpv/script-opts/vsr.conf and M.defaults / M.normalize in mpv/vsr.lua.
const std::vector<Opt> &mpv_options() {
  static const std::vector<Opt> opts = {
      Opt("enabled", Kind::Bool, true, "Automatic super resolution",
          "Master switch for the automatic mode. With off nothing happens by itself, but the toggle key (Ctrl+s) "
          "still turns SR on for the current file."),
      Opt("ac_only", Kind::Bool, true, "Only on AC power",
          "Only run SR on AC power: the RTX draws about 150 W while it works. Mains and USB-C PD supplies count as "
          "AC."),
      Opt("quality", Kind::Enum, "auto", "Quality",
          "auto picks by source height and frame rate: up to 540p Ultra, 541-800p High, above 800p Medium (very high "
          "frame rates one level lower). The adaptive step-down starts from this level.")
          .with_choices(quality_choices()),
      Opt("min_height", Kind::Int, 360, "Smallest source height", "Smallest source height (pixels) worth upscaling.")
          .with_range(0, 4320),
      Opt("adaptive", Kind::Bool, true, "Adaptive quality",
          "When frames are dropped, SR restarts one quality level lower (Ultra, High, Medium, Low, Bicubic) and never "
          "goes back up during the same file. If Bicubic cannot keep up either, SR is switched off for the file."),
      Opt("hwdec", Kind::Choice, "copy", "Hardware decoding",
          "The VapourSynth filter needs frames in system memory. copy: while SR is active switch your hwdec to its "
          "-copy variant and restore it afterwards. keep: leave hwdec alone. Anything else is used as the hwdec "
          "value while SR is active, e.g. nvdec-copy.")
          .with_choices({{"copy", "Use -copy variant"},
                         {"keep", "Keep as is"},
                         {"nvdec-copy", "nvdec-copy"},
                         {"vaapi-copy", "vaapi-copy"},
                         {"auto-copy", "auto-copy"}}),
      Opt("convert_10bit", Kind::Bool, true, "Convert 10-bit video",
          "10-bit / 4:2:2 / 4:4:4 sources: yes lets mpv convert them to 8-bit 4:2:0 in front of the filter (the SR "
          "engine is 8-bit); no skips SR for them."),
      Opt("retry_on_next_file", Kind::Bool, true, "Retry on the next file",
          "After a failure SR stays off for that file. With yes it tries again on the next file."),
      Opt("osd", Kind::Bool, true, "Status badge",
          "A small, semi-transparent badge with one or two words (SR, SR off, SR failed) in a screen corner. "
          "Ctrl+Alt+s toggles it while playing."),
      Opt("osd_size", Kind::Int, 11, "Badge text size",
          "Text size at a 1280x720 scale (11 is about 2% of the screen height).")
          .with_range(6, 40),
      Opt("osd_opacity", Kind::Float, 0.55, "Badge opacity", "How opaque the badge is (0.1 to 1).")
          .with_range(0.1, 1, 2),
      Opt("osd_position", Kind::EnumInt, 9, "Badge corner",
          "Numpad layout: 9 top right, 7 top left, 3 bottom right, 1 bottom left, 8 top centre, 2 bottom centre.")
          .with_choices({{"9", "Top right"},
                         {"7", "Top left"},
                         {"3", "Bottom right"},
                         {"1", "Bottom left"},
                         {"8", "Top centre"},
                         {"2", "Bottom centre"}})
          .with_range(1, 9),
      Opt("osd_duration", Kind::Float, 1.5, "Badge duration (s)",
          "Seconds the badge stays up; 0 keeps it until the next message.")
          .with_range(0, 30, 1),
      Opt("pool", Kind::Int, 1, "Concurrent contexts",
          "Concurrent libnvvsr contexts (1-4). Every extra one costs about 250 MiB of VRAM, one already saturates the "
          "GPU.")
          .with_range(1, 4),
      Opt("strength", Kind::OptFloat, "", "Strength", "Strength 0..1 of the SR effect. Empty uses the SDK default.")
          .with_range(0, 1, 2),
      // advanced
      Opt("check_interval", Kind::Int, 30, "Power check interval (s)", "Seconds between power-source checks.")
          .with_range(1, 3600)
          .as_advanced(),
      Opt("slop", Kind::Float, 0.02, "Minimum gain",
          "Do nothing if the fitted picture would be less than this much (0.02 = 2%) larger than the source.")
          .with_range(0, 1, 2)
          .as_advanced(),
      Opt("adaptive_window", Kind::Float, 5.0, "Adaptive window (s)", "Window in which dropped frames are counted.")
          .with_range(1, 120)
          .as_advanced(),
      Opt("adaptive_drops", Kind::Int, 10, "Adaptive drop threshold",
          "Dropped frames within the window that trigger a step down.")
          .with_range(1, 10000)
          .as_advanced(),
      Opt("settle", Kind::Float, 2.0, "Settle time (s)",
          "Nothing is judged for this many seconds after (re)starting SR.")
          .with_range(0, 60, 1)
          .as_advanced(),
      Opt("confirm_timeout", Kind::Float, 8.0, "Confirm timeout (s)",
          "SR counts as failed if no scaled frames arrive within this many seconds.")
          .with_range(1, 120)
          .as_advanced(),
      Opt("stats_interval", Kind::Float, 1.0, "Stats refresh (s)", "Refresh interval of the Alt+s stats overlay.")
          .with_range(0.2, 10, 1)
          .as_advanced(),
      Opt("buffered_frames", Kind::Int, 4, "Buffered frames", "Frames mpv buffers in the filter.")
          .with_range(1, 16)
          .as_advanced(),
      Opt("concurrent_frames", Kind::Int, 2, "Concurrent frames", "Frames mpv renders concurrently in the filter.")
          .with_range(1, 16)
          .as_advanced(),
  };
  return opts;
}

// Mirrors the `nvvsr-*` options in vlc/src/plugin.c.
const std::vector<Opt> &vlc_options() {
  static const std::vector<Opt> opts = {
      Opt("nvvsr-enable", Kind::Bool, true, "Enable super resolution",
          "Master switch of the nvvsr video output. When disabled VLC uses its normal video output."),
      Opt("nvvsr-quality", Kind::Enum, "auto", "Quality",
          "auto picks it from the source height and frame rate (up to 540p ultra, up to 800p high, otherwise medium). "
          "bicubic uses no neural network.")
          .with_choices(quality_choices()),
      Opt("nvvsr-fullscreen-only", Kind::Bool, true, "Only while fullscreen",
          "Upscale only while the video is fullscreen. When off it also runs in a window larger than the video."),
      Opt("nvvsr-ac-only", Kind::Bool, true, "Only on mains power",
          "Do not run super resolution on battery (the GPU draws about 150 W). Checked every 30 seconds."),
      Opt("nvvsr-min-height", Kind::Int, 360, "Smallest source height",
          "Sources lower than this many lines are left alone.")
          .with_range(0, 4320),
      Opt("nvvsr-min-gain", Kind::Int, 2, "Minimum gain (percent)",
          "The video must be at least this many percent smaller than the display (fitted) to be upscaled.")
          .with_range(0, 100),
      Opt("nvvsr-width", Kind::Int, 0, "Panel width",
          "Width of the display in pixels. 0 uses the size of the video window (the screen while fullscreen).")
          .with_range(0, 16384)
          .as_advanced(),
      Opt("nvvsr-height", Kind::Int, 0, "Panel height", "Height of the display in pixels, see the width.")
          .with_range(0, 16384)
          .as_advanced(),
      Opt("nvvsr-vout", Kind::Str, "", "Real video output",
          "Video output module that shows the pictures (for example gl, xcb_x11, wl_shm). Empty selects it "
          "automatically, like VLC would without nvvsr.")
          .as_advanced(),
  };
  return opts;
}

// ---------------------------------------------------------------------------------------------------------------
// parse / format
// ---------------------------------------------------------------------------------------------------------------

Value parse_value(const Opt &opt, const std::string &text) {
  const std::string t = strip(text);
  if (opt.kind == Kind::Bool) {
    const std::string v = lower(t);
    for (const char *s : {"yes", "true", "1", "on"})
      if (v == s) return true;
    for (const char *s : {"no", "false", "0", "off"})
      if (v == s) return false;
    throw std::invalid_argument(opt.key + ": expected yes or no, found '" + t + "'");
  }
  if (opt.kind == Kind::Int || opt.kind == Kind::Float || opt.kind == Kind::OptFloat || opt.kind == Kind::EnumInt) {
    if (opt.kind == Kind::OptFloat && t.empty()) return std::string();
    const auto n = to_number(t);
    if (!n) throw std::invalid_argument(opt.key + ": '" + t + "' is not a number");
    const double v = *n;
    if (std::isnan(v) || v < opt.lo || v > opt.hi)
      throw std::invalid_argument(opt.key + ": " + t + " is outside " + general(opt.lo) + ".." + general(opt.hi));
    if (opt.kind == Kind::Int || opt.kind == Kind::EnumInt) {
      if (v != std::trunc(v)) throw std::invalid_argument(opt.key + ": '" + t + "' is not a whole number");
      if (std::fabs(v) > INT_MAX) throw std::invalid_argument(opt.key + ": " + t + " is too large");
      return static_cast<int>(v);
    }
    return v;
  }
  if (opt.kind == Kind::Enum) {
    std::string v = lower(t);
    v.erase(std::remove(v.begin(), v.end(), ' '), v.end());
    for (const ChoiceItem &c : opt.choices)
      if (c.value == v) return v;
    throw std::invalid_argument(opt.key + ": unknown value '" + t + "'");
  }
  return t;  // Choice / Str: any single-line text
}

std::string format_value(const Opt &opt, const Value &value, BoolStyle bool_style) {
  if (opt.kind == Kind::Bool) {
    const bool on = truthy(value);
    return bool_style == BoolStyle::ZeroOne ? (on ? "1" : "0") : (on ? "yes" : "no");
  }
  if (opt.kind == Kind::Float || opt.kind == Kind::OptFloat) {
    if (const auto *s = std::get_if<std::string>(&value); s && s->empty()) return "";
    return general(round4(as_number(value)));
  }
  if (opt.kind == Kind::Int || opt.kind == Kind::EnumInt) {
    const double v = std::trunc(as_number(value));
    if (!(std::fabs(v) < 9e18)) throw std::invalid_argument(opt.key + ": number is out of range");
    return std::to_string(static_cast<long long>(v));
  }
  return is_text(value) ? std::get<std::string>(value) : general(as_number(value));
}

std::string default_text(const Opt &opt, BoolStyle bool_style) { return format_value(opt, opt.def, bool_style); }
