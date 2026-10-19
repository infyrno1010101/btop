#!/usr/bin/env python3
"""Passive Ollama and explicit process-mapping terminal integration check."""
import codecs
import fcntl
import json
import os
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pyte


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.paths.append(self.path)
        if self.path != "/api/ps":
            self.send_error(404)
            return
        body = json.dumps({"models": [{
            "name": "gemma3:latest",
            "size_vram": 5 * 1024**3,
            "digest": "ignored-private-metadata",
        }]}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        self.server.posts += 1
        self.send_error(405)

    def log_message(self, *_):
        pass


with tempfile.TemporaryDirectory(prefix="btop-ollama-ui-") as directory:
    root = Path(directory)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.paths, server.posts = [], 0
    threading.Thread(target=server.serve_forever, daemon=True).start()
    connections = root / "servers.json"
    connections.write_text(json.dumps([{
        "name": "Local Ollama",
        "runtime": "ollama",
        "endpoint": f"127.0.0.1:{server.server_port}",
        "pid": os.getpid(),
    }]))
    config = root / "btop.conf"
    config.write_text(
        f'shown_boxes = "proc"\nai_servers_file = "{connections}"\n'
        'ai_panel = true\nupdate_ms = 500\nshow_gpu_info = "Off"\n'
    )
    master, slave = pty.openpty()
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 110, 0, 0))
    process = subprocess.Popen(
        [str(Path(sys.argv[1]).resolve()), "--config", str(config)],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env={**os.environ, "TERM": "xterm-256color"},
    )
    os.close(slave)
    screen = pyte.Screen(110, 30)
    stream = pyte.Stream(screen)
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    pending = b""

    def screen_text():
        return "\n".join(screen.display)

    try:
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if not select.select([master], [], [], 0.1)[0]:
                continue
            pending += os.read(master, 65536)
            while (finish := pending.find(b"\x1b[?2026l")) >= 0:
                finish += len(b"\x1b[?2026l")
                stream.feed(decoder.decode(pending[:finish]))
                pending = pending[finish:]
            text = screen_text()
            if ("Loaded models: 1 | gemma3:latest" in text
                    and "Model VRAM: 5.0 GiB" in text
                    and "Tokens: N/A (Ollama has no passive" in text
                    and f"PID {os.getpid()} 1p" in text
                    and "GPU 0% | VRAM 0B" in text):
                break
        else:
            Path("/tmp/btop-ollama-ui-failure.txt").write_text(screen_text())
            raise AssertionError("Ollama panel did not render expected telemetry")
        assert server.paths and set(server.paths) == {"/api/ps"}, server.paths
        assert server.posts == 0, "monitoring must never create inference requests"
        os.write(master, b"q")
        assert process.wait(timeout=3) == 0
        print("PASS: passive Ollama telemetry and explicit process mapping")
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
        os.close(master)
        server.shutdown()
