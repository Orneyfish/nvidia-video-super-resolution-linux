// Subprocess helpers. Nothing here throws: a missing tool or a timeout is reported in the result.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct CmdResult {
  std::vector<std::string> argv;
  std::optional<int> rc;  // empty: could not start or timed out; -N: killed by signal N
  std::string out;
  std::string err;
  bool missing = false;
  bool timed_out = false;

  bool ok() const { return rc && *rc == 0; }
  std::string error_text() const;
};

// Runs argv without a shell. `env` replaces the child's environment; empty inherits it.
CmdResult run(const std::vector<std::string> &argv, double timeout_s = 10.0,
              const std::vector<std::string> &env = {});

using Which = std::function<bool(const std::string &)>;  // true if the tool is installed
bool on_path(const std::string &name);

// Terminals the app may open for privileged commands, in preference order.
std::optional<std::vector<std::string>> terminal_choice(const Which &which = on_path);
// argv that opens a terminal running `command` and keeps it open afterwards, or nullopt.
std::optional<std::vector<std::string>> terminal_argv(const std::vector<std::string> &command,
                                                      const Which &which = on_path);

// The command line shown to the user, shell-quoted like Python's shlex.join.
std::string display_command(const std::vector<std::string> &command);

// "KEY=VALUE" entries of the current environment with `extra` applied on top.
std::vector<std::string> clean_env(const std::map<std::string, std::string> &extra = {});
