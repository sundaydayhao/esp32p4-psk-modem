"""HPM6750 TX (unchanged firmware) -> P4 RX diagnostic captures.

Raw payload windows contain no diagnostic queries or log stripping. Exact repeated
frames are counted; counts are not presented as raw channel BER.
"""
import argparse
import json
import random
import re
import time
from pathlib import Path
import serial

ROOT = Path(__file__).resolve().parents[1]
MODES = {'8PSK125F':65, '8PSK250F':45, '2x8PSK1200F':25}

def port(name):
    s = serial.Serial(port=None, baudrate=115200, timeout=.01, write_timeout=3)
    s.dtr = False; s.rts = False; s.port = name; s.open()
    return s

def frame(seed):
    r = random.Random(seed)
    return ('[BEGIN-P4-%04X]' % seed + ''.join(r.choice('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789') for _ in range(128)) + '[END-P4-%04X]' % seed).encode()

def score(raw, expected):
    first, last = expected[:15], expected[-13:]
    import numpy as np
    data=np.frombuffer(raw,dtype=np.uint8)
    pattern=np.frombuffer(expected,dtype=np.uint8)
    indices=np.arange(len(data)) % len(pattern)
    counts=[int(np.count_nonzero(data != pattern[(indices+offset)%len(pattern)])) for offset in range(len(pattern))]
    best=min(counts) if len(data) else None
    # A zero value checks EVERY byte in the whole raw window against the cyclic
    # TX payload, including partial edge frames. Nonzero is not channel BER:
    # insertions/deletions can shift alignment and require separate analysis.
    return dict(received_bytes=len(raw), exact_frames=raw.count(expected),
        begin_markers=raw.count(first), end_markers=raw.count(last),
        cyclic_mismatching_bytes=best, all_received_bytes_exact=bool(len(data) and best==0),
        diagnostic_contamination=bool(re.search(rb'[IEWD] \(\d+\)', raw)))

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--modes',nargs='+',choices=MODES,default=list(MODES))
    ap.add_argument('--seconds',type=float)
    ap.add_argument('--gain',type=float,default=22)
    ap.add_argument('--warmup',type=float,default=0)
    ap.add_argument('--branch',choices=['AUTO','0','1'],default='AUTO')
    ap.add_argument('--label',required=True)
    ap.add_argument('--tx-dac',type=int)
    ap.add_argument('--tx-hp',type=int)
    ap.add_argument('--require-clean',action='store_true')
    ap.add_argument('--seed',type=int,default=0x3a17)
    args=ap.parse_args()
    assert (args.tx_dac is None)==(args.tx_hp is None)
    if args.tx_dac is not None:assert 0<=args.tx_dac<=255 and 0<=args.tx_hp<=127
    assert re.fullmatch('[a-zA-Z0-9_-]+',args.label) and 0 <= args.gain <= 42
    out=ROOT/'build/psk-opt-20260908'/args.label;out.mkdir(parents=True,exist_ok=True)
    tx,rx=port('COM23'),port('COM21'); logs={tx:bytearray(),rx:bytearray()}
    def collect(seconds, payload=None):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            pending=False
            for s in (rx,tx):
                n=s.in_waiting
                if not n:continue
                pending=True
                b=s.read(min(n,65536))
                logs[s].extend(b)
                if s is rx and payload is not None:payload.extend(b)
            if not pending:time.sleep(.001)
    def send(s,text,settle=.65):
        data=(text+'\n').encode()
        # HPM's USB command ring needs paced writes; a bulk line can lose bytes.
        for offset in range(0,len(data),8):
            s.write(data[offset:offset+8]); collect(.05)
        collect(settle)
    results=[]
    try:
        collect(2)
        send(tx,'');send(rx,'')
        send(tx,'STATUS'); send(rx,'STATUS')
        (out/'initial-hpm.log').write_bytes(logs[tx]);(out/'initial-p4.log').write_bytes(logs[rx])
        send(tx,'ROLE RX');send(tx,'LOG OFF');send(tx,'CONTENT TEXT')
        if args.tx_dac is not None:send(tx,f'OUTVOL {args.tx_dac} {args.tx_hp}')
        send(rx,'ROLE RX');send(rx,'CONTENT TEXT');send(rx,f'INGAIN {args.gain}')
        for mode in args.modes:
            logs[tx].clear();logs[rx].clear()
            send(tx,'ROLE RX');send(tx,'TXMODE '+mode);send(tx,'TXTONE 2000')
            send(rx,'MODE '+mode,1.5);send(rx,'TONE 2000',1.5);send(rx,'FECBRANCH '+args.branch)
            send(rx,'STATUS');send(tx,'STATUS')
            if ('psk='+mode).encode() not in logs[rx]:raise RuntimeError('P4 mode not confirmed')
            expected=frame(args.seed);raw=bytearray()
            (out/(mode+'.expected')).write_bytes(expected)
            collect(.5); rx.reset_input_buffer()
            send(tx,'TX '+expected.decode(),0)
            collect(args.warmup)
            seconds=args.seconds or MODES[mode]
            print(f'RUN {mode} {seconds}s gain={args.gain} branch={args.branch}',flush=True)
            collect(seconds,raw)
            if b"text='"+expected+b"'" not in logs[tx]:
                (out/(mode+'-hpm-error.log')).write_bytes(logs[tx])
                raise RuntimeError('HPM payload echo did not exactly match; measurement invalid')
            (out/(mode+'.raw')).write_bytes(raw)
            send(rx,'RXDIAG',1.0);send(rx,'STATUS');send(tx,'STATUS')
            row=dict(mode=mode,seconds=seconds,warmup=args.warmup,tx_dac=args.tx_dac,tx_hp=args.tx_hp,gain=args.gain,branch=args.branch,**score(raw,expected))
            row['diagnostics']=[m.group(0) for m in re.finditer(
                r'[IEWD] \(\d+\) psk-modem: (?:diag psk=|perf load=|heap internal=)[^\r\n]*',
                logs[rx].decode(errors='replace'))]
            results.append(row)
            (out/(mode+'-p4.log')).write_bytes(logs[rx]);(out/(mode+'-hpm.log')).write_bytes(logs[tx])
            (out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2))
            print(json.dumps(row,ensure_ascii=True),flush=True)
    finally:
        try:send(tx,'ROLE RX')
        finally:tx.close();rx.close()
    print('Saved',out,flush=True)
    if args.require_clean and not all(r['all_received_bytes_exact'] and r['exact_frames'] > 0 and not r['diagnostic_contamination'] for r in results):
        raise SystemExit('FAIL: at least one raw window was not completely exact')

if __name__=='__main__':main()
