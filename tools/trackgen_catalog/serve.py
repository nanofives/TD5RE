#!/usr/bin/env python3
"""serve.py -- serve the auto-track catalogue on localhost only.

    python tools/trackgen_catalog/serve.py            # http://127.0.0.1:6700/
    python tools/trackgen_catalog/serve.py --build    # rebuild first, then serve
    python tools/trackgen_catalog/serve.py --port 6701

Port 6700 is registered for TD5RE in Proyectos/PORTS.md. Binds 127.0.0.1, never
0.0.0.0. Standard library only.
"""
from __future__ import annotations

import argparse
import functools
import http.server
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SITE = os.path.join(HERE, "site")
PORT = 6700


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):  # quiet: one line per page, not per image
        if self.path.endswith(".html") or self.path == "/":
            sys.stderr.write("%s %s\n" % (self.command, self.path))

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--build", action="store_true", help="run build.py first")
    a = ap.parse_args()
    if a.build or not os.path.exists(os.path.join(SITE, "index.html")):
        r = subprocess.run([sys.executable, os.path.join(HERE, "build.py")])
        if r.returncode != 0:
            return r.returncode
    handler = functools.partial(Handler, directory=SITE)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), handler)
    print(f"auto-track catalogue: http://127.0.0.1:{a.port}/  (Ctrl+C to stop)", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
