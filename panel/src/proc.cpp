#include "proc.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace {

double now_s() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

std::string trim(const std::string &s) {
  const char *ws = " \t\r\n\f\v";
  const size_t a = s.find_first_not_of(ws);
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

// Reads what is available on fd; false once the pipe is closed.
bool read_chunk(int fd, std::string &into) {
  char buf[4096];
  const ssize_t n = read(fd, buf, sizeof buf);
  if (n > 0) {
    into.append(buf, static_cast<size_t>(n));
    return true;
  }
  return n < 0 && (errno == EINTR || errno == EAGAIN);
}

// Closes fd (if open) and marks it closed.
void drop(int &fd) {
  if (fd >= 0) close(fd);
  fd = -1;
}

// Pointers into `v` for execvp/environ; the strings must outlive the pointers.
std::vector<char *> c_strings(std::vector<std::string> &v) {
  std::vector<char *> ptrs;
  for (auto &s : v) ptrs.push_back(s.data());
  ptrs.push_back(nullptr);
  return ptrs;
}

}  // namespace

std::string CmdResult::error_text() const {
  const std::string name = argv.empty() ? "" : argv.front();
  if (missing) return name + " not found";
  if (timed_out) return name + " timed out";
  const std::string text = trim(!err.empty() ? err : out);
  if (text.empty()) return name + " exited with status " + std::to_string(rc.value_or(-1));
  const size_t nl = text.rfind('\n');
  return nl == std::string::npos ? text : text.substr(nl + 1);
}

CmdResult run(const std::vector<std::string> &argv, double timeout_s, const std::vector<std::string> &env) {
  CmdResult r;
  r.argv = argv;
  if (argv.empty()) {
    r.missing = true;
    return r;
  }
  // Built before fork(): the child must not allocate.
  std::vector<std::string> args = argv, envs = env;
  std::vector<char *> cargv = c_strings(args), cenv = c_strings(envs);

  int pipes[3][2] = {{-1, -1}, {-1, -1}, {-1, -1}};  // stdout, stderr, exec-failure report
  auto close_all = [&] {
    for (auto &p : pipes)
      for (int &fd : p) drop(fd);
  };
  for (auto &p : pipes) {
    if (pipe2(p, O_CLOEXEC) != 0) {
      r.missing = true;
      r.err = std::strerror(errno);
      close_all();
      return r;
    }
  }

  const pid_t pid = fork();
  if (pid < 0) {
    r.missing = true;
    r.err = std::strerror(errno);
    close_all();
    return r;
  }
  if (pid == 0) {
    const int nul = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (nul >= 0) dup2(nul, 0);
    dup2(pipes[0][1], 1);
    dup2(pipes[1][1], 2);
    if (!env.empty()) environ = cenv.data();
    execvp(cargv[0], cargv.data());
    const int e = errno;
    if (write(pipes[2][1], &e, sizeof e) < 0) _exit(127);
    _exit(127);
  }

  drop(pipes[0][1]);
  drop(pipes[1][1]);
  drop(pipes[2][1]);
  int exec_errno = 0;
  ssize_t got;
  do {
    got = read(pipes[2][0], &exec_errno, sizeof exec_errno);
  } while (got < 0 && errno == EINTR);
  drop(pipes[2][0]);
  if (got == static_cast<ssize_t>(sizeof exec_errno)) {  // the child could not exec
    int status = 0;
    waitpid(pid, &status, 0);
    r.missing = true;
    r.err = std::strerror(exec_errno);
    close_all();
    return r;
  }

  std::string out, err;
  const double deadline = now_s() + timeout_s;
  bool reaped = false;
  int status = 0;
  while (true) {
    if (!reaped) {
      const pid_t w = waitpid(pid, &status, WNOHANG);
      if (w == pid || w < 0) reaped = true;
    }
    if (reaped && pipes[0][0] < 0 && pipes[1][0] < 0) break;
    const double left = deadline - now_s();
    if (left <= 0) {
      r.timed_out = true;
      break;
    }
    pollfd fds[2] = {{pipes[0][0], POLLIN, 0}, {pipes[1][0], POLLIN, 0}};
    poll(fds, 2, static_cast<int>(std::min(left * 1000.0, 50.0)) + 1);
    if (fds[0].revents && !read_chunk(pipes[0][0], out)) drop(pipes[0][0]);
    if (fds[1].revents && !read_chunk(pipes[1][0], err)) drop(pipes[1][0]);
  }
  r.out = out;
  r.err = err;
  if (r.timed_out) {
    if (!reaped) {  // a reaped pid may already belong to someone else
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
    }
    close_all();
    return r;
  }
  if (WIFEXITED(status)) r.rc = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) r.rc = -WTERMSIG(status);
  close_all();
  return r;
}

bool on_path(const std::string &name) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (name.find('/') != std::string::npos) return fs::is_regular_file(name, ec) && access(name.c_str(), X_OK) == 0;
  const char *path = getenv("PATH");
  if (!path) return false;
  const std::string p = path;
  size_t start = 0;
  while (true) {
    const size_t end = p.find(':', start);
    const std::string dir = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const fs::path cand = fs::path(dir.empty() ? "." : dir) / name;
    if (fs::is_regular_file(cand, ec) && access(cand.c_str(), X_OK) == 0) return true;
    if (end == std::string::npos) return false;
    start = end + 1;
  }
}

std::optional<std::vector<std::string>> terminal_choice(const Which &which) {
  static const std::vector<std::vector<std::string>> terminals = {{"kitty", "--hold", "-e"},
                                                                  {"alacritty", "--hold", "-e"}};
  for (const auto &term : terminals)
    if (which(term.front())) return term;
  return std::nullopt;
}

std::optional<std::vector<std::string>> terminal_argv(const std::vector<std::string> &command, const Which &which) {
  auto term = terminal_choice(which);
  if (!term) return std::nullopt;
  term->insert(term->end(), command.begin(), command.end());
  return term;
}

namespace {

std::string shell_quote(const std::string &s) {
  static const char *safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_@%+=:,./-";
  if (!s.empty() && s.find_first_not_of(safe) == std::string::npos) return s;
  std::string q = "'";
  for (char c : s) {
    if (c == '\'') q += "'\"'\"'";
    else q += c;
  }
  return q + "'";
}

}  // namespace

std::string display_command(const std::vector<std::string> &command) {
  std::string line;
  for (const auto &arg : command) {
    if (!line.empty()) line += ' ';
    line += shell_quote(arg);
  }
  return line;
}

std::vector<std::string> clean_env(const std::map<std::string, std::string> &extra) {
  std::map<std::string, std::string> env;
  for (char **e = environ; e && *e; ++e) {
    const std::string kv = *e;
    const size_t eq = kv.find('=');
    if (eq != std::string::npos) env[kv.substr(0, eq)] = kv.substr(eq + 1);
  }
  for (const auto &[k, v] : extra) env[k] = v;
  std::vector<std::string> out;
  for (const auto &[k, v] : env) out.push_back(k + "=" + v);
  return out;
}
