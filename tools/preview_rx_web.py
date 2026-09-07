"""Local browser QA fixture only. This does not emulate ESP-IDF or radio hardware."""
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit, parse_qs

ROOT = Path(__file__).resolve().parents[1]
MODES = ['BPSK31', '8PSK1000', '8PSK1200F', '2x8PSK1200F']
STATE = dict(boot_id='preview-only', mode='BPSK31', mode_id=0, tone_hz=2000,
             baud=31.25, carriers=1, information_bps=31.25, lock_state=3,
             input_level=1200, samples_per_second=48000, dsp_load_percent=24.6,
             dsp_max_us=1800, block_budget_us=5333, deadline_misses=0,
             read_errors=0, received_bytes=0, usb_dropped_bytes=0,
             internal_heap_min=120000, uptime_ms=10000)
DATA = b'LOCAL PREVIEW / simulated receive data\nCQ CQ DE HPM6750\nThe quick brown fox 0123456789.\n<script>alert("render as text")</script>\n\x00\xff'
STATE['received_bytes'] = len(DATA)


class Handler(BaseHTTPRequestHandler):
    def reply(self, body, kind='application/json', code=200, headers=None):
        self.send_response(code)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Boot-Id', STATE['boot_id'])
        for k, v in (headers or {}).items():
            self.send_header(k, str(v))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlsplit(self.path)
        if url.path == '/':
            self.reply((ROOT / 'main/rx.html').read_bytes(), 'text/html; charset=utf-8')
        elif url.path == '/api/status':
            self.reply(json.dumps(STATE).encode())
        elif url.path == '/api/modes':
            self.reply(json.dumps([dict(id=i, name=n) for i, n in enumerate(MODES)]).encode())
        elif url.path == '/api/rx':
            cursor = int(parse_qs(url.query).get('cursor', ['0'])[0])
            if cursor > len(DATA):
                cursor = 0
            chunk = DATA[cursor:cursor + 1024]
            self.reply(chunk, 'application/octet-stream', headers={
                'X-Next-Cursor': cursor + len(chunk), 'X-Lost-Bytes': 0})
        else:
            self.reply(b'Not found', 'text/plain', 404)

    def do_POST(self):
        if self.path != '/api/config':
            return self.reply(b'Not found', 'text/plain', 404)
        config = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        mode, tone = config['mode'], config['tone_hz']
        if not 0 <= mode < len(MODES) or not 100 <= tone <= 3500:
            return self.reply(b'Invalid', 'text/plain', 400)
        STATE.update(mode_id=mode, mode=MODES[mode], tone_hz=tone)
        self.reply(b'{"queued":true}', code=202)

    def log_message(self, *args):
        pass


if __name__ == '__main__':
    print('Preview fixture: http://127.0.0.1:8874 (no hardware)', flush=True)
    ThreadingHTTPServer(('127.0.0.1', 8874), Handler).serve_forever()
