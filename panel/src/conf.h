// Round-trip editor for the players' native key=value config files (mpv script-opts `vsr.conf`, VLC `vlcrc`) and the
// option tables for them. Port of the first half of nvc/conf.py (ConfFile, Opt, tables, parse/format); load and save
// come separately.
//
// Only the keys you set are touched: comments, blank lines, unknown keys and ordering stay as they are.
#pragma once

#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <sys/types.h>  // mode_t

// Line-preserving key=value file. A `#key=value` line is a documented default, not an assignment.
class ConfFile {
 public:
  explicit ConfFile(const std::string &text = "");

  // Value of the last active assignment (what the player uses), or nullopt.
  std::optional<std::string> get(const std::string &key) const;
  // Rewrites the last active assignment; else uncomments a documented `#key=...` line; else appends the key (inside
  // [section] when given, which is created if missing). Throws std::invalid_argument when value spans lines.
  void set(const std::string &key, const std::string &value, const std::string &section = "");
  // Back to the default: the assignment becomes the documented `#key=default_text` line, or disappears when
  // default_text is nullopt or the file already documents the default. No-op when the key is not assigned.
  void unset(const std::string &key, const std::optional<std::string> &default_text = std::nullopt);
  // The file as it was read, with the edits applied (line endings and the final newline are kept).
  std::string render() const;

 private:
  std::string eol_;
  bool final_eol_;
  std::vector<std::string> lines_;

  std::vector<size_t> active(const std::string &key) const;
  std::optional<size_t> commented_default(const std::string &key) const;
  void append(const std::string &line, const std::string &section);
};

enum class Kind {
  Bool,      // yes/no (mpv), 1/0 (vlcrc)
  Int,
  Float,
  OptFloat,  // a number or empty (empty is std::string "")
  Enum,      // one of choices
  EnumInt,   // an integer, choices are the suggested ones
  Choice,    // free text, choices are the suggested ones
  Str,       // free text
};

// Typed option value: bool for Bool, int for Int/EnumInt, double for Float/OptFloat (or "" for an empty OptFloat),
// std::string for Enum/Choice/Str.
using Value = std::variant<bool, int, double, std::string>;

struct ChoiceItem {
  std::string value;  // as written in the file
  std::string label;
};

struct Opt {
  std::string key;
  Kind kind;
  Value def;
  std::string label;
  std::string help;
  double lo = -std::numeric_limits<double>::infinity();  // inclusive range for the numeric kinds
  double hi = std::numeric_limits<double>::infinity();
  int digits = 0;  // spin button decimals
  bool advanced = false;
  std::vector<ChoiceItem> choices;

  Opt(std::string key, Kind kind, Value def, std::string label, std::string help = "")
      : key(std::move(key)), kind(kind), def(std::move(def)), label(std::move(label)), help(std::move(help)) {}

  Opt with_range(double lo_, double hi_, int digits_ = 0) && {
    lo = lo_;
    hi = hi_;
    digits = digits_;
    return std::move(*this);
  }
  Opt with_choices(std::vector<ChoiceItem> c) && {
    choices = std::move(c);
    return std::move(*this);
  }
  Opt as_advanced() && {
    advanced = true;
    return std::move(*this);
  }
};

// Mirror mpv/script-opts/vsr.conf and the `nvvsr-*` options of vlc/src/plugin.c (the tests compare against both).
const std::vector<Opt> &mpv_options();
const std::vector<Opt> &vlc_options();

enum class BoolStyle { YesNo, ZeroOne };

// Typed value for the text found in the file. Throws std::invalid_argument (what() is a user-facing message) when
// the text is not valid for the option.
Value parse_value(const Opt &opt, const std::string &text);
// Text to write into the file. Throws std::invalid_argument when the value cannot be read as a number.
std::string format_value(const Opt &opt, const Value &value, BoolStyle bool_style = BoolStyle::YesNo);
std::string default_text(const Opt &opt, BoolStyle bool_style = BoolStyle::YesNo);

// ---------------------------------------------------------------------------------------------------------------
// Loading and saving (port of the second half of nvc/conf.py)
// ---------------------------------------------------------------------------------------------------------------

inline constexpr const char *kBackupSuffix = ".bak-nvvsr";

struct Loaded {
  std::string path;
  bool exists = false;  // the path names an existing file (also true when it could not be read, see error)
  std::map<std::string, Value> values;                    // key -> typed value (file value or default)
  std::map<std::string, std::optional<std::string>> raw;  // key -> active text in the file, or nullopt
  std::vector<std::string> problems;  // invalid values found (the default is shown instead)
  std::string error;                  // the file exists but cannot be read; saving it is refused too
};

// A missing file (or one below a regular file) loads as all defaults with exists == false.
Loaded load_settings(const std::string &path, const std::vector<Opt> &options);

struct SaveResult {
  bool ok = false;
  bool wrote = false;
  std::vector<std::string> conflicts;  // keys changed by the user and on disk since load; nothing was written
  std::string error;                   // user-facing text when !ok and there are no conflicts
  std::string backup;                  // the .bak-nvvsr copy this save made (first modification only), else empty
};

// Applies the user's edits (key -> typed value) to the file as it is on disk right now. Only keys whose value
// differs from `loaded` are written, in option order, so the rest of the file (comments, unknown keys, edits made
// elsewhere since the load) is merged, never overwritten. If a changed key also changed on disk, nothing is
// written and the keys come back in `conflicts` (the UI offers Reload or Overwrite); force == true overwrites them.
// Values the loader would reject (out of range, not a number, several lines) are refused with `error`. After a
// successful save load again: `loaded` no longer describes the file. A failed save leaves the file as it was.
SaveResult save_changes(const std::string &path, const std::vector<Opt> &options,
                        const std::map<std::string, Value> &edited, const Loaded &loaded,
                        const std::string &section = "", BoolStyle bool_style = BoolStyle::YesNo, bool force = false);

// Both throw std::system_error. atomic_write: temp file in the same directory, fsync, rename; creates missing
// parent directories and leaves no temp file behind on failure. backup_once: writes `text` (the original) to
// path + kBackupSuffix unless that exists; returns the backup path, or "" when one was already there.
void atomic_write(const std::string &path, const std::string &text, mode_t mode);
std::string backup_once(const std::string &path, const std::string &text, mode_t mode);
