"""Subprocess helpers. Nothing here raises: a missing tool or a timeout is reported in the result."""

import os
import shlex
import shutil
import subprocess
from dataclasses import dataclass

# Terminals the app may open for privileged commands (the user watches and types the sudo password there).
TERMINALS = (("kitty", "--hold", "-e"), ("alacritty", "--hold", "-e"))


@dataclass
class CmdResult:
    argv: list
    rc: "int | None" = None  # None: could not start or timed out
    out: str = ""
    err: str = ""
    missing: bool = False
    timed_out: bool = False

    @property
    def ok(self):
        return self.rc == 0

    @property
    def error_text(self):
        if self.missing:
            return f"{self.argv[0]} not found"
        if self.timed_out:
            return f"{self.argv[0]} timed out"
        text = (self.err or self.out).strip()
        return text.splitlines()[-1] if text else f"{self.argv[0]} exited with status {self.rc}"


def run(argv, timeout=10.0, env=None):
    """Run argv (no shell) and capture text output. Never raises."""
    argv = [str(a) for a in argv]
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, env=env, errors="replace",
                           stdin=subprocess.DEVNULL)
    except FileNotFoundError:
        return CmdResult(argv, missing=True)
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        return CmdResult(argv, timed_out=True, out=out)
    except OSError as e:
        return CmdResult(argv, err=str(e), missing=True)
    return CmdResult(argv, rc=p.returncode, out=p.stdout or "", err=p.stderr or "")


def find_terminal(which=shutil.which):
    for term in TERMINALS:
        if which(term[0]):
            return list(term)
    return None


def terminal_argv(command, which=shutil.which):
    """argv that opens a terminal running `command` (a list) and keeps it open afterwards, or None."""
    term = find_terminal(which)
    return term + [str(c) for c in command] if term else None


def display_command(command):
    """The exact command line shown to the user before they run it."""
    return shlex.join([str(c) for c in command])


def clean_env(extra=None):
    env = dict(os.environ)
    if extra:
        env.update(extra)
    return env
