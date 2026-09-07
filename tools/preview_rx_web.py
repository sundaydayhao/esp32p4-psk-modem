"""Local UI fixture only. Exercises the production HTML without a board or Wi-Fi."""
import json
import re
import struct
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
mode_names = re.findall(r'MODE\([^,]+, "([^"]+)"', (ROOT/'main/psk_modes.c').read_text())
allowed = {9, 10, 11, 12, 20, 21, 25, 26, 27}
state = dict(boot_id='preview-only', content_epoch=1, content_type=0, mode_id=0, tone_hz=2000,
    role='RX', lock_state=3, dsp_load_percent=12.5, received_bytes=0, baud=1230.769,
    carriers=2, information_bps=4923.077, samples_per_second=48000, dsp_max_us=900,
    block_budget_us=5333, deadline_misses=0, read_errors=0, image_rows=63, image_version=1,
    image_bad_rows=0, voice_rate=600, voice_packets=24, voice_frames=144, voice_crc_errors=0,
    uart_rx_frames=0, uart_tx_frames=0, uart_timeouts=0, queue_drops=0, uart_ready=False,
    last_voice_frame='59 4F 01 07 DD AA 96 E7 00 FF 12 00 00 00 00 24')
pixels = bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})', (ROOT/'main/modem_image_pixels.inc').read_text()))
assert len(pixels) == 9216
bmp = bytearray(54 + 96*96*3)
struct.pack_into('<2sIHHIIIIHH', bmp, 0, b'BM', len(bmp), 0, 0, 54, 40, 96, 96, 1, 24)
for y in range(96):
    for x in range(96):
        p = pixels[(95-y)*96+x] if 95-y < 63 else 0
        bmp[54+(y*96+x)*3:54+(y*96+x)*3+3] = bytes(((p&3)*85, ((p>>2)&7)*255//7, (p>>5)*255//7))

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def respond(self, data, mime='application/json', code=200):
        if not isinstance(data, (bytes,bytearray)): data = json.dumps(data).encode()
        self.send_response(code)
        for k,v in {'Content-Type':mime, 'Content-Length':str(len(data)), 'Cache-Control':'no-store',
            'X-Boot-Id':state['boot_id'], 'X-Content-Epoch':str(state['content_epoch']),
            'X-Image-Version':str(state['image_version']), 'X-Next-Cursor':'0', 'X-Lost-Bytes':'0'}.items():
            self.send_header(k,v)
        self.end_headers(); self.wfile.write(data)
    def do_GET(self):
        if self.path == '/': self.respond((ROOT/'main/rx.html').read_bytes(), 'text/html; charset=utf-8')
        elif self.path == '/api/status': self.respond(dict(state,mode=mode_names[state['mode_id']]))
        elif self.path == '/api/modes': self.respond([dict(id=i,name=n,image_allowed=i in allowed) for i,n in enumerate(mode_names)])
        elif self.path.startswith('/api/rx?'): self.respond(b'', 'application/octet-stream')
        elif self.path == '/api/image': self.respond(bmp, 'image/bmp')
        else: self.respond({},code=404)
    def do_POST(self):
        value = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        state.update(content_type=value['content_type'], mode_id=value['mode'],tone_hz=value['tone_hz'])
        state['content_epoch'] += 1
        self.respond({'queued':True},code=202)

print('Local UI fixture at http://127.0.0.1:8765 (sample data only)',flush=True)
HTTPServer(('127.0.0.1',8765),Handler).serve_forever()
