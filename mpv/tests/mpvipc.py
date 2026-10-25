"""Minimal mpv JSON IPC client for the integration tests (standard library only; run with python3 -I)."""
import json
import os
import socket
import subprocess
import time


class Mpv:
    """Starts mpv with an IPC socket and offers get/set/command/wait helpers."""

    def __init__(self, mpv, args, sock, env=None, log=None):
        # sun_path is limited to 107 bytes and the scratch paths are longer: bind and connect relative to the
        # socket's directory.
        sdir, sname = os.path.split(os.path.abspath(sock))
        self.log = open(log, "wb") if log else subprocess.DEVNULL
        if os.path.exists(sock):
            os.unlink(sock)
        self.proc = subprocess.Popen([mpv, "--input-ipc-server=" + sname] + list(args), env=env, cwd=sdir,
                                     stdin=subprocess.DEVNULL, stdout=self.log, stderr=subprocess.STDOUT)
        self.sock = None
        self.buf = b""
        self.events = []
        deadline = time.time() + 15
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("mpv exited early with %s" % self.proc.returncode)  # (nothing left to kill)
            here = os.getcwd()
            try:
                os.chdir(sdir)
                s = socket.socket(socket.AF_UNIX)
                s.connect(sname)
                self.sock = s
                break
            except OSError:
                time.sleep(0.05)
            finally:
                os.chdir(here)
        if self.sock is None:
            self.proc.kill()
            raise RuntimeError("cannot connect to mpv IPC socket")
        self.sock.settimeout(0.05)
        self.req = 0

    def _read_some(self):
        try:
            data = self.sock.recv(65536)
        except socket.timeout:
            return
        if not data:
            raise EOFError("mpv closed the IPC socket")
        self.buf += data

    def _next_message(self):
        while b"\n" not in self.buf:
            self._read_some()
            if b"\n" not in self.buf:
                return None
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line)

    def command(self, *args, timeout=15):
        self.req += 1
        rid = self.req
        self.sock.sendall(json.dumps({"command": list(args), "request_id": rid}).encode() + b"\n")
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = self._next_message()
            if msg is None:
                continue
            if msg.get("request_id") == rid:
                return msg
            if "event" in msg:
                self.events.append(msg)
        raise TimeoutError("no reply to %r" % (args,))

    def get(self, prop, default=None):
        r = self.command("get_property", prop)
        return r.get("data", default) if r.get("error") == "success" else default

    def set(self, prop, value):
        return self.command("set_property", prop, value).get("error")

    def alive(self):
        return self.proc.poll() is None

    def wait_for(self, pred, timeout=20, step=0.1):
        """Polls pred() until it returns something truthy; returns that or None on timeout."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not self.alive():
                return None
            try:
                v = pred()
            except (EOFError, TimeoutError, BrokenPipeError):
                return None
            if v:
                return v
            time.sleep(step)
        return None

    def vf_labels(self):
        return [f.get("label") for f in (self.get("vf") or [])]

    def close(self):
        try:
            if self.alive():
                self.command("quit", timeout=2)
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        if self.sock:
            self.sock.close()
        if self.log is not subprocess.DEVNULL:
            self.log.close()
