// Port of SaveChanges (10) and VlcSave (1) in panel/tests/test_conf.py, plus the failure modes of a program that
// rewrites the user's real config files: read-only directory, vanished file, symlinks, empty file, no final newline,
// very long lines, a write that fails half way, unreadable files, bad values. Everything lives in a temp directory.
#include <csignal>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "conf.h"

namespace {

namespace fs = std::filesystem;

// A fresh directory under $TMPDIR (or /tmp), removed on destruction even when a test left a directory unreadable.
class Scratch {
 public:
  Scratch() {
    std::string tmpl = (fs::temp_directory_path() / "nvc_conf_io_XXXXXX").string();
    if (!mkdtemp(tmpl.data())) throw std::runtime_error("mkdtemp failed");
    root_ = tmpl;
  }
  ~Scratch() {
    std::error_code ec;
    try {
      for (const auto &e : fs::recursive_directory_iterator(root_, fs::directory_options::skip_permission_denied, ec))
        if (e.is_directory(ec) && !e.is_symlink(ec)) fs::permissions(e.path(), fs::perms::owner_all, ec);
    } catch (const fs::filesystem_error &) {
    }
    fs::permissions(root_, fs::perms::owner_all, ec);
    fs::remove_all(root_, ec);
  }
  Scratch(const Scratch &) = delete;
  Scratch &operator=(const Scratch &) = delete;
  std::string path(const std::string &rel) const { return root_ + "/" + rel; }

 private:
  std::string root_;
};

void write_text(const std::string &path, const std::string &text) {
  fs::create_directories(fs::path(path).parent_path());
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << text;
}

std::string read_text(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int mode_of(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? static_cast<int>(st.st_mode & 0777) : -1;
}

// Sorted names in a directory, comma separated.
std::string names_in(const std::string &dir) {
  std::set<std::string> names;
  for (const auto &e : fs::directory_iterator(dir)) names.insert(e.path().filename().string());
  std::string out;
  for (const std::string &n : names) out += (out.empty() ? "" : ",") + n;
  return out;
}

bool contains(const std::string &hay, const std::string &needle) { return hay.find(needle) != std::string::npos; }

const std::string kBak = kBackupSuffix;
using Edits = std::map<std::string, Value>;

SaveResult save(const std::string &path, const Loaded &loaded, const Edits &edits, bool force = false) {
  Edits edited = loaded.values;
  for (const auto &[k, v] : edits) edited[k] = v;
  return save_changes(path, mpv_options(), edited, loaded, "", BoolStyle::YesNo, force);
}

Loaded load(const std::string &path) { return load_settings(path, mpv_options()); }

Value text(const char *s) { return Value(std::string(s)); }  // a bare "..." would pick the bool alternative

// ---- the 11 Python tests ---------------------------------------------------------------------------------------

void test_new_file_is_created_with_only_the_changed_key() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");  // the mpv directory does not exist yet
  const Loaded loaded = load(p);
  CHECK(!loaded.exists, "a missing file loads with exists == false");
  CHECK(loaded.error.empty(), "a missing file is not an error");
  CHECK(loaded.values.at("quality") == text("auto"), "the default is shown");
  const SaveResult r = save(p, loaded, {{"quality", text("high")}});
  CHECK(r.ok && r.wrote, "saved");
  CHECK_EQ(read_text(p), std::string("quality=high\n"));
  CHECK(r.backup.empty(), "no backup of a file that did not exist");
  CHECK_EQ(mode_of(p), 0644);
}

void test_unchanged_values_write_nothing() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  write_text(p, "# keep\n#quality=auto\n");
  const SaveResult r = save(p, load(p), {});
  CHECK(r.ok, "ok");
  CHECK(!r.wrote, "nothing written");
  CHECK(!fs::exists(p + kBak), "no backup for a save that changed nothing");
  CHECK_EQ(read_text(p), std::string("# keep\n#quality=auto\n"));
  // Setting a value to what it already is (also as 2 vs 2.0, true vs 1) is not a change either.
  write_text(p, "pool=2\nosd_opacity=0.5\nosd=yes\n");
  const SaveResult r2 = save(p, load(p), {{"pool", 2.0}, {"osd_opacity", 0.5}, {"osd", true}, {"osd_size", 11.0}});
  CHECK(r2.ok && !r2.wrote, "equal values in another numeric type write nothing");
}

