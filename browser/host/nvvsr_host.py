#!/usr/bin/env python3
"""NVVSR native messaging host: hands a web video over to the private mpv (RTX super resolution).

Chrome starts this process for every sendNativeMessage call. Framing: 4-byte little-endian length, then UTF-8 JSON,
on stdin; the same framing on stdout. Requests (strict schema, unknown fields rejected):

  {"op": "ping"}
  {"op": "handoff", "url": "https://...", "currentTime": 12.5, "title": "optional"}

Replies: {"ok": bool, "time": number or null, "ended": bool, "error": string or null}; ping: {"ok": true, "version": 1}.

Security: a request that fails validation launches nothing. Only one handoff runs at a time (flock on
$XDG_RUNTIME_DIR/nvvsr/handoff.lock). The mpv IPC socket lives in that 0700 directory and is 0600. mpv gets the URL
after "--" and no shell is involved. Diagnostics go to $XDG_STATE_HOME/nvvsr/handoff.log (rotating; URLs are reduced to
scheme, host, path and the allowlisted query parameters) and to stderr. Stdout carries the protocol only.

Environment: NVVSR_MPV (mpv binary), NVVSR_VSR_LUA (mpv/vsr.lua), NVVSR_LIBDIR (wheel libs for the VapourSynth
plugin), NVVSR_PANEL_H (panel height, default 1440). ~/.config/nvvsr/handoff.json (XDG_CONFIG_HOME) may set panel_h,
mpv, vsr_lua, libdir, use_chrome_cookies (default false) and cookies_browser (default chrome; e.g. chrome+gnomekeyring); the environment wins over the file.
Test-only (ignored unless NVVSR_HOST_TEST=1): NVVSR_HOST_MPV_EXTRA, extra mpv arguments that replace --fullscreen.
"""
import collections
import fcntl
import glob
import json
import logging
import logging.handlers
import math
import os
import re
import secrets
import shlex
import shutil
import socket
import stat
import struct
import subprocess
import sys
import threading
import time
import unicodedata
from urllib.parse import parse_qsl, urlencode, urlsplit

HOST_NAME = "com.nvvsr.mpv_handoff"
PROTOCOL_VERSION = 1
MAX_MESSAGE = 1 << 20          # request size limit (bytes of JSON)
MAX_URL = 2048
MAX_TITLE = 200                # code points, after control characters are removed
MAX_TIME = 7 * 86400.0         # seconds; anything larger is not a playback position
PANEL_DEFAULT = 1440
COOKIES_BROWSER_RE = re.compile(r"[a-z][a-z0-9]{1,15}(?:\+[a-z][a-z0-9]{1,15})?")
PANEL_RANGE = (144, 4320)
IPC_CONNECT_TIMEOUT = 10.0     # mpv creates its socket at start-up; wait this long at most
IPC_FS_ATTEMPTS = 4            # re-asking for fullscreen: at most this many times ...
IPC_FS_WINDOW = 8.0            # ... and only during the first seconds after the socket connected
MPV_EXIT_GRACE = 5.0           # after SIGTERM before SIGKILL, and for the IPC reader to finish
LOG_KEEP_PARAMS = frozenset({"v", "list", "t", "index", "start", "time_continue"})

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.realpath(__file__)), "..", ".."))
PRIVATE_MPV_REL = os.path.join(".local", "opt", "mpv-vsr", "bin", "mpv")

Request = collections.namedtuple("Request", "op url time title")


class Reject(Exception):
    """A bad request, refused before anything is launched. Its text is a fixed message, never request data."""


class HandoffError(Exception):
    """A failed handoff. `time` is the last known playback position, if any."""

    def __init__(self, message, time=None):
        super().__init__(message)
        self.time = time


LOG = logging.getLogger("nvvsr_host")


# ---------------------------------------------------------------------------------------------------------------
# Framing and schema
# ---------------------------------------------------------------------------------------------------------------

