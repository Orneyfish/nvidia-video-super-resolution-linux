// load_settings / save_changes / atomic_write: the part of the settings editor that touches the user's real mpv and
// VLC config files. Data loss is what this file is written against: nothing is written unless the current content
// was read successfully, every write goes through a temp file + rename, and the first modification keeps a backup.
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "conf.h"

namespace {

namespace fs = std::filesystem;

std::string errno_text(int e) { return std::error_code(e, std::generic_category()).message(); }

struct FileState {
  bool exists = false;
  std::string text;
  std::string error;  // exists but could not be read
  struct stat st {};  // valid when exists && error.empty()
};

// A missing file (ENOENT, or ENOTDIR below a regular file) is not an error; anything else that stops the read is.
FileState read_file(const std::string &path) {
  FileState f;
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);  // O_NONBLOCK: a FIFO must not hang us
  if (fd < 0) {
    if (errno == ENOENT || errno == ENOTDIR) return f;
    f.exists = true;
    f.error = errno_text(errno);
    return f;
  }
  f.exists = true;
  if (fstat(fd, &f.st) != 0)
    f.error = errno_text(errno);
  else if (!S_ISREG(f.st.st_mode))
    f.error = "not a regular file";
  else {
    char buf[65536];
    for (;;) {
      const ssize_t n = read(fd, buf, sizeof buf);
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) {
        f.error = errno_text(errno);
        break;
      }
      if (n == 0) break;
      f.text.append(buf, static_cast<size_t>(n));
    }
  }
  close(fd);
  if (!f.error.empty()) f.text.clear();
  return f;
}

// A symlinked dotfile (dotfile managers) stays a symlink: the file it points to is the one rewritten. A dangling
// link resolves to its missing target, which is then created. After 40 hops (a loop) open() reports ELOOP.
std::string resolve(std::string path) {
  for (int hops = 0; hops < 40; ++hops) {
    std::error_code ec;
    if (!fs::is_symlink(path, ec)) return path;
    const fs::path link = fs::read_symlink(path, ec);
    if (ec) return path;
    path = (link.is_absolute() ? link : fs::path(path).parent_path() / link).string();
  }
  return path;
}

// Python float() == semantics for the editor's values: 2 == 2.0 == true-ish, but "" never equals a number.
bool same_value(const Value &a, const Value &b) {
  const auto *sa = std::get_if<std::string>(&a);
  const auto *sb = std::get_if<std::string>(&b);
  if (sa || sb) return sa && sb && *sa == *sb;
  const auto num = [](const Value &v) {
    if (const bool *x = std::get_if<bool>(&v)) return *x ? 1.0 : 0.0;
    if (const int *x = std::get_if<int>(&v)) return static_cast<double>(*x);
    return std::get<double>(v);
  };
  return num(a) == num(b);
}

// True when the file is no longer the one we read (edited, replaced, removed, or created meanwhile).
bool changed_since(const std::string &path, const FileState &f) {
  struct stat st;
  const bool there = stat(path.c_str(), &st) == 0;
  if (!f.exists) return there;
  return !there || st.st_ino != f.st.st_ino || st.st_dev != f.st.st_dev || st.st_size != f.st.st_size ||
         st.st_mtim.tv_sec != f.st.st_mtim.tv_sec || st.st_mtim.tv_nsec != f.st.st_mtim.tv_nsec;
}