void test_edit_keeps_comments_and_unknown_keys_and_backs_up_once() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  const std::string original = "# my notes\nfuture_option=7\n#quality=auto\nosd=no\n";
  write_text(p, original);
  SaveResult r = save(p, load(p), {{"quality", text("ultra")}, {"osd", true}});
  CHECK(r.ok && r.wrote, "saved");
  CHECK_EQ(read_text(p), std::string("# my notes\nfuture_option=7\nquality=ultra\n#osd=yes\n"));  // yes is the default
  CHECK_EQ(read_text(p + kBak), original);
  CHECK_EQ(r.backup, p + kBak);
  r = save(p, load(p), {{"quality", text("low")}});
  CHECK(r.ok && r.wrote && r.backup.empty(), "the second save makes no backup");
  CHECK_EQ(read_text(p + kBak), original);  // the user's original is never overwritten
}

void test_back_to_default_restores_the_documented_line() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  write_text(p, "quality=high\n");
  save(p, load(p), {{"quality", text("auto")}});
  CHECK_EQ(read_text(p), std::string("#quality=auto\n"));
}

void test_external_edit_of_other_keys_is_merged_not_overwritten() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  write_text(p, "quality=high\n");
  const Loaded loaded = load(p);
  write_text(p, "quality=high\nmy_new_key=1\n");  // edited elsewhere after we loaded
  const SaveResult r = save(p, loaded, {{"osd", false}});
  CHECK(r.ok && r.wrote, "saved");
  CHECK_EQ(read_text(p), std::string("quality=high\nmy_new_key=1\nosd=no\n"));
}

void test_external_edit_of_the_same_key_is_a_conflict() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  write_text(p, "quality=high\n");
  const Loaded loaded = load(p);
  write_text(p, "quality=low\n");
  SaveResult r = save(p, loaded, {{"quality", text("ultra")}});
  CHECK(!r.ok && !r.wrote, "not saved");
  CHECK(r.conflicts == std::vector<std::string>{"quality"}, "quality is the conflict");
  CHECK_EQ(read_text(p), std::string("quality=low\n"));  // untouched
  CHECK(!fs::exists(p + kBak), "no backup for a refused save");
  r = save(p, loaded, {{"quality", text("ultra")}}, /*force=*/true);
  CHECK(r.ok && r.wrote && r.conflicts.empty(), "force overwrites");
  CHECK_EQ(read_text(p), std::string("quality=ultra\n"));
  CHECK_EQ(read_text(p + kBak), std::string("quality=low\n"));  // the backup holds what was on disk, not the stale load
}

void test_symlinked_dotfile_stays_a_symlink() {
  Scratch t;
  const std::string real = t.path("dotfiles/vsr.conf");
  write_text(real, "#quality=auto\n");
  const std::string p = t.path("mpv/vsr.conf");
  fs::create_directories(t.path("mpv"));
  fs::create_symlink(real, p);
  save(p, load(p), {{"quality", text("high")}});
  CHECK(fs::is_symlink(p), "still a symlink");
  CHECK_EQ(read_text(real), std::string("quality=high\n"));
  CHECK_EQ(read_text(real + kBak), std::string("#quality=auto\n"));  // the backup sits next to the target
  CHECK_EQ(names_in(t.path("mpv")), std::string("vsr.conf"));
}

void test_file_mode_is_kept_and_no_temp_files_are_left() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  write_text(p, "#quality=auto\n");
  chmod(p.c_str(), 0600);
  save(p, load(p), {{"quality", text("low")}});
  CHECK_EQ(mode_of(p), 0600);
  CHECK_EQ(mode_of(p + kBak), 0600);
  CHECK_EQ(names_in(t.path("mpv")), std::string("vsr.conf,vsr.conf.bak-nvvsr"));
}

