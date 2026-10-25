"""IpcWatcher re-asserts fullscreen over mpv's socket (a tiling compositor can demote --fullscreen).

A tiny fake mpv socket plays back property-change events and records the commands it receives."""
import importlib.util
import json
import os
import shutil
import socket
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("nvvsr_host_fs", os.path.join(HERE, "..", "host", "nvvsr_host.py"))
host = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(host)


class FakeProc:
    def poll(self):
        return None


class FakeMpvSocket:
    def __init__(self, path):
        self.path = path
        self.commands = []
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(1)
        self.conn = None
        self.ready = threading.Event()
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        self.conn, _ = self.srv.accept()
        self.ready.set()
        buf = b""
        while True:
            try:
                data = self.conn.recv(65536)
            except OSError:
                return
            if not data:
                return
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    self.commands.append(json.loads(line)["command"])
                except (ValueError, KeyError):
                    pass

    def emit(self, prop_id, name, value):
        self.conn.sendall((json.dumps({"event": "property-change", "id": prop_id, "name": name, "data": value}) + "\n").encode())

    def fullscreen_requests(self):
        return [c for c in self.commands if c[:3] == ["set_property", "fullscreen", True]]

    def close(self):
        for s in (self.conn, self.srv):
            try:
                s.close()
            except Exception:
                pass


class FullscreenReassert(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="fs-")
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.sock = FakeMpvSocket(os.path.join(self.dir, "m.sock"))
        self.addCleanup(self.sock.close)

    def start(self, want):
        w = host.IpcWatcher(self.sock.path, FakeProc(), want_fullscreen=want)
        w.start()
        self.assertTrue(self.sock.ready.wait(5))
        time.sleep(0.2)
        return w

    def settle(self):
        time.sleep(0.3)

    def test_observes_the_properties_it_needs(self):
        self.start(True)
        self.settle()
        observed = {c[2] for c in self.sock.commands if c[0] == "observe_property"}
        self.assertEqual(observed, {"time-pos", "eof-reached", "fullscreen", "vo-configured"})

    def test_demoted_fullscreen_is_asked_for_again_and_not_more_than_the_limit(self):
        self.start(True)
        for _ in range(12):  # mpv keeps saying "not fullscreen"
            self.sock.emit(3, "fullscreen", False)
            time.sleep(0.45)  # longer than the 0.4 s spacing
        self.settle()
        self.assertEqual(len(self.sock.fullscreen_requests()), host.IPC_FS_ATTEMPTS)

    def test_requests_are_spaced(self):
        self.start(True)
        for _ in range(10):  # a burst of events inside the spacing interval
            self.sock.emit(3, "fullscreen", False)
        self.settle()
        self.assertEqual(len(self.sock.fullscreen_requests()), 1)

    def test_vo_configured_while_windowed_asks(self):
        self.start(True)
        self.sock.emit(4, "vo-configured", True)
        self.settle()
        self.assertEqual(len(self.sock.fullscreen_requests()), 1)

    def test_nothing_after_mpv_was_fullscreen_once(self):
        w = self.start(True)
        self.sock.emit(3, "fullscreen", True)
        self.settle()
        self.assertTrue(w.fs_seen)
        time.sleep(0.5)
        self.sock.emit(3, "fullscreen", False)  # the user left fullscreen: respected
        self.sock.emit(4, "vo-configured", True)
        self.settle()
        self.assertEqual(self.sock.fullscreen_requests(), [])

    def test_nothing_when_not_wanted(self):
        self.start(False)  # test mode / headless
        self.sock.emit(3, "fullscreen", False)
        self.sock.emit(4, "vo-configured", True)
        self.settle()
        self.assertEqual(self.sock.fullscreen_requests(), [])

    def test_nothing_after_the_window_closed(self):
        old = host.IPC_FS_WINDOW
        host.IPC_FS_WINDOW = 0.3
        self.addCleanup(setattr, host, "IPC_FS_WINDOW", old)
        self.start(True)
        time.sleep(0.6)
        self.sock.emit(3, "fullscreen", False)
        self.settle()
        self.assertEqual(self.sock.fullscreen_requests(), [])

    def test_time_tracking_still_works(self):
        w = self.start(True)
        self.sock.emit(1, "time-pos", 12.3456)
        self.settle()
        self.assertEqual(w.time, 12.346)


if __name__ == "__main__":
    unittest.main()