def _read_n(stream, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return bytes(buf)


def read_frame(stream):
    """Returns the message body, None on a clean EOF before any byte, raises Reject on bad framing."""
    head = _read_n(stream, 4)
    if not head:
        return None
    if len(head) < 4:
        raise Reject("truncated frame header")
    (size,) = struct.unpack("<I", head)
    if size == 0:
        raise Reject("empty message")
    if size > MAX_MESSAGE:
        raise Reject("message too large")  # the body is not read
    body = _read_n(stream, size)
    if len(body) != size:
        raise Reject("truncated message")
    return body


def write_frame(stream, obj):
    data = json.dumps(obj, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    stream.write(struct.pack("<I", len(data)) + data)
    stream.flush()


def make_reply(ok, time=None, ended=False, error=None):
    return {"ok": bool(ok), "time": time, "ended": bool(ended), "error": error}


def _reject_constant(name):
    raise Reject("non-finite number")


def parse_message(body):
    try:
        obj = json.loads(body.decode("utf-8"), parse_constant=_reject_constant)
    except UnicodeDecodeError:
        raise Reject("message is not UTF-8")
    except (ValueError, RecursionError):
        raise Reject("malformed JSON")
    if not isinstance(obj, dict):
        raise Reject("message must be a JSON object")
    return obj


# http(s) only, ASCII, no userinfo (the authority allows no "@"), no whitespace or control characters.
_URL_RE = re.compile(r"https?://[A-Za-z0-9.\-]+(?::[0-9]{1,5})?(?:[/?#][\x21-\x7e]*)?", re.IGNORECASE)


def validate_url(value):
    if not isinstance(value, str):
        raise Reject("url must be a string")
    if not 1 <= len(value) <= MAX_URL:
        raise Reject("url length out of range")
    if not _URL_RE.fullmatch(value):
        raise Reject("url must be a plain http or https URL")
    try:
        parts = urlsplit(value)
        parts.port  # noqa: B018 - raises ValueError for a bad port
    except ValueError:
        raise Reject("url has an invalid port")
    if parts.scheme not in ("http", "https") or not parts.hostname:
        raise Reject("url needs an http or https host")
    return parts.scheme + value[len(parts.scheme):]


def validate_time(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise Reject("currentTime must be a number")
    try:
        t = float(value)
    except OverflowError:
        raise Reject("currentTime out of range")
    if not math.isfinite(t) or t < 0 or t > MAX_TIME:
        raise Reject("currentTime out of range")
    return round(t, 3)


def clean_title(value):
    """Control and format characters (bidi overrides included) become spaces; whitespace is collapsed; length capped."""
    if value is None:
        return ""
    if not isinstance(value, str):
        raise Reject("title must be a string")
    value = value[:4096]
    out = []
    for ch in value:
        cat = unicodedata.category(ch)
        out.append(" " if cat[0] == "C" or cat in ("Zl", "Zp") else ch)
    return " ".join("".join(out).split())[:MAX_TITLE]


_HANDOFF_KEYS = frozenset({"op", "url", "currentTime", "title"})


def validate_request(obj):
    op = obj.get("op")
    if op == "ping":
        if set(obj) != {"op"}:
            raise Reject("ping takes no fields")
        return Request("ping", None, None, "")
    if op != "handoff":
        raise Reject("unknown op")
    if set(obj) - _HANDOFF_KEYS:
        raise Reject("unexpected field")
    if "url" not in obj or "currentTime" not in obj:
        raise Reject("url and currentTime are required")
    return Request("handoff", validate_url(obj["url"]), validate_time(obj["currentTime"]),
                   clean_title(obj.get("title")))


# ---------------------------------------------------------------------------------------------------------------
# Logging (URLs reduced; never stdout)
# ---------------------------------------------------------------------------------------------------------------

def sanitize_url(url):
    """scheme://host[:port]/path plus the allowlisted query parameters; no userinfo, no fragment, no other secrets."""
    try:
        parts = urlsplit(url)
        host = parts.hostname or "?"
        port = parts.port
    except ValueError:
        return "<unparseable url>"
    keep = [(k, v) for k, v in parse_qsl(parts.query, keep_blank_values=True) if k in LOG_KEEP_PARAMS]
    netloc = host + (":%d" % port if port else "")
    out = "%s://%s%s" % (parts.scheme or "?", netloc, parts.path)
    return out + ("?" + urlencode(keep) if keep else "")


_URL_IN_TEXT = re.compile(r"[a-z][a-z0-9+.\-]*://[^\s\"'<>]+", re.IGNORECASE)


def redact_text(text):
    text = _URL_IN_TEXT.sub(lambda m: sanitize_url(m.group(0)), text)
    return re.sub(r"[\x00-\x1f\x7f]", "?", text)


def _xdg(env, name, fallback):
    value = env.get(name, "")
    return value if os.path.isabs(value) else fallback


def _home(env):
    return env.get("HOME") or os.path.expanduser("~")


def setup_logging(env):
    for h in list(LOG.handlers):
        LOG.removeHandler(h)
        h.close()
    LOG.setLevel(logging.INFO)
    LOG.propagate = False
    state = os.path.join(_xdg(env, "XDG_STATE_HOME", os.path.join(_home(env), ".local", "state")), "nvvsr")
    handler = None
    try:
        os.makedirs(state, mode=0o700, exist_ok=True)
        path = os.path.join(state, "handoff.log")
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        os.close(fd)
        handler = logging.handlers.RotatingFileHandler(path, maxBytes=512 * 1024, backupCount=3, encoding="utf-8")
    except OSError:
        handler = logging.StreamHandler(sys.stderr)
    handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
    LOG.addHandler(handler)
    return state


# ---------------------------------------------------------------------------------------------------------------
# Configuration and binaries
# ---------------------------------------------------------------------------------------------------------------

def _as_int(value):
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.strip().isdigit():
        return int(value.strip())
    return None


def default_libdir():
    for sp in sorted(glob.glob(os.path.join(REPO_ROOT, ".venv", "lib", "python*", "site-packages"))):
        dirs = [os.path.join(sp, "nvvfx", "libs"), os.path.join(sp, "nvidia_vfx.libs")]
        if all(os.path.isdir(d) for d in dirs):
            return ":".join(dirs)
    return None


def load_config(env):
    """Returns (cfg, warnings). Never fails: a broken file is ignored with a warning."""
    warns = []
    path = os.path.join(_xdg(env, "XDG_CONFIG_HOME", os.path.join(_home(env), ".config")), "nvvsr", "handoff.json")
    data = {}
    try:
        with open(path, "rb") as f:
            raw = f.read(65536)
        loaded = json.loads(raw.decode("utf-8"))
        if isinstance(loaded, dict):
            data = loaded
        else:
            warns.append("handoff.json is not an object, ignored")
    except FileNotFoundError:
        pass
    except (OSError, ValueError, RecursionError):
        warns.append("handoff.json unreadable or invalid, ignored")

    panel = PANEL_DEFAULT
    raw_panel = env.get("NVVSR_PANEL_H", data.get("panel_h"))
    if raw_panel is not None:
        n = _as_int(raw_panel)
        if n is not None and PANEL_RANGE[0] <= n <= PANEL_RANGE[1]:
            panel = n
        else:
            warns.append("panel_h out of range or not an integer, using %d" % PANEL_DEFAULT)

    def text(env_name, key):
        v = env.get(env_name) or data.get(key)
        return v if isinstance(v, str) and v else None

    # yt-dlp's --cookies-from-browser value: browser[+keyring], e.g. "chrome" or "chrome+gnomekeyring" (Chrome run with
    # --password-store=gnome-libsecret stores its cookie key in the GNOME keyring, which plain "chrome" does not find).
    # It ends up inside one mpv option list, so only a strict, comma-free shape is accepted.
    cookies_browser = "chrome"
    raw_cb = env.get("NVVSR_COOKIES_BROWSER") or data.get("cookies_browser")
    if raw_cb is not None:
        if isinstance(raw_cb, str) and COOKIES_BROWSER_RE.fullmatch(raw_cb):
            cookies_browser = raw_cb
        else:
            warns.append("cookies_browser is not of the form browser or browser+keyring, using chrome")

    cfg = {
        "panel_h": panel,
        "use_chrome_cookies": data.get("use_chrome_cookies") is True,
        "cookies_browser": cookies_browser,
        "vsr_lua": text("NVVSR_VSR_LUA", "vsr_lua") or os.path.join(REPO_ROOT, "mpv", "vsr.lua"),
        "mpv": text("NVVSR_MPV", "mpv"),
        "libdir": text("NVVSR_LIBDIR", "libdir") or default_libdir(),
        "private_mpv": os.path.join(_home(env), PRIVATE_MPV_REL),
    }
    return cfg, warns


def resolve_mpv(cfg):
    if cfg["mpv"]:  # explicit: never silently replaced by another binary
        if os.path.isfile(cfg["mpv"]) and os.access(cfg["mpv"], os.X_OK):
            return cfg["mpv"]
        raise HandoffError("mpv not found")
    cand = cfg["private_mpv"]
    if os.path.isfile(cand) and os.access(cand, os.X_OK):
        return cand
    found = shutil.which("mpv-vsr")
    if found:
        return found
    raise HandoffError("mpv not found (the private mpv-vsr build is required)")


def build_mpv_argv(mpv, req, sock, cfg, test_extra=None):
    h = cfg["panel_h"]
    argv = [mpv]
    if test_extra is None:
        argv.append("--fullscreen")
    argv += [
        "--start=%.3f" % req.time,
        "--input-ipc-server=" + sock,
        "--script=" + cfg["vsr_lua"],
        "--no-resume-playback",  # the page's time wins over mpv's own saved position
        "--ytdl-format=bestvideo[height<=?%d]+bestaudio/best[height<=?%d]" % (h, h),
    ]
    if req.title:
        argv.append("--force-media-title=" + req.title)
    if cfg["use_chrome_cookies"]:
        argv.append("--ytdl-raw-options=cookies-from-browser=" + cfg["cookies_browser"])
    if test_extra:
        argv += test_extra
    argv += ["--", req.url]  # the URL is validated http(s) and comes after "--": never read as an option
    return argv


def test_extra_args(env):
    """Test-only: NVVSR_HOST_MPV_EXTRA is honoured only with NVVSR_HOST_TEST=1."""
    if env.get("NVVSR_HOST_TEST") != "1" or not env.get("NVVSR_HOST_MPV_EXTRA"):
        return None
    tokens = shlex.split(env["NVVSR_HOST_MPV_EXTRA"])
    if any(not t.startswith("--") or t == "--" for t in tokens):
        raise HandoffError("invalid NVVSR_HOST_MPV_EXTRA")
    return tokens


# ---------------------------------------------------------------------------------------------------------------
# mpv IPC: observes time-pos and end-file; the socket is the only channel to the running mpv
# ---------------------------------------------------------------------------------------------------------------

class IpcWatcher(threading.Thread):
    def __init__(self, path, proc, want_fullscreen=False):
        super().__init__(name="mpv-ipc", daemon=True)
        self.path = path
        self.proc = proc
        self.want_fullscreen = want_fullscreen
        self.sock = None
        self.fs_seen = False       # mpv has been fullscreen at least once: from then on the user decides
        self.fs_tries = 0
        self.fs_last = 0.0
        self.started = time.monotonic()
        self.time = None
        self.end_reason = None
        self.eof_reached = False

    def run(self):
        try:
            self._run()
        except Exception as e:  # the IPC is best effort; the exit status still decides the reply
            LOG.info("ipc watcher stopped: %s", type(e).__name__)

    def _run(self):
        deadline = time.monotonic() + IPC_CONNECT_TIMEOUT
        sock = None
        while time.monotonic() < deadline and self.proc.poll() is None:
            if os.path.exists(self.path):
                try:
                    os.chmod(self.path, 0o600)
                except OSError:
                    pass
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    s.connect(self.path)
                    sock = s
                    break
                except OSError:
                    s.close()
            time.sleep(0.05)
        if sock is None:
            return
        self.sock = sock
        self.started = time.monotonic()
        try:
            sock.sendall(b'{"command":["observe_property",1,"time-pos"]}\n'
                         b'{"command":["observe_property",2,"eof-reached"]}\n'
                         b'{"command":["observe_property",3,"fullscreen"]}\n'
                         b'{"command":["observe_property",4,"vo-configured"]}\n')
            reader = sock.makefile("rb")
            while True:
                line = reader.readline(65536)
                if not line:
                    break
                self._handle(line)
        finally:
            sock.close()

    def _ask_fullscreen(self):
        """A tiling compositor (Hyprland with on_focus_under_fullscreen) can demote mpv's --fullscreen while the
        browser window is still leaving fullscreen. Ask again over the socket: a few times, spaced, only in the first
        seconds, and never after mpv has been fullscreen once (a later change is the user's)."""
        if not self.want_fullscreen or self.fs_seen or self.fs_tries >= IPC_FS_ATTEMPTS or self.sock is None:
            return
        now = time.monotonic()
        if now - self.started > IPC_FS_WINDOW or now - self.fs_last < 0.4:
            return
        self.fs_last = now
        self.fs_tries += 1
        try:
            self.sock.sendall(b'{"command":["set_property","fullscreen",true]}\n')
        except OSError:
            pass

    def _handle(self, line):
        try:
            msg = json.loads(line.decode("utf-8", "replace"))
        except ValueError:
            return
        if not isinstance(msg, dict):
            return
        if msg.get("event") == "property-change":
            v = msg.get("data")
            if msg.get("id") == 1 and isinstance(v, (int, float)) and not isinstance(v, bool):
                if math.isfinite(v) and 0 <= v <= MAX_TIME:
                    self.time = round(float(v), 3)
            elif msg.get("id") == 2 and v is True:
                self.eof_reached = True
            elif msg.get("id") == 3:
                if v is True:
                    self.fs_seen = True
                elif v is False:
                    self._ask_fullscreen()
            elif msg.get("id") == 4 and v is True:
                self._ask_fullscreen()
        elif msg.get("event") == "end-file":
            self.end_reason = str(msg.get("reason", ""))[:16]


# ---------------------------------------------------------------------------------------------------------------
# The handoff
# ---------------------------------------------------------------------------------------------------------------

def runtime_dir(env):
    base = env.get("XDG_RUNTIME_DIR", "")
    if not os.path.isabs(base):
        raise HandoffError("XDG_RUNTIME_DIR is not set")
    d = os.path.join(base, "nvvsr")
    try:
        os.mkdir(d, 0o700)
    except FileExistsError:
        pass
    st = os.lstat(d)
    if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.getuid():
        raise HandoffError("runtime directory is not private")
    if st.st_mode & 0o077:
        os.chmod(d, 0o700)
    return d


def acquire_lock(d):
    fd = os.open(os.path.join(d, "handoff.lock"), os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0), 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(fd)
        raise HandoffError("another handoff is already running")
    except OSError:
        os.close(fd)
        raise HandoffError("cannot take the handoff lock")
    return fd


def _remove(path):
    try:
        os.unlink(path)
    except OSError:
        pass


def _terminate(proc):
    proc.terminate()
    try:
        proc.wait(MPV_EXIT_GRACE)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def _log_mpv_output(path):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - 65536))
            lines = f.read().decode("utf-8", "replace").splitlines()[-40:]
        for ln in lines:
            LOG.info("mpv: %s", redact_text(ln)[:300])
    except OSError:
        pass
    _remove(path)


def run_handoff(req, cfg, env, test_extra):
    mpv = resolve_mpv(cfg)
    if not os.path.isfile(cfg["vsr_lua"]):
        raise HandoffError("vsr.lua not found")
    rdir = runtime_dir(env)
    lock_fd = acquire_lock(rdir)
    try:
        return _run_locked(req, cfg, env, mpv, rdir, test_extra)
    finally:
        os.close(lock_fd)


def _run_locked(req, cfg, env, mpv, rdir, test_extra):
    # The socket is named relative to the runtime directory (mpv runs with it as cwd) and reached through
    # /proc/self/fd/<dirfd>/: AF_UNIX paths are limited to 108 bytes, and XDG_RUNTIME_DIR may be long.
    stem = "mpv-%d-%s" % (os.getpid(), secrets.token_hex(6))
    sock_name = stem + ".sock"
    out_path = os.path.join(rdir, stem + ".out")
    argv = build_mpv_argv(mpv, req, sock_name, cfg, test_extra)
    env_mpv = dict(env)
    if cfg["libdir"]:
        env_mpv["NVVSR_LIBDIR"] = cfg["libdir"]

    dfd = os.open(rdir, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        sock_path = "/proc/self/fd/%d/%s" % (dfd, sock_name)
        out_fd = os.open(out_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | getattr(os, "O_NOFOLLOW", 0), 0o600)
        old_umask = os.umask(0o077)  # the socket is created 0600
        try:
            out = os.fdopen(out_fd, "wb")
            try:
                proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
                                        env=env_mpv, close_fds=True, cwd=rdir)
            except OSError:
                _remove(out_path)
                raise HandoffError("cannot start mpv")
            finally:
                out.close()
        finally:
            os.umask(old_umask)

        LOG.info("mpv started pid=%d start=%.3f url=%s", proc.pid, req.time, sanitize_url(req.url))
        watch = IpcWatcher(sock_path, proc, want_fullscreen=test_extra is None)
        watch.start()
        try:
            rc = proc.wait()  # no timeout: playback lasts as long as the user watches
        finally:
            if proc.poll() is None:
                _terminate(proc)
            watch.join(MPV_EXIT_GRACE)
            try:
                os.unlink(sock_name, dir_fd=dfd)
            except OSError:
                pass
            _log_mpv_output(out_path)
    finally:
        os.close(dfd)

    LOG.info("mpv exited status=%s end=%s time=%s", rc, watch.end_reason, watch.time)
    last = watch.time
    if watch.end_reason == "error":
        raise HandoffError("mpv could not play the video", time=last)
    if rc < 0:
        raise HandoffError("mpv crashed (signal %d)" % -rc, time=last)
    if rc != 0:
        raise HandoffError("mpv exited with status %d" % rc, time=last)
    ended = watch.end_reason == "eof" or watch.eof_reached
    return make_reply(True, time=last if last is not None else req.time, ended=ended)


def main(stdin=None, stdout=None, env=None):
    env = dict(os.environ) if env is None else dict(env)
    stdin = stdin if stdin is not None else sys.stdin.buffer
    stdout = stdout if stdout is not None else sys.stdout.buffer
    setup_logging(env)
    cfg, warns = load_config(env)
    for w in warns:
        LOG.warning(w)
    try:
        body = read_frame(stdin)
        if body is None:
            return 0
        req = validate_request(parse_message(body))
    except Reject as e:
        LOG.warning("rejected request: %s", e)
        write_frame(stdout, make_reply(False, error=str(e)))
        return 0

    if req.op == "ping":
        write_frame(stdout, {"ok": True, "version": PROTOCOL_VERSION, "time": None, "ended": False, "error": None})
        return 0

    LOG.info("handoff request url=%s t=%.3f title_len=%d", sanitize_url(req.url), req.time, len(req.title))
    try:
        reply = run_handoff(req, cfg, env, test_extra_args(env))
    except HandoffError as e:
        LOG.warning("handoff failed: %s", e)
        reply = make_reply(False, time=e.time, error=str(e))
    except Exception as e:  # never leave Chrome waiting without an answer
        LOG.error("internal error: %s", type(e).__name__)
        reply = make_reply(False, error="internal error")
    write_frame(stdout, reply)
    return 0


if __name__ == "__main__":
    sys.exit(main())