void test_invalid_file_value_loads_as_default_with_a_problem() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  write_text(p, "pool=99\nquality=nonsense\n");
  const Loaded loaded = load(p);
  CHECK(loaded.values.at("pool") == Value(1), "pool falls back to its default");
  CHECK(loaded.values.at("quality") == text("auto"), "quality falls back to its default");
  CHECK_EQ(loaded.problems.size(), size_t(2));
  CHECK(loaded.raw.at("pool") == std::optional<std::string>("99"), "the raw text is kept for the conflict check");
  CHECK(!loaded.raw.at("osd"), "an absent key has no raw text");
}

void test_unwritable_directory_reports_an_error_without_a_traceback() {
  const std::string p = "/proc/nvc-test/vsr.conf";  // mkdir below /proc always fails; nothing is created
  const Loaded loaded = load(p);
  const SaveResult r = save(p, loaded, {{"quality", text("high")}});
  CHECK(!r.ok && !r.wrote, "not saved");
  CHECK(contains(r.error, "could not write"), "error is '%s'", r.error.c_str());
  CHECK(r.conflicts.empty(), "an error is not a conflict");
  CHECK(!fs::exists("/proc/nvc-test"), "nothing was created");
}

// The Python VLCRC_SAMPLE.
const char *kVlcrc = R"(### vlc 3.0.24 configuration file ###

[core] # core program
# Fullscreen video output (boolean)
#fullscreen=0

# Audio volume
volume=256

[avcodec] # AVCodec audio/video decoder
# Direct rendering (boolean)
#avcodec-dr=1
avcodec-hw=any

[nvvsr] # NVIDIA VSR
# Enable NVIDIA super resolution (boolean)
#nvvsr-enable=1
# Quality (string)
#nvvsr-quality=auto
# Only on mains power (boolean)
#nvvsr-ac-only=1

[zzz-other] # something after
#zzz-key=1
)";

void test_vlcrc_edit_uses_documented_default_lines_and_01_booleans() {
  Scratch t;
  const std::string p = t.path("vlcrc");
  write_text(p, kVlcrc);
  const Loaded loaded = load_settings(p, vlc_options());
  Edits edited = loaded.values;
  edited["nvvsr-quality"] = text("ultra");
  edited["nvvsr-ac-only"] = false;
  edited["nvvsr-min-height"] = 480;
  const SaveResult r = save_changes(p, vlc_options(), edited, loaded, "nvvsr", BoolStyle::ZeroOne);
  CHECK(r.ok && r.wrote, "saved");
  const std::string out = read_text(p);
  CHECK(contains(out, "\nnvvsr-quality=ultra\n"), "quality line");
  CHECK(contains(out, "\nnvvsr-ac-only=0\n"), "0/1 boolean");
  CHECK(contains(out, "\n[nvvsr] # NVIDIA VSR\n"), "the section header is untouched");
  CHECK(contains(out, "nvvsr-min-height=480\n"), "appended key");
  CHECK_EQ(out.find("[nvvsr]"), out.rfind("[nvvsr]"));  // no second section
  CHECK(out.find("nvvsr-min-height=480") < out.find("[zzz-other]"), "appended inside [nvvsr], before the next section");
  CHECK(contains(out, "avcodec-hw=any"), "other sections are kept");
  CHECK_EQ(read_text(p + kBak), std::string(kVlcrc));
}

// ---- failure modes beyond the Python tests ------------------------------------------------------------------------

