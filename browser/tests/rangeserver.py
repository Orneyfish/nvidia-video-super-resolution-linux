"""Local HTTP server with byte-range support for the headless mpv tests (stdlib only).

python3 -m http.server has no Range support, and mpv cannot seek in an mp4 served that way (every --start fails).
This serves one directory on 127.0.0.1 and answers "Range: bytes=a-b", "bytes=a-" and "bytes=-n" with 206.
"""
import http.server
import os
import re
import threading

_RANGE = re.compile(r"bytes=(\d*)-(\d*)$")


def make_handler(directory):
    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=directory, **kwargs)

        def log_message(self, fmt, *args):  # keep the test output quiet
            pass

        def send_head(self):
            rng = self.headers.get("Range")
            if not rng:
                return super().send_head()
            path = self.translate_path(self.path)
            if not os.path.isfile(path):
                self.send_error(404)
                return None
            size = os.path.getsize(path)
            m = _RANGE.match(rng.strip())
            if not m or (m.group(1) == "" and m.group(2) == ""):
                self.send_error(416)
                return None
            if m.group(1) == "":  # suffix range: last n bytes
                start = max(0, size - int(m.group(2)))
                end = size - 1
            else:
                start = int(m.group(1))
                end = int(m.group(2)) if m.group(2) else size - 1
                end = min(end, size - 1)
            if start > end or start >= size:
                self.send_error(416)
                return None
            f = open(path, "rb")
            f.seek(start)
            self._remaining = end - start + 1
            self.send_response(206)
            self.send_header("Content-Type", self.guess_type(path))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
            self.send_header("Content-Length", str(self._remaining))
            self.end_headers()
            return f

        def copyfile(self, source, outputfile):
            remaining = getattr(self, "_remaining", None)
            if remaining is None:
                return super().copyfile(source, outputfile)
            while remaining > 0:
                chunk = source.read(min(65536, remaining))
                if not chunk:
                    break
                outputfile.write(chunk)
                remaining -= len(chunk)
            self._remaining = None

    return Handler


class _Quiet(http.server.ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        pass  # mpv drops connections when it quits or seeks: not an error for the tests


def start(directory):
    """Starts the server in a daemon thread; returns (server, base_url). Call server.shutdown() then server_close()."""
    srv = _Quiet(("127.0.0.1", 0), make_handler(directory))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, "http://127.0.0.1:%d" % srv.server_address[1]
