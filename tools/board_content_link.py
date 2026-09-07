"""HPM -> P4 physical image check. Changes only runtime modem settings, no flashing."""
import argparse
import re
import time
from pathlib import Path
import serial

parser=argparse.ArgumentParser()
parser.add_argument('--mode',choices=['2x8PSK1200F','8PSK1000'],default='8PSK1000')
parser.add_argument('--seconds',type=int,default=90)
parser.add_argument('--gain',type=float,default=22)
parser.add_argument('--tx-dac',type=int,default=220)
parser.add_argument('--tx-hp',type=int,default=122)
parser.add_argument('--label',default='board-image')
args=parser.parse_args()
assert 0<=args.gain<=42
assert 0<=args.tx_dac<=255 and 0<=args.tx_hp<=127
assert re.fullmatch(r'[a-zA-Z0-9_-]+',args.label)
def open_port(name):
    s=serial.Serial(port=None,baudrate=115200,timeout=.01)
    s.dtr=False;s.rts=False;s.port=name;s.open();return s
p4=open_port('COM21'); hpm=open_port('COM23'); logs={p4:bytearray(),hpm:bytearray()}
def collect(seconds):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        pending=False
        for s in (p4,hpm):
            if s.in_waiting:
                pending=True;logs[s].extend(s.read(min(s.in_waiting,65536)))
        if not pending:time.sleep(.001)
def send(s,cmd):
    data=(cmd+'\n').encode()
    for offset in range(0,len(data),8):
        s.write(data[offset:offset+8]);collect(.05)
    collect(.6)
success=False
try:
    collect(8)
    send(hpm,'ROLE RX')
    send(hpm,f'OUTVOL {args.tx_dac} {args.tx_hp}')
    send(hpm,'CONTENT IMAGE');send(hpm,'TXMODE '+args.mode);send(hpm,'TXTONE 2000')
    send(p4,'ROLE RX');send(p4,f'INGAIN {args.gain}');send(p4,'CONTENT IMAGE')
    send(p4,'MODE '+args.mode);send(p4,'TONE 2000')
    send(p4,'RXDIAG');send(p4,'STATUS');send(hpm,'STATUS')
    if ('psk='+args.mode).encode() not in logs[p4] or ('txmode='+args.mode).encode() not in logs[hpm]:
        raise RuntimeError('Board modes not confirmed')
    send(hpm,'ROLE TX')
    start=time.monotonic()
    print(f'Receiving image for {args.seconds}s without serial diagnostics',flush=True)
    # RXDIAG logs synchronously from the audio task; querying during the stream
    # can itself stall sampling. Inspect the completed capture after TX stops.
    collect(args.seconds)
    send(hpm,'ROLE RX');collect(.5);send(p4,'RXDIAG')
    text=logs[p4].decode(errors='replace')
    rows=list(re.finditer(r'content=Image epoch=\d+ image=(\d+)/96 bad=(\d+)',text))
    completed=list(re.finditer(r'image_completed=(\d+)',text))
    if rows:print(f'{args.mode} rows={rows[-1][1]}/96 bad={rows[-1][2]}',flush=True)
    success=bool((rows and int(rows[-1][1])==96) or (completed and int(completed[-1][1])>0))
finally:
    try:send(hpm,'ROLE RX')
    except serial.SerialException:pass
    root=Path(__file__).resolve().parents[1]/'build'
    for s,name in ((p4,'p4'),(hpm,'hpm')):
        (root/f'{args.label}-{args.mode}-{name}.log').write_bytes(logs[s]);s.close()
print(f'{"PASS" if success else "INCOMPLETE"}: HPM -> P4 {args.mode}; receiver gain {args.gain} dB',flush=True)
if not success:raise SystemExit(1)
