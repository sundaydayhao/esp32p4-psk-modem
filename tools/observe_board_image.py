"""Observe an already-running HPM image transmitter; configure P4 RX only."""
import argparse
import re
import time
from pathlib import Path
import serial

p=argparse.ArgumentParser()
p.add_argument('--port',default='COM21');p.add_argument('--seconds',type=int,default=100)
p.add_argument('--gain',type=float)
p.add_argument('--label',default='default')
p.add_argument('--keep',action='store_true',help='Continue the current image session without resetting it')
args=p.parse_args()
s=serial.Serial(port=None,baudrate=115200,timeout=.05)
s.dtr=False;s.rts=False;s.port=args.port;s.open()
log=bytearray(); start=time.monotonic();next_diag=start+5
rows=0
try:
    if args.gain is not None:
        assert 0 <= args.gain <= 42
        s.write(f'INGAIN {args.gain}\n'.encode())
        until=time.monotonic()+.5
        while time.monotonic()<until:log.extend(s.read(4096))
    if not args.keep:s.write(b'CONTENT IMAGE\n')
    while time.monotonic()-start<args.seconds:
        log.extend(s.read(4096))
        if time.monotonic()>=next_diag:
            s.write(b'RXDIAG\n'); next_diag+=5
        text=log.decode(errors='replace')
        matches=list(re.finditer(r'content=Image epoch=\d+ image=(\d+)/96 bad=(\d+)',text))
        completed=list(re.finditer(r'image_completed=(\d+)',text))
        if matches and int(matches[-1][1]) != rows:
            rows=int(matches[-1][1]);print(f'{time.monotonic()-start:.1f}s rows={rows}/96 bad={matches[-1][2]}',flush=True)
            if rows==96:break
        if completed and int(completed[-1][1])>0:
            rows=96;print('Complete image confirmed by board counter: '+completed[-1][0],flush=True);break
finally:s.close()
assert re.fullmatch(r'[a-zA-Z0-9_-]+',args.label)
path=Path(__file__).resolve().parents[1]/f'build/board-hpm-image-rx-{args.label}.log';path.write_bytes(log)
print(f'{"PASS" if rows==96 else "INCOMPLETE"}: physical HPM to P4 image, rows={rows}/96; {path}',flush=True)
