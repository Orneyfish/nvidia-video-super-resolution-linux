#include <cstdlib>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "check.h"
#include "proc.h"

namespace {

using Argv = std::vector<std::string>;

bool only_alacritty(const std::string &n) { return n == "alacritty"; }
bool both_terminals(const std::string &) { return true; }
bool no_terminal(const std::string &) { return false; }

}  // namespace

int main() {
  // missing tool: reported, never thrown
  {
    CmdResult r = run({"nvc-no-such-tool"});
    CHECK(r.missing && !r.rc.has_value(), "missing tool is reported");
    CHECK_EQ(r.error_text(), std::string("nvc-no-such-tool not found"));
    CHECK(!r.ok(), "missing tool is not ok");
  }

  // timeout: killed and reported, quickly
  {
    const auto t0 = std::chrono::steady_clock::now();
    CmdResult r = run({"sleep", "5"}, 0.2);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.timed_out && !r.rc.has_value(), "timeout is reported");
    CHECK(secs < 3.0, "timed-out child is killed well before its own 5 s (took %.2f s)", secs);
    CHECK_EQ(r.error_text(), std::string("sleep timed out"));
  }

  // non-zero exit with stdout and stderr captured separately
  {
    CmdResult r = run({"sh", "-c", "echo out; echo oops >&2; exit 3"});
    CHECK_EQ(r.rc.value_or(-999), 3);
    CHECK_EQ(r.out, std::string("out\n"));
    CHECK_EQ(r.err, std::string("oops\n"));
    CHECK_EQ(r.error_text(), std::string("oops"));
    CHECK(!r.ok() && !r.missing && !r.timed_out, "non-zero exit is not ok, not missing, not timed out");
  }

  // no output: the status is the message
  {
    CmdResult r = run({"sh", "-c", "exit 4"});
    CHECK_EQ(r.error_text(), std::string("sh exited with status 4"));
  }

  // a signal death is a negative status
  {
    CmdResult r = run({"sh", "-c", "kill -9 $$"});
    CHECK_EQ(r.rc.value_or(-999), -9);
  }

  // success
  {
    CmdResult r = run({"sh", "-c", "echo hi"});
    CHECK(r.ok(), "exit 0 is ok");
    CHECK_EQ(r.out, std::string("hi\n"));
  }

  // no shell: metacharacters and expansions arrive verbatim, and nothing runs
  {
    const std::string arg = "a; echo pwned $(id) `id` > /dev/null $HOME '\"";
    CmdResult r = run({"printf", "%s", arg});
    CHECK_EQ(r.out, arg);
    CHECK(r.ok(), "printf with metacharacters succeeds");
  }

  // environment: the child sees the given env, and clean_env applies overrides on top
  {
    setenv("NVC_TEST_VAR", "from-parent", 1);
    const auto inherited = clean_env();
    bool seen = false;
    for (const auto &kv : inherited) seen = seen || kv == "NVC_TEST_VAR=from-parent";
    CHECK(seen, "clean_env keeps the parent environment");

    const auto env = clean_env({{"NVC_TEST_VAR", "override"}});
    CHECK_EQ(run({"sh", "-c", "printf %s \"$NVC_TEST_VAR\""}, 10.0, env).out, std::string("override"));
    CHECK_EQ(run({"sh", "-c", "printf %s \"$NVC_TEST_VAR\""}).out, std::string("from-parent"));

    unsetenv("NVC_TEST_VAR");
    CHECK_EQ(run({"sh", "-c", "printf %s \"$NVC_TEST_VAR\""}, 10.0, clean_env()).out, std::string(""));
  }

  // terminal choice: kitty first, alacritty only when it is the one installed, none gives nothing
  {
    auto t = terminal_argv({"sudo", "nvidia-smi", "-pl", "120"}, only_alacritty);
    CHECK(t && *t == (Argv{"alacritty", "--hold", "-e", "sudo", "nvidia-smi", "-pl", "120"}),
          "alacritty runs the command after --hold -e");
    t = terminal_argv({"id"}, both_terminals);
    CHECK(t && *t == (Argv{"kitty", "--hold", "-e", "id"}), "kitty is preferred when both are installed");
    CHECK(!terminal_argv({"id"}, no_terminal).has_value(), "no terminal gives nullopt");
    CHECK(!terminal_choice(no_terminal).has_value(), "no terminal choice");
  }

  // display_command: shlex.join quoting
  CHECK_EQ(display_command({"sudo", "nvidia-smi", "-pl", "120"}), std::string("sudo nvidia-smi -pl 120"));
  CHECK_EQ(display_command({"echo", "a b"}), std::string("echo 'a b'"));
  CHECK_EQ(display_command({"printf", "%s", "it's"}), std::string("printf %s 'it'\"'\"'s'"));
  CHECK_EQ(display_command({"x", ""}), std::string("x ''"));
  CHECK_EQ(display_command({"rm", "-rf", "$HOME;x"}), std::string("rm -rf '$HOME;x'"));

  return finish("test_proc");
}