void test_read_only_directory_leaves_the_file_alone() {
  if (geteuid() == 0) return void(std::puts("SKIP read-only directory: root ignores directory modes"));
  Scratch t;
  const std::string p = t.path("ro/vsr.conf");
  const std::string original = "# mine\nquality=high\n";
  write_text(p, original);
  const Loaded loaded = load(p);
  chmod(t.path("ro").c_str(), 0555);
  const SaveResult r = save(p, loaded, {{"quality", text("low")}});
  CHECK(!r.ok && !r.wrote, "not saved");
  CHECK(contains(r.error, "could not write") && contains(r.error, "Permission denied"), "error is '%s'", r.error.c_str());
  CHECK_EQ(read_text(p), original);
  CHECK_EQ(names_in(t.path("ro")), std::string("vsr.conf"));  // no backup, no temp file
  chmod(t.path("ro").c_str(), 0755);
  CHECK(save(p, loaded, {{"quality", text("low")}}).ok, "saving works again once the directory is writable");
  CHECK_EQ(read_text(p), std::string("# mine\nquality=low\n"));
}

void test_a_write_that_fails_half_way_leaves_the_original_byte_identical() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  std::string original = "# notes \xff\xfe\r\n\r\nquality=high\r\n";  // CRLF and bytes that are not UTF-8 must survive
  original += std::string(500, '#') + "\r\n";
  write_text(p, original);
  const Loaded loaded = load(p);
  std::signal(SIGXFSZ, SIG_IGN);  // make write() fail with EFBIG after the temp file exists
  rlimit old{};
  getrlimit(RLIMIT_FSIZE, &old);
  rlimit lim = old;
  lim.rlim_cur = 8192;
  setrlimit(RLIMIT_FSIZE, &lim);
  const SaveResult r = save(p, loaded, {{"hwdec", Value(std::string(100000, 'x'))}});
  setrlimit(RLIMIT_FSIZE, &old);
  CHECK(!r.ok && !r.wrote, "not saved");
  CHECK(contains(r.error, "could not write"), "error is '%s'", r.error.c_str());
  CHECK_EQ(read_text(p), original);  // byte-identical
  CHECK_EQ(names_in(t.path("mpv")), std::string("vsr.conf,vsr.conf.bak-nvvsr"));  // the temp file was removed
  CHECK_EQ(read_text(p + kBak), original);
  // The same save without the limit works, and keeps the CRLF and the odd bytes.
  const SaveResult ok = save(p, load(p), {{"quality", text("low")}});
  CHECK(ok.ok && ok.wrote, "saved without the limit");
  CHECK_EQ(read_text(p), "# notes \xff\xfe\r\n\r\nquality=low\r\n" + std::string(500, '#') + "\r\n");
}

void test_vanished_file() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  write_text(p, "quality=high\nosd=no\n");
  const Loaded loaded = load(p);
  fs::remove(p);
  SaveResult r = save(p, loaded, {{"quality", text("ultra")}});
  CHECK(!r.ok && r.conflicts == std::vector<std::string>{"quality"}, "a key that was set and is gone is a conflict");
  CHECK(!fs::exists(p), "a refused save does not recreate the file");
  r = save(p, loaded, {{"pool", 3}});
  CHECK(r.ok && r.wrote, "a key that was not set can be saved");
  CHECK_EQ(read_text(p), std::string("pool=3\n"));  // only what the user changed, not the deleted lines
  CHECK(r.backup.empty(), "nothing to back up");
  fs::remove_all(t.path("mpv"));  // the directory vanished too
  r = save(p, loaded, {{"quality", text("ultra")}}, /*force=*/true);
  CHECK(r.ok && r.wrote, "force recreates the file and its directory");
  CHECK_EQ(read_text(p), std::string("quality=ultra\n"));
}

