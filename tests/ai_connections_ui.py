#!/usr/bin/env python3
"""Multi-server terminal checks: python tests/ai_connections_ui.py /path/to/btop.

Linux; requires pyte. Uses two mock local servers and temporary credentials.
BTOP_TEST_RUNTIME=vllm or sglang selects a mixed-runtime test (default: llama.cpp).
"""
import codecs
import fcntl
import json
import os
import pty
import re
import select
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import pyte


runtime = os.environ.get("BTOP_TEST_RUNTIME", "vllm" if os.environ.get("BTOP_TEST_VLLM") == "1" else "llama.cpp")
assert runtime in {"llama.cpp", "vllm", "sglang"}
mixed = runtime != "llama.cpp"
beta_runtime = {"llama.cpp": "llama.cpp", "vllm": "vLLM", "sglang": "SGLang"}[runtime]


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        expected = 'Bearer beta-test-key' if self.server.label == 'Beta' else None
        if self.headers.get('Authorization') != expected:
            self.server.wrong_keys += 1
            self.send_error(401)
            return
        if self.server.fail:
            self.send_error(401)
            return
        if self.path == '/metrics':
            if mixed and self.server.label == 'Beta':
                self.server.ticks += 1
                if runtime == 'sglang':
                    body = (f'sglang:gen_throughput {self.server.rate}\nsglang:num_running_reqs 2\nsglang:num_queue_reqs 0\n'
                            'sglang:generation_tokens_total 100\nsglang:prompt_tokens_total 1000\n').encode()
                else:
                    body = (f'vllm:num_requests_running 2\nvllm:num_requests_waiting 0\n'
                            f'vllm:generation_tokens_total{{model_name="test",engine="0"}} {self.server.ticks * 300}\n'
                            f'vllm:prompt_tokens_total 1000\n').encode()
            else:
                body = f'llamacpp:requests_processing {self.server.active}\nllamacpp:requests_deferred 0\n'.encode()
        elif self.path == '/slots':
            assert not (mixed and self.server.label == 'Beta'), 'metrics exporters must never poll /slots'
            self.server.slot_requests += 1
            self.server.ticks += 1
            body = json.dumps([dict(id=index, id_task=42+index, is_processing=True,
                                    next_token=dict(n_decoded=self.server.ticks * self.server.step))
                               for index in range(self.server.active)]).encode()
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


