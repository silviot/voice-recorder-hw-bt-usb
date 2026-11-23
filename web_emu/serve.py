#!/usr/bin/env python3
"""
Zero-dependency local HTTP server for the Cassini OpenFW Browser Emulator.

Sets Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy headers so
SharedArrayBuffer + Atomics.wait work cleanly in Chrome, Firefox, and Safari.

Usage:
    python3 web_emu/serve.py [--port 8080] [--open]

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import webbrowser

WEB_DIR = Path(__file__).resolve().parent


class COOPCOEPHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(WEB_DIR), **kwargs)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main() -> int:
    ap = argparse.ArgumentParser(description="Serve Cassini OpenFW browser emulator")
    ap.add_argument("--host", default="127.0.0.1", help="Bind host (default: 127.0.0.1)")
    ap.add_argument("--port", type=int, default=8080, help="Bind port (default: 8080)")
    ap.add_argument("--open", action="store_true", help="Open browser automatically")
    args = ap.parse_args()

    wasm_path = WEB_DIR / "openfw.wasm"
    if not wasm_path.exists():
        print(f"Warning: {wasm_path} not found. Build it first with: make -C open_fw wasm")

    url = f"http://{args.host}:{args.port}/"
    server = ThreadingHTTPServer((args.host, args.port), COOPCOEPHandler)
    print(f"Serving Cassini OpenFW Browser Emulator at {url}")
    print("Press Ctrl+C to stop.")
    if args.open:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
