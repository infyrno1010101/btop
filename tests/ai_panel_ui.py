#!/usr/bin/env python3
"""AI-panel terminal integration checks (Linux; test dependency: pyte).

Usage: python tests/ai_panel_ui.py /path/to/btop
Optional screenshot: BTOP_PANEL_SCREENSHOT=/path/to/panel.png (requires Pillow).
Uses a mock loopback server and temporary configuration, never real credentials.
"""
import sys, tempfile
import os, pty, select, subprocess, time, fcntl, termios, struct, codecs, signal, re, json, threading
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import pyte

class Handler(BaseHTTPRequestHandler):
    ticks = 0
    def do_GET(self):
        if self.path == '/metrics':
            body = b'llamacpp:predicted_tokens_seconds 0\nllamacpp:tokens_predicted_total 7470\nllamacpp:tokens_predicted_seconds_total 227.973\nllamacpp:requests_processing 1\nllamacpp:requests_deferred 2\n'
        elif self.path == '/slots':
            Handler.ticks += 1
            prompt = min(Handler.ticks, 2) * 100
            decoded = max(Handler.ticks - 2, 0) * 75
            body = json.dumps([dict(id=0,id_task=42,is_processing=True,n_prompt_tokens_processed=prompt,next_token=[dict(n_decoded=decoded)])]).encode()
        else:
            self.send_error(404);return
        self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
    def log_message(self,*args): pass

server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
endpoint=f'127.0.0.1:{server.server_port}'
workdir=tempfile.TemporaryDirectory(prefix='btop-panel-ui-')
config=Path(workdir.name)/'btop.conf'
config.write_text(f'shown_boxes = "proc"\nai_llama_endpoint = "{endpoint}"\nai_llama_live = true\nai_panel = true\nproc_follow_detailed = false\nupdate_ms = 500\nshow_gpu_info = "Off"\n')
master,slave=pty.openpty()
def winsize(rows,cols): fcntl.ioctl(master,termios.TIOCSWINSZ,struct.pack('HHHH',rows,cols,0,0))
winsize(40,160)
p=subprocess.Popen([str(Path(sys.argv[1]).resolve()),'--config',str(config)],stdin=slave,stdout=slave,stderr=slave,env={**os.environ,'TERM':'xterm-256color'})
os.close(slave)
screen=pyte.Screen(160,40);stream=pyte.Stream(screen);decoder=codecs.getincrementaldecoder('utf-8')('replace')
pending=b''
def drain(seconds=.7):
    global pending
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if select.select([master],[],[],.05)[0]:
            try: data=os.read(master,65536)
            except OSError: break
            pending+=data
            while (finish:=pending.find(b'\x1b[?2026l')) >= 0:
                finish+=len(b'\x1b[?2026l')
                stream.feed(decoder.decode(pending[:finish]));pending=pending[finish:]
def send(keys): os.write(master,keys);drain()
def text(): return '\n'.join(screen.display)
def check(condition,label):
    if not condition:
        Path('/tmp/btop-panel-ui-failure.txt').write_text(text())
        raise AssertionError(label+' (screen: /tmp/btop-panel-ui-failure.txt)')
    print('PASS:',label,flush=True)
def screenshot():
    from PIL import Image,ImageDraw,ImageFont
    font=ImageFont.truetype('/usr/share/fonts/noto/NotoSansMono-Regular.ttf',16)
    cw=10;ch=23
    first=next(i for i,line in enumerate(screen.display) if 'AI inference' in line)
    im=Image.new('RGB',(screen.columns*cw,8*ch),'#111118');draw=ImageDraw.Draw(im)
    for y in range(first,min(first+8,screen.lines)):
        for x in range(screen.columns):
            c=screen.buffer[y][x]
            fg='#d8dde8'
            if len(c.fg)==6 and all(k in '0123456789abcdef' for k in c.fg.lower()):fg='#'+c.fg
            draw.text((x*cw,(y-first)*ch),c.data,font=font,fill=fg)
    im.save(os.environ['BTOP_PANEL_SCREENSHOT'])