void test_symlinks() {
  Scratch t;
  // dangling link: the missing target is created, the link stays
  fs::create_directories(t.path("mpv"));
  fs::create_symlink(t.path("dot/new.conf"), t.path("mpv/a.conf"));
  Loaded loaded = load(t.path("mpv/a.conf"));
  CHECK(!loaded.exists && loaded.error.empty(), "a dangling link loads as a missing file");
  SaveResult r = save(t.path("mpv/a.conf"), loaded, {{"quality", text("high")}});
  CHECK(r.ok && r.wrote, "saved through a dangling link");
  CHECK(fs::is_symlink(t.path("mpv/a.conf")), "still a symlink");
  CHECK_EQ(read_text(t.path("dot/new.conf")), std::string("quality=high\n"));

  // relative link, then a chain of two links
  write_text(t.path("dot/real.conf"), "#quality=auto\n");
  fs::create_symlink("../dot/real.conf", t.path("mpv/b.conf"));
  fs::create_symlink("b.conf", t.path("mpv/c.conf"));
  loaded = load(t.path("mpv/c.conf"));
  CHECK(loaded.exists, "a chain of links is followed");
  r = save(t.path("mpv/c.conf"), loaded, {{"quality", text("low")}});
  CHECK(r.ok && r.wrote, "saved through two links");
  CHECK(fs::is_symlink(t.path("mpv/c.conf")) && fs::is_symlink(t.path("mpv/b.conf")), "both links are intact");
  CHECK_EQ(read_text(t.path("dot/real.conf")), std::string("quality=low\n"));
  CHECK_EQ(names_in(t.path("mpv")), std::string("a.conf,b.conf,c.conf"));  // nothing beside the links

  // a link loop is an error, not a hang, and nothing is replaced
  fs::create_symlink("y.conf", t.path("mpv/x.conf"));
  fs::create_symlink("x.conf", t.path("mpv/y.conf"));
  loaded = load(t.path("mpv/x.conf"));
  CHECK(!loaded.error.empty(), "a link loop cannot be read: '%s'", loaded.error.c_str());
  r = save(t.path("mpv/x.conf"), loaded, {{"quality", text("low")}});
  CHECK(!r.ok && contains(r.error, "could not read"), "error is '%s'", r.error.c_str());
  CHECK(fs::is_symlink(t.path("mpv/x.conf")) && fs::is_symlink(t.path("mpv/y.conf")), "the links are still links");
}

void test_empty_file() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  write_text(p, "");
  chmod(p.c_str(), 0640);
  Loaded loaded = load(p);
  CHECK(loaded.exists && loaded.error.empty(), "an empty file exists");
  CHECK(loaded.values.at("quality") == text("auto") && loaded.problems.empty(), "all defaults, no problems");
  SaveResult r = save(p, loaded, {});
  CHECK(r.ok && !r.wrote, "no change, no write");
  CHECK_EQ(read_text(p), std::string());
  r = save(p, loaded, {{"quality", text("high")}});
  CHECK(r.ok && r.wrote, "saved");
  CHECK_EQ(read_text(p), std::string("quality=high\n"));
  CHECK_EQ(read_text(p + kBak), std::string());  // an empty original is still backed up
  CHECK_EQ(mode_of(p), 0640);
}

void test_no_trailing_newline() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  write_text(p, "# c\nquality=high");
  SaveResult r = save(p, load(p), {{"quality", text("low")}});
  CHECK(r.ok && r.wrote, "saved");
  CHECK_EQ(read_text(p), std::string("# c\nquality=low"));  // still no final newline
  r = save(p, load(p), {{"osd", false}});
  CHECK_EQ(read_text(p), std::string("# c\nquality=low\nosd=no"));
  write_text(p, "quality=high");  // the same, as the only line
  r = save(p, load(p), {{"quality", text("auto")}});
  CHECK_EQ(read_text(p), std::string("#quality=auto"));
  write_text(p, "osd=no");
  r = save(p, load(p), {{"osd", true}});
  CHECK(r.ok, "ok");
  CHECK_EQ(read_text(p), std::string("#osd=yes"));
}

void test_very_long_lines() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  const std::string big(1 << 20, 'x');
  const std::string original = "# " + big + "\n#quality=auto\nunknown=" + big + "\nosd=no\n";
  write_text(p, original);
  Loaded loaded = load(p);
  CHECK(loaded.problems.empty(), "no problems");
  SaveResult r = save(p, loaded, {{"quality", text("high")}});
  CHECK(r.ok && r.wrote, "saved");
  CHECK_EQ(read_text(p), "# " + big + "\nquality=high\nunknown=" + big + "\nosd=no\n");
  CHECK_EQ(read_text(p + kBak), original);
  // a very long value of a free-text option survives a save and a load
  const std::string long_value(300000, 'v');
  r = save(p, load(p), {{"hwdec", Value(long_value)}});
  CHECK(r.ok && r.wrote, "saved a 300000 byte value");
  loaded = load(p);
  CHECK(std::get<std::string>(loaded.values.at("hwdec")) == long_value, "loaded back unchanged");
  r = save(p, loaded, {{"osd", true}});
  CHECK(r.ok && r.wrote, "and the next edit still works");
  CHECK(contains(read_text(p), "hwdec=" + long_value + "\n"), "the long value is untouched");
}

