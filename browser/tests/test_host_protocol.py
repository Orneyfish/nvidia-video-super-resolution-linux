"""Host protocol and validation tests. No mpv is started: subprocess.Popen is patched to fail if called."""
import importlib.util
import io
import json
import os
import shutil
import struct
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.realpath(__file__))
HOST_PATH = os.path.join(HERE, "..", "host", "nvvsr_host.py")
_spec = importlib.util.spec_from_file_location("nvvsr_host", HOST_PATH)
host = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(host)

GOOD = {"op": "handoff", "url": "https://www.youtube.com/watch?v=abc", "currentTime": 12.5}


def frame(body):
    """Length-prefixes a body (bytes, or a JSON-serialisable object)."""
    if not isinstance(body, bytes):
        body = json.dumps(body).encode("utf-8")
    return struct.pack("<I", len(body)) + body


def decode_reply(stdout_bytes):
    (n,) = struct.unpack("<I", stdout_bytes[:4])
    assert len(stdout_bytes) == 4 + n, "reply framing is wrong"
    return json.loads(stdout_bytes[4:].decode("ascii"))


class HostCase(unittest.TestCase):
    """Runs host.main() in-process with private XDG dirs; any attempt to launch a process fails the test."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="nvvsr-proto-", dir=os.environ.get("TMPDIR"))
        self.env = {
            "HOME": self.tmp,
            "XDG_STATE_HOME": os.path.join(self.tmp, "state"),
            "XDG_CONFIG_HOME": os.path.join(self.tmp, "cfg"),
            "XDG_RUNTIME_DIR": os.path.join(self.tmp, "run"),
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "NVVSR_MPV": os.path.join(self.tmp, "no-such-mpv"),
        }
        os.makedirs(self.env["XDG_RUNTIME_DIR"], mode=0o700)
        self.patch = mock.patch.object(host.subprocess, "Popen", side_effect=AssertionError("mpv must not start"))
        self.popen = self.patch.start()

    def tearDown(self):
        self.patch.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_host(self, raw_stdin, env=None):
        stdin, stdout = io.BytesIO(raw_stdin), io.BytesIO()
        code = host.main(stdin=stdin, stdout=stdout, env=env or self.env)
        self.assertEqual(code, 0)
        data = stdout.getvalue()
        return (decode_reply(data) if data else None), stdin

    def send(self, body, env=None):
        """One framed request; body is a JSON object or raw bytes."""
        return self.run_host(frame(body), env)

    def send_raw(self, raw, env=None):
        return self.run_host(raw, env)

    def assert_refused(self, body, expect=None):
        reply, _ = self.send(body)
        self.assertIsNotNone(reply, "no reply")
        self.assertFalse(reply["ok"])
        self.assertIsNone(reply["time"])
        self.assertIsInstance(reply["error"], str)
        if expect is not None:
            self.assertEqual(reply["error"], expect)
        self.popen.assert_not_called()
        return reply

    def assert_refused_raw(self, raw, expect):
        reply, _ = self.send_raw(raw)
        self.assertFalse(reply["ok"])
        self.assertEqual(reply["error"], expect)
        self.popen.assert_not_called()


class Framing(HostCase):
    def test_ping_replies_version_and_launches_nothing(self):
        reply, _ = self.send({"op": "ping"})
        self.assertEqual(reply, {"ok": True, "version": 1, "time": None, "ended": False, "error": None})
        self.popen.assert_not_called()

    def test_empty_stdin_is_a_clean_exit_without_reply(self):
        reply, _ = self.send_raw(b"")
        self.assertIsNone(reply)

    def test_truncated_header(self):
        self.assert_refused_raw(b"\x05\x00", "truncated frame header")

    def test_zero_length(self):
        self.assert_refused_raw(struct.pack("<I", 0), "empty message")

    def test_truncated_body(self):
        self.assert_refused_raw(struct.pack("<I", 10) + b"{}", "truncated message")

    def test_oversize_is_refused_without_reading_the_body(self):
        stdin = io.BytesIO(struct.pack("<I", host.MAX_MESSAGE + 1) + b"x" * 32)
        stdout = io.BytesIO()
        self.assertEqual(host.main(stdin=stdin, stdout=stdout, env=self.env), 0)
        self.assertEqual(decode_reply(stdout.getvalue())["error"], "message too large")
        self.assertEqual(stdin.tell(), 4, "the oversize body must not be read")
        self.popen.assert_not_called()

    def test_exactly_the_limit_is_accepted_by_the_framing(self):
        # 1 MiB of JSON whitespace around a valid ping: the framing accepts it.
        body = b'{"op":"ping"}' + b" " * (host.MAX_MESSAGE - len(b'{"op":"ping"}'))
        reply, _ = self.send(body)
        self.assertTrue(reply["ok"])
        self.assertEqual(reply["version"], 1)

    def test_one_byte_over_the_limit_is_refused(self):
        body = b'{"op":"ping"}' + b" " * (host.MAX_MESSAGE - len(b'{"op":"ping"}') + 1)
        self.assert_refused_raw(struct.pack("<I", len(body)) + body, "message too large")


class MalformedJson(HostCase):
    def test_malformed_json(self):
        self.assert_refused(b"{", "malformed JSON")
        self.assert_refused(b'{"op":"ping",}', "malformed JSON")

    def test_not_utf8(self):
        self.assert_refused(b"\xff\xfe\x00", "message is not UTF-8")

    def test_not_an_object(self):
        for raw in [b"[1,2]", b'"text"', b"null", b"42", b"true"]:
            with self.subTest(raw=raw):
                self.assert_refused(raw, "message must be a JSON object")

    def test_non_finite_numbers(self):
        self.assert_refused(b'{"op":"handoff","url":"https://a.example/","currentTime":NaN}', "non-finite number")
        self.assert_refused(b'{"op":"handoff","url":"https://a.example/","currentTime":Infinity}',
                            "non-finite number")
        self.assert_refused(b'{"op":"handoff","url":"https://a.example/","currentTime":-Infinity}',
                            "non-finite number")

    def test_deep_nesting_does_not_crash(self):
        self.assert_refused(b"[" * 200000, "malformed JSON")


class Schema(HostCase):
    def test_unknown_op(self):
        self.assert_refused({"op": "launch"}, "unknown op")
        self.assert_refused({"op": ["handoff"]}, "unknown op")
        self.assert_refused({}, "unknown op")

    def test_unexpected_field(self):
        self.assert_refused(dict(GOOD, extra=1), "unexpected field")
        self.assert_refused(dict(GOOD, mpv_args="--no-config"), "unexpected field")

    def test_missing_fields(self):
        self.assert_refused({"op": "handoff", "currentTime": 1}, "url and currentTime are required")
        self.assert_refused({"op": "handoff", "url": "https://a.example/"}, "url and currentTime are required")

    def test_ping_with_fields(self):
        self.assert_refused({"op": "ping", "url": "https://a.example/"}, "ping takes no fields")

    def test_wrong_types(self):
        self.assert_refused(dict(GOOD, url=123), "url must be a string")
        self.assert_refused(dict(GOOD, url=None), "url must be a string")
        self.assert_refused(dict(GOOD, url=["https://a.example/"]), "url must be a string")
        self.assert_refused(dict(GOOD, title=5), "title must be a string")


class Urls(HostCase):
    HOSTILE = [
        "file:///etc/passwd", "javascript:alert(1)", "data:text/html,<script>x</script>",
        "ftp://files.example/a.mp4", "--no-config", "-- https://example.com/", "--script=/tmp/x.lua",
        "https://user:pw@example.com/a", "https://example.com@evil.example/", "https://example.com/a b",
        "https://example.com/\n--script=/x", "https://exa\u0000mple.com/", "https://exämple.com/",
        "http://", "https:///path", "https://example.com:99999/", "https://example.com:notaport/",
        "http://[::1]/", "http://a.b\\@c.d/", "https://example.com/" + "a" * 2100, "", " https://example.com/",
        "mpv://example.com/", "https//example.com/", "http:/example.com/", "https://exa%mple.com/",
        "https://example.com/‮abc", "https://example.com/\x7f",
    ]

    def test_hostile_urls_are_refused_and_nothing_launches(self):
        for url in self.HOSTILE:
            with self.subTest(url=url[:40]):
                self.assert_refused(dict(GOOD, url=url))

    def test_scheme_is_normalised_and_valid_urls_pass(self):
        req = host.validate_request(dict(GOOD, url="HTTPS://Example.com/Path?x=1#frag"))
        self.assertEqual(req.url, "https://Example.com/Path?x=1#frag")
        req = host.validate_request(dict(GOOD, url="http://127.0.0.1:8080/v.mp4?token=a&b=c"))
        self.assertEqual(req.url, "http://127.0.0.1:8080/v.mp4?token=a&b=c")

    def test_sanitize_url_for_log_drops_credentials_and_secrets(self):
        s = host.sanitize_url("https://u:pw@h.example:8443/watch?v=abc&token=SECRET&list=L&t=5#frag")
        self.assertEqual(s, "https://h.example:8443/watch?v=abc&list=L&t=5")
        self.assertNotIn("SECRET", s)
        self.assertNotIn("pw", s)

    def test_redact_text_applies_to_urls_in_mpv_output(self):
        out = host.redact_text("Opening https://r.example/videoplayback?sig=SECRETSIG&v=1\x1b[0m")
        self.assertNotIn("SECRETSIG", out)
        self.assertNotIn("\x1b", out)


class Times(HostCase):
    def test_bad_times_are_refused(self):
        for bad in [-1, -0.001, 1e10, host.MAX_TIME + 1, "5", None, True, [1], {"t": 1}, 10 ** 400]:
            with self.subTest(t=str(bad)[:20]):
                self.assert_refused(dict(GOOD, currentTime=bad), "currentTime must be a number"
                                    if not isinstance(bad, (int, float)) or isinstance(bad, bool)
                                    else "currentTime out of range")

    def test_good_times_are_rounded(self):
        for t, want in [(0, 0.0), (12.3456, 12.346), (host.MAX_TIME, host.MAX_TIME), (7, 7.0)]:
            with self.subTest(t=t):
                self.assertEqual(host.validate_request(dict(GOOD, currentTime=t)).time, want)


class Titles(HostCase):
    def test_title_cleaning(self):
        self.assertEqual(host.clean_title("a\x00b\x1b[31mc\x07d"), "a b [31mc d")
        self.assertEqual(host.clean_title("x‮evil‬"), "x evil")
        self.assertEqual(host.clean_title("line1\nline2\r\n\tend"), "line1 line2 end")
        self.assertEqual(host.clean_title("\ud800lone"), "lone")
        self.assertEqual(len(host.clean_title("a" * 5000)), host.MAX_TITLE)
        self.assertEqual(host.clean_title(None), "")
        self.assertEqual(host.clean_title("--no-config"), "--no-config")  # text only; one argv token later

    def test_long_hostile_title_is_capped_not_refused(self):
        req = host.validate_request(dict(GOOD, title="x" * 100000))
        self.assertEqual(len(req.title), host.MAX_TITLE)


class Launch(HostCase):
    def test_missing_mpv_is_refused_before_any_process(self):
        env = dict(self.env)
        env.pop("XDG_RUNTIME_DIR")
        reply, _ = self.send(GOOD, env=env)
        self.assertFalse(reply["ok"])
        self.assertEqual(reply["error"], "mpv not found")  # NVVSR_MPV names a missing file
        self.popen.assert_not_called()

    def test_internal_error_still_answers(self):
        with mock.patch.object(host, "run_handoff", side_effect=RuntimeError("boom")):
            reply, _ = self.send(GOOD)
        self.assertFalse(reply["ok"])
        self.assertEqual(reply["error"], "internal error")

    def test_log_written_without_secrets(self):
        self.send(dict(GOOD, url="https://example.com/watch?v=abc&token=SECRETTOKEN"))
        log = os.path.join(self.env["XDG_STATE_HOME"], "nvvsr", "handoff.log")
        with open(log, encoding="utf-8") as f:
            text = f.read()
        self.assertIn("v=abc", text)
        self.assertNotIn("SECRETTOKEN", text)


if __name__ == "__main__":
    unittest.main()