try:
    saw_prompt_rate = False
    for _ in range(70):
        drain(.1)
        if re.search(r'Live: [1-9][0-9.]* tok/s \(prompt\)', text()):
            saw_prompt_rate = True
        if re.search(r'Live: [1-9][0-9.]* tok/s \(gen\)',text()): break
    check(saw_prompt_rate, 'live prompt-processing rate displayed')
    check('AI inference [A]' in text(),'dedicated panel visible')
    check('Server: '+endpoint in text(),'source endpoint visible')
    check(bool(re.search(r'Live: [1-9][0-9.]* tok/s',text())),'live rate displayed')
    for value in ['Active: 1 | Queue: 2','Output:','Prompt: 200','Avg(total): 32.8 tok/s']:
        check(value in text(),value)
    check('tok/s peak' in text() and 'Queue peak' in text(),'wide panel shows throughput and queue history')
    check('~2s/col' in text(),'history sampling scale is labeled')
    first=next(i for i,line in enumerate(screen.display) if 'AI inference' in line)
    check(all('\ufffd' not in line for line in screen.display[first:first+7]),'Unicode history bars render without split characters')
    check('select' in screen.display[-1] and 'info' in screen.display[-1],'process controls remain on outer footer')
    if os.environ.get('BTOP_PANEL_SCREENSHOT'): screenshot()
    send(b'u');send(b'\x1b[B');send(b'\x1b[F');drain(.5)
    location=re.search(r'(\d+)/(\d+)',screen.display[-1])
    check(location is not None and location[1]==location[2],'End reaches the last process with panel visible')
    before=location.group(0)
    panel_y=next(i for i,line in enumerate(screen.display) if 'AI inference' in line)+1
    send(f'\x1b[<0;10;{panel_y+2}M\x1b[<0;10;{panel_y+2}m'.encode())
    drain(.3)
    check(before in screen.display[-1],'clicking panel data does not change process selection')
    send(b'u')
    send(f'\x1b[<0;10;{panel_y}M\x1b[<0;10;{panel_y}m'.encode())
    check('AI inference [A]' not in text() and re.search(r' tok/s \((?:prompt|gen)\) \| out ', text()),'clicking title switches to compact metrics')
    send(b'A')
    check('AI inference [A]' in text(),'Shift+A restores the panel')
    check('select' in screen.display[-1],'footer survives toggling')
    send(b'\x1b[H');send(b'\x1b[B');send(b'\r')
    check('AI inference [A]' in text(),'panel coexists with process details')
    send(b'\r')
    screen.resize(lines=16,columns=160);winsize(16,160);os.kill(p.pid,signal.SIGWINCH);drain(1)
    check('AI inference [A]' in text(),'panel remains visible in a 16-row process area')
    send(b'\x1b[H');send(b'\x1b[B');send(b'\r')
    check('AI inference [A]' not in text() and re.search(r' tok/s \((?:prompt|gen)\) \| out ', text()),'short window with process details falls back to compact')
    screen.resize(lines=40,columns=160);winsize(40,160);os.kill(p.pid,signal.SIGWINCH);drain(1)
    check('AI inference [A]' in text(),'panel returns after growing the window')
    check('tok/s peak' in text(),'history returns after growing the window')
    send(b'\r')
    screen.resize(lines=22,columns=56);winsize(22,56);os.kill(p.pid,signal.SIGWINCH);drain(1)
    check('tok/s peak' in text() and 'Queue peak' in text(),'graphs fit a 56-column process panel')
    screen.resize(lines=22,columns=44);winsize(22,44);os.kill(p.pid,signal.SIGWINCH);drain(1)
    check('AI inference [A]' in text(),'panel fits minimum process width')
    check('Active: 1 | Queue: 2' in text() and 'Avg(total): 32.8 tok/s' in text(),'key metrics fit at minimum width')
    check('tok/s peak' not in text() and 'Queue peak' not in text(),'narrow panel preserves numbers instead of cramped graphs')
    screen.resize(lines=26,columns=116);winsize(26,116);os.kill(p.pid,signal.SIGWINCH);drain(1)
    send(b'1');send(b'2');send(b'3')
    check('AI inference [A]' in text() and 'tok/s peak' in text() and 'Queue peak' in text(),
          'graphs remain visible in a 116x26 CPU/memory/network/process dashboard')
    if os.environ.get('BTOP_PANEL_SCREENSHOT'): screenshot()
    send(b'q');check(p.wait(timeout=3)==0,'clean exit')
finally:
    if p.poll() is None: p.terminate();p.wait(timeout=3)
    os.close(master);server.shutdown();workdir.cleanup()
