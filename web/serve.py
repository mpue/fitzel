#!/usr/bin/env python3
"""Play an exported fitzel web game on this machine.

    python serve.py [port]

Serves the folder this file sits in at http://localhost:8000/ (or the port
given) and opens it in the browser. A plain file:// open does not work, and
neither does an arbitrary local server: the engine runs on several threads,
which browsers allow only on a page sent with the two cross-origin isolation
headers this server adds.
"""
import http.server
import os
import socketserver
import sys
import threading
import webbrowser

here = os.path.dirname(os.path.abspath(__file__))
nums = [a for a in sys.argv[1:] if a.isdigit()]
port = int(nums[0]) if nums else 8000


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
        ".fpak": "application/octet-stream",
    }

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=here, **kwargs)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def log_message(self, fmt, *args):
        pass


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


with Server(("127.0.0.1", port), Handler) as httpd:
    url = "http://localhost:%d/" % port
    print("Serving %s at %s  (Ctrl+C to stop)" % (here, url))
    if "--no-browser" not in sys.argv:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
