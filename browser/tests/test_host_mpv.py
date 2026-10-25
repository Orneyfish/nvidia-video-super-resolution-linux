"""Host + real private mpv, headless (--vo=null --ao=null --no-audio --hwdec=no), media from 127.0.0.1 only.

Environment (all optional):
  NVVSR_TEST_MPV   mpv binary (default ~/.local/opt/mpv-vsr/bin/mpv); the tests skip when it is missing
  NVVSR_TEST_CLIP  media file (default: generated with ffmpeg, 20 s 720p H.264); skipped if neither exists
  TMPDIR           where the temporary HOME/XDG directories go
The fake-mpv tests use small shell scripts instead of mpv to check the argument list, crash and exit paths.
"""
import fcntl
import glob
import json
import os
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time
import unittest

import rangeserver

HERE = os.path.dirname(os.path.realpath(__file__))
HOST = os.path.join(HERE, "..", "host", "nvvsr_host.py")
REPO_VSR_LUA = os.path.join(HERE, "..", "..", "mpv", "vsr.lua")
DEFAULT_MPV = os.path.join(os.path.expanduser("~"), ".local", "opt", "mpv-vsr", "bin", "mpv")
HEADLESS = "--vo=null --ao=null --no-audio --hwdec=no --ytdl=no"


def _frame(obj):
    body = json.dumps(obj).encode("utf-8")
    return struct.pack("<I", len(body)) + body


def _read_reply(proc, timeout):
    """Waits for the host to answer; returns the reply dict (raises if the host printed nothing)."""
    proc.stdin.close()
    out = proc.stdout.read()
    proc.wait(timeout)
    if len(out) < 4:
        raise AssertionError("no reply; stderr=%r" % proc.stderr.read()[-400:])
    (n,) = struct.unpack("<I", out[:4])
    return json.loads(out[4:4 + n].decode("ascii"))


def _pids_with(text):
    found = []
    for path in glob.glob("/proc/[0-9]*/cmdline"):
        try:
            with open(path, "rb") as f:
                cmd = f.read()
        except OSError:
            continue
        if text.encode() in cmd:
            found.append(int(path.split("/")[2]))
    return found


