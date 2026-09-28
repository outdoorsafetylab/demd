#!/usr/bin/env python3
"""A local HTTP origin for one DEM file, which counts what GDAL asks it for.

The point of an index is that startup stops opening every DEM. That is a
statement about I/O, and the only way to check it is to count the I/O: asking
whether the log stayed quiet cannot tell a lazy server apart from an eager one
that swallowed its errors.

So this serves a single file over `/vsicurl` and counts the requests. Range is
implemented because `http.server` does not, and GDAL will not read a remote
raster without it.
"""
import http.server
import os
import re
import threading
import time


class _Handler(http.server.BaseHTTPRequestHandler):
    # Quiet: the suite prints its own results, and GDAL makes a lot of requests.
    def log_message(self, fmt, *args):
        pass

    def _body(self):
        with open(self.server.dem_path, "rb") as f:
            return f.read()

    def _serve(self, with_body):
        if self.path != "/" + os.path.basename(self.server.dem_path):
            self.send_error(404)
            return
        with self.server.lock:
            self.server.waiting += 1
        self.server.gate.wait()
        with self.server.lock:
            self.server.waiting -= 1
        self.server.requests += 1
        data = self._body()
        total = len(data)
        start, end = 0, total - 1
        status = 200
        m = re.match(r"bytes=(\d*)-(\d*)$", self.headers.get("Range", "") or "")
        if m:
            status = 206
            if m.group(1):
                start = int(m.group(1))
                end = int(m.group(2)) if m.group(2) else total - 1
            else:
                # A suffix range: the last N bytes.
                start = max(0, total - int(m.group(2)))
            end = min(end, total - 1)
        chunk = data[start:end + 1]
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(len(chunk)))
        if status == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, total))
        self.end_headers()
        if with_body:
            self.wfile.write(chunk)

    def do_GET(self):
        self._serve(True)

    def do_HEAD(self):
        self._serve(False)


class Origin:
    """Serves `path` at http://127.0.0.1:<port>/<basename>, counting requests.

    hold() makes every request wait until release(), which is how a test gets
    a lookup that is still running, for exactly as long as it wants: timing a
    large request instead would make the result depend on the machine."""

    def __init__(self, path):
        self.path = path
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        self.server.daemon_threads = True
        self.server.dem_path = path
        self.server.requests = 0
        self.server.waiting = 0
        self.server.lock = threading.Lock()
        self.server.gate = threading.Event()
        self.server.gate.set()
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self):
        return "http://127.0.0.1:%d/%s" % (
            self.server.server_address[1], os.path.basename(self.path))

    @property
    def requests(self):
        return self.server.requests

    def reset(self):
        self.server.requests = 0

    def hold(self):
        self.server.gate.clear()

    def release(self):
        self.server.gate.set()

    def await_waiting(self, n, timeout=10):
        """True once `n` requests are held at the gate."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.server.lock:
                if self.server.waiting >= n:
                    return True
            time.sleep(0.02)
        return False

    def stop(self):
        self.release()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
