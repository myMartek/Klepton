#!/usr/bin/env python3
# A web server for tests/t_bsremote.c: serves tests/data with byte ranges
# (what pCloud's download links answer). Under /flaky/ every file is refused
# once (403, as an expired link is) before it is served. Prints the port.
import http.server, os, re, socketserver, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
refused = set()

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        flaky = path.startswith("/flaky/")
        name = os.path.basename(path)
        full = os.path.join(ROOT, name)
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", name) or not os.path.isfile(full):
            self.send_error(404)
            return
        if flaky and name not in refused:
            refused.add(name)
            self.send_error(403)
            return
        size = os.path.getsize(full)
        m = re.fullmatch(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        start, end = 0, size - 1
        if m:
            start = int(m.group(1))
            if m.group(2):
                end = min(int(m.group(2)), size - 1)
        with open(full, "rb") as f:
            f.seek(start)
            body = f.read(end - start + 1)
        self.send_response(206 if m else 200)
        if m:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        self.wfile.write(body)

class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

srv = S(("127.0.0.1", 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