class Harness(unittest.TestCase):
    """Common setup: private XDG/HOME dirs, host launch, IPC client."""

    mpv = None
    clip = None
    base = None

    @classmethod
    def setUpClass(cls):
        cls.mpv = os.environ.get("NVVSR_TEST_MPV", DEFAULT_MPV)
        if not os.access(cls.mpv, os.X_OK):
            raise unittest.SkipTest("private mpv not found at %s" % cls.mpv)
        cls.root = tempfile.mkdtemp(prefix="nvvsr-mpvtest-", dir=os.environ.get("TMPDIR"))
        media = os.path.join(cls.root, "media")
        os.makedirs(media)
        clip = os.environ.get("NVVSR_TEST_CLIP")
        if clip:
            if not os.path.isfile(clip):
                raise unittest.SkipTest("NVVSR_TEST_CLIP missing")
            shutil.copy(clip, os.path.join(media, "clip.mp4"))
        else:
            if not shutil.which("ffmpeg"):
                raise unittest.SkipTest("no test clip: set NVVSR_TEST_CLIP or install ffmpeg")
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=20",
                            "-c:v", "libx264", "-pix_fmt", "yuv420p", "-preset", "ultrafast",
                            os.path.join(media, "clip.mp4")], check=True, timeout=120)
        cls.srv, cls.base = rangeserver.start(media)

    @classmethod
    def tearDownClass(cls):
        if getattr(cls, "srv", None):
            cls.srv.shutdown()
            cls.srv.server_close()
        if getattr(cls, "root", None):
            shutil.rmtree(cls.root, ignore_errors=True)

    def setUp(self):
        self.hosts = []
        self.dir = tempfile.mkdtemp(prefix="case-", dir=self.root)
        self.rt = os.path.join(self.dir, "run")
        os.makedirs(self.rt, mode=0o700)
        self.sockdir = os.path.join(self.rt, "nvvsr")  # the host's private directory; mpv runs with it as cwd
        self.env = {
            "HOME": self.dir, "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "XDG_STATE_HOME": os.path.join(self.dir, "state"), "XDG_CONFIG_HOME": os.path.join(self.dir, "cfg"),
            "XDG_RUNTIME_DIR": self.rt, "NVVSR_MPV": self.mpv, "NVVSR_VSR_LUA": REPO_VSR_LUA,
            "NVVSR_HOST_TEST": "1", "NVVSR_HOST_MPV_EXTRA": HEADLESS,
        }

    def tearDown(self):
        # no mpv may outlive a test: kill anything whose cwd is this test's socket directory
        for path in glob.glob("/proc/[0-9]*/cwd"):
            try:
                if os.readlink(path) == self.sockdir:
                    os.kill(int(path.split("/")[2]), signal.SIGKILL)
            except OSError:
                pass
        for proc in self.hosts:  # close our ends of the pipes and reap any host still running
            if proc.poll() is None:
                proc.kill()
            for f in (proc.stdin, proc.stdout, proc.stderr):
                f.close()
            proc.wait()
        shutil.rmtree(self.dir, ignore_errors=True)

    def url(self, query=""):
        return self.base + "/clip.mp4" + query

    def start_host(self, req, env=None):
        e = dict(self.env)
        if env:
            e.update(env)
        proc = subprocess.Popen([sys.executable, "-I", HOST], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=e)
        self.hosts.append(proc)
        proc.stdin.write(_frame(req))
        proc.stdin.flush()
        return proc

    def wait_socket(self, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                names = [n for n in os.listdir(self.sockdir) if n.startswith("mpv-") and n.endswith(".sock")]
            except FileNotFoundError:  # the host creates its directory when the request starts
                names = []
            if names:
                return names[0]
            time.sleep(0.05)
        raise AssertionError("mpv IPC socket never appeared")

    def ipc(self, name):
        dfd = os.open(self.sockdir, os.O_RDONLY | os.O_DIRECTORY)
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect("/proc/self/fd/%d/%s" % (dfd, name))
        finally:
            os.close(dfd)
        s.settimeout(0.5)
        s.sendall(b'{"command":["observe_property",1,"time-pos"]}\n')
        return s

    def time_pos_until(self, sock, target, timeout=40):
        """Reads IPC events until time-pos >= target; returns that time-pos."""
        buf, last, deadline = b"", None, time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    msg = json.loads(line)
                except ValueError:
                    continue
                if msg.get("event") == "property-change" and msg.get("name") == "time-pos" and \
                        isinstance(msg.get("data"), (int, float)):
                    last = float(msg["data"])
                    if last >= target:
                        return last
        raise AssertionError("time-pos never reached %s (last %s)" % (target, last))

    def assert_cleaned_up(self, sock_name):
        self.assertFalse(os.path.exists(os.path.join(self.sockdir, sock_name)), "socket left behind")
        self.assertEqual(_pids_with(sock_name), [], "mpv left running")
        with open(os.path.join(self.sockdir, "handoff.lock"), "a") as lk:
            fcntl.flock(lk, fcntl.LOCK_EX | fcntl.LOCK_NB)  # the lock must be free again
            fcntl.flock(lk, fcntl.LOCK_UN)


class RealMpv(Harness):
    def test_play_then_quit_over_ipc_reports_the_time(self):
        proc = self.start_host({"op": "handoff", "url": self.url("?token=SECRET123"), "currentTime": 5})
        name = self.wait_socket()
        dfd = os.open(self.sockdir, os.O_RDONLY | os.O_DIRECTORY)
        try:
            sock_mode = stat.S_IMODE(os.stat("/proc/self/fd/%d/%s" % (dfd, name)).st_mode)
            dir_mode = stat.S_IMODE(os.stat(self.sockdir).st_mode)
        finally:
            os.close(dfd)
        self.assertEqual(sock_mode, 0o600)
        self.assertEqual(dir_mode, 0o700)
        s = self.ipc(name)
        seen = self.time_pos_until(s, 8.0)           # about 3 s of playback from t=5
        s.sendall(b'{"command":["quit"]}\n')
        s.close()
        reply = _read_reply(proc, 60)
        self.assertTrue(reply["ok"], reply)
        self.assertFalse(reply["ended"])
        self.assertIsNone(reply["error"])
        self.assertAlmostEqual(reply["time"], seen, delta=0.6)
        self.assertGreaterEqual(reply["time"], 7.9)
        self.assertLess(reply["time"], 9.5)
        self.assert_cleaned_up(name)

    def test_playing_to_the_end_reports_ended(self):
        proc = self.start_host({"op": "handoff", "url": self.url(), "currentTime": 17.5})
        reply = _read_reply(proc, 90)
        self.assertTrue(reply["ok"], reply)
        self.assertTrue(reply["ended"])
        self.assertGreaterEqual(reply["time"], 19.0)
        self.assertIsNone(reply["error"])

    def test_mpv_killed_mid_playback_is_an_error_with_the_last_time(self):
        proc = self.start_host({"op": "handoff", "url": self.url(), "currentTime": 5})
        name = self.wait_socket()
        s = self.ipc(name)
        seen = self.time_pos_until(s, 6.5)
        pids = _pids_with(name)
        self.assertEqual(len(pids), 1)
        os.kill(pids[0], signal.SIGKILL)
        s.close()
        reply = _read_reply(proc, 60)
        self.assertFalse(reply["ok"])
        self.assertIn("crashed (signal 9)", reply["error"])
        self.assertIsNotNone(reply["time"])
        self.assertGreaterEqual(reply["time"], seen - 0.5)
        self.assert_cleaned_up(name)

    def test_second_concurrent_request_is_refused_and_launches_nothing(self):
        first = self.start_host({"op": "handoff", "url": self.url(), "currentTime": 5})
        name = self.wait_socket()
        s = self.ipc(name)
        self.time_pos_until(s, 6.0)
        second = self.start_host({"op": "handoff", "url": self.url(), "currentTime": 1})
        reply2 = _read_reply(second, 30)
        self.assertFalse(reply2["ok"])
        self.assertEqual(reply2["error"], "another handoff is already running")
        names = [n for n in os.listdir(self.sockdir) if n.startswith("mpv-") and n.endswith(".sock")]
        self.assertEqual(names, [name], "the refused request must not start a second mpv")
        s.sendall(b'{"command":["quit"]}\n')
        s.close()
        reply1 = _read_reply(first, 60)
        self.assertTrue(reply1["ok"], reply1)
        self.assert_cleaned_up(name)


class FakeMpv(Harness):
    """A shell stand-in for mpv: records argv, or crashes, or exits with a status."""

    def setUp(self):
        super().setUp()
        self.env.pop("NVVSR_HOST_TEST", None)  # the real-mpv override must not reach the argument checks
        self.env.pop("NVVSR_HOST_MPV_EXTRA", None)

    def make_fake(self, body):
        path = os.path.join(self.dir, "fake-mpv")
        with open(path, "w") as f:
            f.write("#!/bin/sh\n" + body + "\n")
        os.chmod(path, 0o755)
        return path

    def recorded_argv(self, env_extra=None, req=None):
        out = os.path.join(self.dir, "argv.bin")
        fake = self.make_fake('printf "%s\\0" "$@" > "$FAKE_OUT"\nexit 0')
        env = {"NVVSR_MPV": fake, "FAKE_OUT": out}
        env.update(env_extra or {})
        proc = self.start_host(req or {"op": "handoff", "url": "https://www.youtube.com/watch?v=abc&list=PL1",
                                       "currentTime": 12.5, "title": "Demo"}, env=env)
        reply = _read_reply(proc, 30)
        with open(out, "rb") as f:
            argv = f.read().split(b"\0")[:-1]
        return reply, [a.decode() for a in argv]

    def test_argument_list(self):
        reply, argv = self.recorded_argv()
        self.assertTrue(reply["ok"], reply)
        self.assertIn("--fullscreen", argv)
        self.assertIn("--start=12.500", argv)
        self.assertTrue(any(a.startswith("--input-ipc-server=mpv-") and a.endswith(".sock") for a in argv))
        self.assertIn("--script=" + REPO_VSR_LUA, argv)
        self.assertIn("--ytdl-format=bestvideo[height<=?1440]+bestaudio/best[height<=?1440]", argv)
        self.assertIn("--force-media-title=Demo", argv)
        self.assertIn("--no-resume-playback", argv)
        self.assertEqual(argv[-2:], ["--", "https://www.youtube.com/watch?v=abc&list=PL1"])
        self.assertNotIn("--ytdl-raw-options=cookies-from-browser=chrome", argv)

    def test_hostile_title_stays_inside_one_argument(self):
        reply, argv = self.recorded_argv(req={"op": "handoff", "url": "https://a.example/v.mp4",
                                              "currentTime": 0, "title": "--no-config --script=/evil"})
        self.assertTrue(reply["ok"], reply)
        self.assertIn("--force-media-title=--no-config --script=/evil", argv)
        self.assertNotIn("--no-config", argv)
        self.assertEqual(argv[-2:], ["--", "https://a.example/v.mp4"])

    def test_test_override_is_ignored_without_the_test_flag(self):
        reply, argv = self.recorded_argv(env_extra={"NVVSR_HOST_MPV_EXTRA": "--marker-extra"})
        self.assertTrue(reply["ok"], reply)
        self.assertIn("--fullscreen", argv)
        self.assertNotIn("--marker-extra", argv)

    def test_test_override_applies_with_the_test_flag(self):
        reply, argv = self.recorded_argv(env_extra={"NVVSR_HOST_TEST": "1", "NVVSR_HOST_MPV_EXTRA": "--marker-extra"})
        self.assertTrue(reply["ok"], reply)
        self.assertNotIn("--fullscreen", argv)
        self.assertIn("--marker-extra", argv)

    def test_panel_height_and_cookie_options_come_from_env_and_config(self):
        cfg_dir = os.path.join(self.dir, "cfg", "nvvsr")
        os.makedirs(cfg_dir)
        with open(os.path.join(cfg_dir, "handoff.json"), "w") as f:
            json.dump({"use_chrome_cookies": True, "panel_h": 1080}, f)
        _, argv = self.recorded_argv(env_extra={"XDG_CONFIG_HOME": os.path.join(self.dir, "cfg")})
        self.assertIn("--ytdl-format=bestvideo[height<=?1080]+bestaudio/best[height<=?1080]", argv)
        self.assertIn("--ytdl-raw-options=cookies-from-browser=chrome", argv)
        _, argv = self.recorded_argv(env_extra={"XDG_CONFIG_HOME": os.path.join(self.dir, "cfg"),
                                                "NVVSR_PANEL_H": "2160"})
        self.assertIn("--ytdl-format=bestvideo[height<=?2160]+bestaudio/best[height<=?2160]", argv)

    def test_cookies_browser_takes_browser_plus_keyring_and_rejects_anything_else(self):
        cfg_home = os.path.join(self.dir, "cfg")
        cfg_dir = os.path.join(cfg_home, "nvvsr")
        os.makedirs(cfg_dir)

        def argv_with(data, env=None):
            with open(os.path.join(cfg_dir, "handoff.json"), "w") as f:
                json.dump(data, f)
            return self.recorded_argv(env_extra=dict({"XDG_CONFIG_HOME": cfg_home}, **(env or {})))[1]

        want = "--ytdl-raw-options=cookies-from-browser=chrome+gnomekeyring"
        self.assertIn(want, argv_with({"use_chrome_cookies": True, "cookies_browser": "chrome+gnomekeyring"}))
        self.assertIn(want, argv_with({"use_chrome_cookies": True}, {"NVVSR_COOKIES_BROWSER": "chrome+gnomekeyring"}))
        for bad in ("chrome,format=best", "chrome+gnomekeyring:Profile 1", "--evil", "chrome keyring", "", 7,
                    "x" * 40, "chrome+kwallet+extra"):
            self.assertIn("--ytdl-raw-options=cookies-from-browser=chrome",
                          argv_with({"use_chrome_cookies": True, "cookies_browser": bad}), bad)
        # a bad value must never leak into the command line
        argv = argv_with({"use_chrome_cookies": True, "cookies_browser": "chrome,format=best"})
        self.assertFalse(any("chrome,format" in a for a in argv), argv)
        # without use_chrome_cookies the setting does nothing
        self.assertFalse(any("cookies-from-browser" in a
                             for a in argv_with({"cookies_browser": "chrome+gnomekeyring"})))

    def test_bad_panel_height_falls_back_to_default(self):
        _, argv = self.recorded_argv(env_extra={"NVVSR_PANEL_H": "99999"})
        self.assertIn("--ytdl-format=bestvideo[height<=?1440]+bestaudio/best[height<=?1440]", argv)

    def test_mpv_crash_at_start_is_an_error(self):
        fake = self.make_fake("kill -SEGV $$")
        proc = self.start_host({"op": "handoff", "url": "https://a.example/v.mp4", "currentTime": 3},
                               env={"NVVSR_MPV": fake})
        reply = _read_reply(proc, 30)
        self.assertFalse(reply["ok"])
        self.assertEqual(reply["error"], "mpv crashed (signal 11)")

    def test_mpv_nonzero_exit_is_an_error(self):
        fake = self.make_fake("exit 3")
        proc = self.start_host({"op": "handoff", "url": "https://a.example/v.mp4", "currentTime": 3},
                               env={"NVVSR_MPV": fake})
        reply = _read_reply(proc, 30)
        self.assertFalse(reply["ok"])
        self.assertEqual(reply["error"], "mpv exited with status 3")

    def test_mpv_not_found_is_an_error_and_nothing_runs(self):
        proc = self.start_host({"op": "handoff", "url": "https://a.example/v.mp4", "currentTime": 3},
                               env={"NVVSR_MPV": os.path.join(self.dir, "missing", "mpv")})
        reply = _read_reply(proc, 30)
        self.assertFalse(reply["ok"])
        self.assertEqual(reply["error"], "mpv not found")

    def test_missing_vsr_lua_is_an_error(self):
        proc = self.start_host({"op": "handoff", "url": "https://a.example/v.mp4", "currentTime": 3},
                               env={"NVVSR_VSR_LUA": os.path.join(self.dir, "nope.lua")})
        reply = _read_reply(proc, 30)
        self.assertFalse(reply["ok"])
        self.assertEqual(reply["error"], "vsr.lua not found")

    def test_test_override_cannot_smuggle_options_into_production(self):
        # NVVSR_HOST_MPV_EXTRA with a non-option token (and the test flag) is refused before anything starts
        fake = self.make_fake("exit 0")
        proc = self.start_host({"op": "handoff", "url": "https://a.example/v.mp4", "currentTime": 3},
                               env={"NVVSR_MPV": fake, "NVVSR_HOST_TEST": "1",
                                    "NVVSR_HOST_MPV_EXTRA": "--x -- evil"})
        reply = _read_reply(proc, 30)
        self.assertFalse(reply["ok"])
        self.assertEqual(reply["error"], "invalid NVVSR_HOST_MPV_EXTRA")


if __name__ == "__main__":
    unittest.main()