void test_unreadable_file_is_never_replaced() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  const std::string original = "quality=high\nprecious=1\n";
  write_text(p, original);
  const Loaded good = load(p);
  if (geteuid() != 0) {
    chmod(p.c_str(), 0000);
    const Loaded loaded = load(p);
    CHECK(loaded.exists && !loaded.error.empty(), "an unreadable file is reported, not taken for a missing one");
    const SaveResult r = save(p, good, {{"osd", false}});
    CHECK(!r.ok && !r.wrote && contains(r.error, "could not read"), "error is '%s'", r.error.c_str());
    chmod(p.c_str(), 0644);
    CHECK_EQ(read_text(p), original);
    CHECK(!fs::exists(p + kBak), "no backup was attempted");
  }
  // a directory where the file should be
  fs::create_directories(t.path("d/vsr.conf"));
  const Loaded loaded = load(t.path("d/vsr.conf"));
  CHECK(loaded.exists && !loaded.error.empty(), "a directory cannot be read as a file: '%s'", loaded.error.c_str());
  const SaveResult r = save(t.path("d/vsr.conf"), loaded, {{"osd", false}});
  CHECK(!r.ok && !r.wrote, "not saved");
  CHECK(fs::is_directory(t.path("d/vsr.conf")), "the directory is still a directory");
  // a path below a regular file
  write_text(t.path("plain"), "x");
  const Loaded below = load(t.path("plain/sub/vsr.conf"));
  CHECK(!below.exists && below.error.empty(), "a path below a regular file is a missing file");
  const SaveResult r2 = save(t.path("plain/sub/vsr.conf"), below, {{"osd", false}});
  CHECK(!r2.ok && contains(r2.error, "could not write"), "error is '%s'", r2.error.c_str());
  CHECK_EQ(read_text(t.path("plain")), std::string("x"));
}

void test_bad_values_are_refused_before_anything_is_touched() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  const std::string original = "quality=high\n";
  write_text(p, original);
  const Loaded loaded = load(p);
  const std::pair<const char *, Value> bad[] = {
      {"pool", 99},                          // outside 1..4
      {"osd_opacity", 0.0},                  // outside 0.1..1
      {"quality", text("ultraa")},           // not a choice
      {"min_height", text("lots")},          // not a number
      {"hwdec", text("a\nb")},               // would be two lines
      {"osd_opacity", text("")},             // a plain float cannot be empty
  };
  for (const auto &[key, value] : bad) {
    const SaveResult r = save(p, loaded, {{key, value}});
    CHECK(!r.ok && !r.wrote && r.conflicts.empty() && !r.error.empty(), "%s refused: '%s'", key, r.error.c_str());
  }
  CHECK_EQ(read_text(p), original);
  CHECK_EQ(names_in(t.path("mpv")), std::string("vsr.conf"));
  // one bad value among good ones writes none of them
  const SaveResult r = save(p, loaded, {{"osd", false}, {"pool", 99}});
  CHECK(!r.ok, "refused");
  CHECK_EQ(read_text(p), original);
  // keys that are not options are ignored
  const SaveResult r2 = save_changes(p, mpv_options(), {{"nonsense", text("1")}}, loaded);
  CHECK(r2.ok && !r2.wrote, "unknown edited keys are ignored");
}