with tempfile.TemporaryDirectory(prefix='btop-multi-ui-') as directory:
    root = Path(directory)
    servers = []
    entries = []
    for index, label in enumerate(['Alpha', 'Beta']):
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.label, server.ticks, server.wrong_keys = label, 0, 0
        server.rate, server.slot_requests = 300.5, 0
        server.active, server.step, server.fail = index + 1, (index + 1) * 50, False
        threading.Thread(target=server.serve_forever, daemon=True).start()
        servers.append(server)
        entry = dict(name=label, endpoint=f'127.0.0.1:{server.server_port}')
        if label == 'Beta':
            if mixed:
                entry['runtime'] = runtime
            key = root / 'beta.key'
            key.write_text('beta-test-key\n')
            key.chmod(0o600)
            entry['api_key_file'] = str(key)
        entries.append(entry)
    connections = root / 'servers.json'

    def replace_entries(value):
        staged = root / 'servers.new'
        staged.write_text(json.dumps(value))
        staged.replace(connections)

    replace_entries(entries)
    config = root / 'btop.conf'
    config.write_text(f'shown_boxes = "proc"\nai_servers_file = "{connections}"\n'
                      'ai_panel = true\nupdate_ms = 500\nshow_gpu_info = "Off"\n')
    master, slave = pty.openpty()
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 100, 0, 0))
    process = subprocess.Popen([str(Path(sys.argv[1]).resolve()), '--config', str(config)],
                               stdin=slave, stdout=slave, stderr=slave,
                               env={**os.environ, 'TERM': 'xterm-256color'})
    os.close(slave)
    screen = pyte.Screen(100, 30)
    stream = pyte.Stream(screen)
    decoder = codecs.getincrementaldecoder('utf-8')('replace')
    pending = b''

    def drain(seconds=.7):
        global pending
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if not select.select([master], [], [], .05)[0]:
                continue
            try:
                pending += os.read(master, 65536)
            except OSError:
                break
            while (finish := pending.find(b'\x1b[?2026l')) >= 0:
                finish += len(b'\x1b[?2026l')
                stream.feed(decoder.decode(pending[:finish]))
                pending = pending[finish:]

    def text():
        return '\n'.join(screen.display)

    def wait_for(predicate, label, seconds=5):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            drain(.1)
            if predicate(text()):
                print('PASS:', label, flush=True)
                return
        Path('/tmp/btop-multi-ui-failure.txt').write_text(text())
        raise AssertionError(label + ' (screen: /tmp/btop-multi-ui-failure.txt)')

    def send(value):
        os.write(master, value)
        drain()

    try:
        wait_for(lambda t: 'AI inference 1/2 [A]' in t and 'Alpha' in t
                 and re.search(r'(?:Live|Rate|Gen\(exporter\)): [1-9][0-9.]* tok/s', t), 'first server has live data')
        alpha_peak = float(re.search(r'tok/s peak ([0-9.]+)', text())[1])
        assert servers[1].ticks >= 2, 'unselected server must also be polled'
        send(b']')
        wait_for(lambda t: 'AI inference 2/2 [A]' in t and 'Beta' in t
                 and re.search(r'(?:Live|Rate|Gen\(exporter\)): [1-9][0-9.]* tok/s', t), 'next server already has a live rate')
        assert 'Active: 2' in text()
        if runtime == 'sglang':
            servers[1].rate = 0
            wait_for(lambda t: 'Gen(exporter): 0.0 tok/s' in t, 'exporter zero is displayed as zero')
            servers[1].rate = 150.25
            wait_for(lambda t: 'Gen(exporter): 150.2 tok/s' in t, 'exporter updates independently of completed-token totals')
        beta_peak = float(re.search(r'tok/s peak ([0-9.]+)', text())[1])
        assert beta_peak > alpha_peak * 2, 'each server needs its own history and scale'
        assert servers[0].wrong_keys == servers[1].wrong_keys == 0
        send(b'A')
        wait_for(lambda t: f'{beta_runtime} [2/2] Beta' in t, 'compact mode identifies the selected server')
        send(b']')
        wait_for(lambda t: 'llama.cpp [1/2] Alpha' in t, 'next wraps to the first server')
        send(b'[')
        wait_for(lambda t: f'{beta_runtime} [2/2] Beta' in t, 'previous wraps to the last server')
        send(b'A')
        wait_for(lambda t: 'AI inference 2/2 [A]' in t, 'full panel restored')
        header = next(i + 1 for i, line in enumerate(screen.display) if 'AI inference' in line)
        send(f'\x1b[<0;7;{header+6}M'.encode())
        wait_for(lambda t: 'AI inference 1/2 [A]' in t and 'Alpha' in t, 'click previous button')
        send(f'\x1b[<0;16;{header+6}M'.encode())
        wait_for(lambda t: 'AI inference 2/2 [A]' in t and 'Beta' in t, 'click next button')
        replace_entries(list(reversed(entries)))
        wait_for(lambda t: 'AI inference 1/2 [A]' in t and 'Beta' in t
                 and re.search(r'(?:Live|Rate|Gen\(exporter\)): [1-9][0-9.]* tok/s', t), 'reload preserves selected server and rate')
        assert float(re.search(r'tok/s peak ([0-9.]+)', text())[1]) >= beta_peak
        servers[1].fail = True
        wait_for(lambda t: 'authentication required' in t, 'server failure is visible')
        send(b']')
        wait_for(lambda t: 'Alpha' in t and re.search(r'(?:Live|Rate|Gen\(exporter\)): [1-9][0-9.]* tok/s', t),
                 'other server remains healthy during failure')
        replace_entries([entries[0]])
        wait_for(lambda t: 'AI inference [A]' in t and 'Alpha' in t, 'removing a server refreshes navigation')
        assert float(re.search(r'tok/s peak ([0-9.]+)', text())[1]) < beta_peak / 2
        assert servers[0].wrong_keys == servers[1].wrong_keys == 0
        if mixed:
            assert servers[1].slot_requests == 0
        assert 'beta-test-key' not in text()
        send(b'q')
        assert process.wait(timeout=3) == 0
        saved = config.read_text()
        assert 'ai_servers_file = ' in saved and 'beta-test-key' not in saved
        print('PASS: clean exit and no credentials in saved configuration', flush=True)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
        os.close(master)
        for server in servers:
            server.shutdown()