SaveResult failure(std::string message) {
  SaveResult r;
  r.error = std::move(message);
  return r;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------------------------------------------

Loaded load_settings(const std::string &path, const std::vector<Opt> &options) {
  Loaded out;
  out.path = path;
  const FileState f = read_file(path);
  out.exists = f.exists;
  out.error = f.error;
  const ConfFile conf(f.text);
  for (const Opt &opt : options) {
    const std::optional<std::string> r = conf.get(opt.key);
    out.raw[opt.key] = r;
    out.values[opt.key] = opt.def;
    if (!r) continue;
    try {
      out.values[opt.key] = parse_value(opt, *r);
    } catch (const std::invalid_argument &e) {
      out.problems.push_back(e.what());
    }
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// write
// ---------------------------------------------------------------------------------------------------------------

void atomic_write(const std::string &path, const std::string &text, mode_t mode) {
  const fs::path p(path);
  const fs::path dir = p.has_parent_path() ? p.parent_path() : fs::path(".");
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) throw std::system_error(ec);

  std::string tmp = (dir / ("." + p.filename().string() + ".XXXXXX")).string();
  const int fd = mkostemp(tmp.data(), O_CLOEXEC);
  if (fd < 0) throw std::system_error(errno, std::generic_category());

  int err = 0;
  for (size_t off = 0; off < text.size() && !err;) {
    const ssize_t n = write(fd, text.data() + off, text.size() - off);
    if (n > 0)
      off += static_cast<size_t>(n);
    else if (n == 0)
      err = ENOSPC;
    else if (errno != EINTR)
      err = errno;
  }
  if (!err && fsync(fd) != 0) err = errno;
  if (!err && fchmod(fd, mode) != 0) err = errno;
  if (close(fd) != 0 && !err) err = errno;
  if (!err && rename(tmp.c_str(), path.c_str()) != 0) err = errno;
  if (err) {
    unlink(tmp.c_str());
    throw std::system_error(err, std::generic_category());
  }
}

std::string backup_once(const std::string &path, const std::string &text, mode_t mode) {
  const std::string bak = path + kBackupSuffix;
  struct stat st;
  if (lstat(bak.c_str(), &st) == 0) return "";  // the first backup (the user's original) is never overwritten
  atomic_write(bak, text, mode);                // atomic, so a half-written backup can never block a later one
  return bak;
}

// ---------------------------------------------------------------------------------------------------------------
// save
// ---------------------------------------------------------------------------------------------------------------

SaveResult save_changes(const std::string &path, const std::vector<Opt> &options,
                        const std::map<std::string, Value> &edited, const Loaded &loaded, const std::string &section,
                        BoolStyle bool_style, bool force) {
  // The text to write for every key the user changed, in option order. Nothing touches the disk before this passes.
  std::vector<std::pair<const Opt *, std::string>> changes;
  for (const Opt &opt : options) {
    const auto e = edited.find(opt.key);
    if (e == edited.end()) continue;
    const auto l = loaded.values.find(opt.key);
    if (l != loaded.values.end() && same_value(e->second, l->second)) continue;
    try {
      std::string text = format_value(opt, e->second, bool_style);
      parse_value(opt, text);  // never write what the loader (and the player) would reject
      changes.emplace_back(&opt, std::move(text));
    } catch (const std::invalid_argument &x) {
      return failure(x.what());
    }
  }

  const std::string real = resolve(path);
  for (int attempt = 0; attempt < 3; ++attempt) {
    const FileState f = read_file(real);
    if (!f.error.empty()) return failure("could not read " + path + ": " + f.error);

    ConfFile conf(f.text);
    SaveResult res;
    for (const auto &[opt, text] : changes) {
      const auto r = loaded.raw.find(opt->key);
      if (conf.get(opt->key) != (r == loaded.raw.end() ? std::nullopt : r->second)) res.conflicts.push_back(opt->key);
    }
    if (!res.conflicts.empty() && !force) return res;
    res.conflicts.clear();

    try {
      for (const auto &[opt, text] : changes) {
        if (text == default_text(*opt, bool_style))
          conf.unset(opt->key, text);  // back to the default: the line becomes the documented `#key=default`
        else
          conf.set(opt->key, text, section);
      }
    } catch (const std::invalid_argument &x) {
      return failure(x.what());
    }

    const std::string out = conf.render();
    res.ok = true;
    if (out == f.text) return res;  // also: no file and nothing to put in it

    if (changed_since(real, f)) continue;  // changed while we were working: redo from the new content
    try {
      const mode_t mode = f.exists ? (f.st.st_mode & 0777) : 0644;
      if (f.exists) res.backup = backup_once(real, f.text, mode);
      atomic_write(real, out, mode);
    } catch (const std::system_error &e) {
      return failure("could not write " + path + ": " + e.code().message());
    }
    res.wrote = true;
    return res;
  }
  return failure("the file kept changing while saving; try again");
}