void test_conflicts_come_back_in_option_order_and_other_keys_still_need_no_conflict() {
  Scratch t;
  const std::string p = t.path("mpv/vsr.conf");
  write_text(p, "quality=high\npool=2\nosd=no\n");
  const Loaded loaded = load(p);
  write_text(p, "osd=yes\npool=3\nquality=high\n");  // reordered; osd and pool changed
  SaveResult r = save(p, loaded, {{"osd", true}, {"pool", 4}, {"quality", text("low")}});
  CHECK(!r.ok && r.conflicts.size() == 2, "both conflicts are listed (%zu)", r.conflicts.size());
  CHECK(r.conflicts.size() == 2 && r.conflicts[0] == "osd" && r.conflicts[1] == "pool",
        "in option order (osd before pool, as in the mpv table), not in file order");
  CHECK_EQ(read_text(p), std::string("osd=yes\npool=3\nquality=high\n"));
  // a conflict on one key blocks the whole save: nothing partial
  r = save(p, loaded, {{"quality", text("low")}, {"pool", 4}});
  CHECK(!r.ok && !r.wrote, "refused");
  CHECK_EQ(read_text(p), std::string("osd=yes\npool=3\nquality=high\n"));
  // a key that someone deleted from the file counts as changed too
  write_text(p, "osd=no\nquality=high\n");
  r = save(p, loaded, {{"pool", 4}});
  CHECK(!r.ok && r.conflicts == std::vector<std::string>{"pool"}, "pool=2 was removed on disk");
}

void test_atomic_write_and_backup_once_directly() {
  Scratch t;
  const std::string p = t.path("a/b/c.conf");
  atomic_write(p, "one\n", 0640);  // creates a/b
  CHECK_EQ(read_text(p), std::string("one\n"));
  CHECK_EQ(mode_of(p), 0640);
  atomic_write(p, "two\n", 0600);
  CHECK_EQ(read_text(p), std::string("two\n"));
  CHECK_EQ(mode_of(p), 0600);
  CHECK_EQ(names_in(t.path("a/b")), std::string("c.conf"));
  atomic_write(p, std::string(), 0600);
  CHECK_EQ(read_text(p), std::string());

  CHECK_EQ(backup_once(p, "orig", 0600), p + kBak);
  CHECK_EQ(read_text(p + kBak), std::string("orig"));
  CHECK_EQ(backup_once(p, "newer", 0600), std::string());
  CHECK_EQ(read_text(p + kBak), std::string("orig"));

  bool threw = false;
  try {
    atomic_write(t.path("a/b/c.conf/inside"), "x", 0644);  // below a regular file
  } catch (const std::system_error &e) {
    threw = true;
    CHECK(e.code().value() != 0, "carries an errno");
  }
  CHECK(threw, "an impossible path throws std::system_error");
  CHECK_EQ(read_text(p), std::string());
}

}  // namespace

int main() {
  // Python SaveChanges (10) + VlcSave (1)
  test_new_file_is_created_with_only_the_changed_key();
  test_unchanged_values_write_nothing();
  test_edit_keeps_comments_and_unknown_keys_and_backs_up_once();
  test_back_to_default_restores_the_documented_line();
  test_external_edit_of_other_keys_is_merged_not_overwritten();
  test_external_edit_of_the_same_key_is_a_conflict();
  test_symlinked_dotfile_stays_a_symlink();
  test_file_mode_is_kept_and_no_temp_files_are_left();
  test_invalid_file_value_loads_as_default_with_a_problem();
  test_unwritable_directory_reports_an_error_without_a_traceback();
  test_vlcrc_edit_uses_documented_default_lines_and_01_booleans();
  // beyond the Python tests
  test_read_only_directory_leaves_the_file_alone();
  test_a_write_that_fails_half_way_leaves_the_original_byte_identical();
  test_vanished_file();
  test_symlinks();
  test_empty_file();
  test_no_trailing_newline();
  test_very_long_lines();
  test_unreadable_file_is_never_replaced();
  test_bad_values_are_refused_before_anything_is_touched();
  test_conflicts_come_back_in_option_order_and_other_keys_still_need_no_conflict();
  test_atomic_write_and_backup_once_directly();
  return finish("test_conf_io");
}
