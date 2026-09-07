"""Capture P4 post-filter ADC audio from the unchanged HPM transmitter.

Uses a light BPSK31 decoder while recording, then stops TX and verifies the
binary USB dump's length and CRC before accepting the WAV for offline replay.
"""
import argparse, json, re, time, wave, zlib
from pathlib import Path
from joint_psk_check import port, frame, ROOT

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--mode',required=True)
    ap.add_argument('--seconds',type=int,default=30)
    ap.add_argument('--gain',type=int,default=10)
    ap.add_argument('--label',required=True)
    ap.add_argument('--seed',type=int,default=0x3a17)
    ap.add_argument('--tx-dac',type=int)
    ap.add_argument('--tx-hp',type=int)
    args=ap.parse_args()
    assert (args.tx_dac is None)==(args.tx_hp is None)
    if args.tx_dac is not None:assert 0<=args.tx_dac<=255 and 0<=args.tx_hp<=127
    assert 1<=args.seconds<=40 and re.fullmatch('[a-zA-Z0-9_-]+',args.label)
    out=ROOT/'build/psk-opt-20260908'/args.label;out.mkdir(parents=True,exist_ok=True)
    tx,rx=port('COM23'),port('COM21');logs={tx:bytearray(),rx:bytearray()}
    def collect(seconds):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            pending=False
            for s in (rx,tx):
                n=s.in_waiting
                if n:logs[s].extend(s.read(min(n,65536)));pending=True
            if not pending:time.sleep(.001)
    def send(s,cmd,settle=.7):
        line=(cmd+'\n').encode()
        for off in range(0,len(line),8):s.write(line[off:off+8]);collect(.05)
        collect(settle)
    try:
        collect(1);send(tx,'');send(rx,'')
        for cmd in ('ROLE RX','LOG OFF','CONTENT TEXT','TXMODE '+args.mode,'TXTONE 2000'):send(tx,cmd)
        if args.tx_dac is not None:send(tx,f'OUTVOL {args.tx_dac} {args.tx_hp}')
        for cmd in ('ROLE RX','CONTENT TEXT','MODE BPSK31','TONE 2000',f'INGAIN {args.gain}'):
            send(rx,cmd)
        expected=frame(args.seed);(out/'expected.bin').write_bytes(expected)
        send(tx,'TX '+expected.decode());collect(3)
        if b"text='"+expected+b"'" not in logs[tx]:raise RuntimeError('HPM TX echo mismatch')
        count=args.seconds*48000;send(rx,'CAP '+str(count))
        if f'[cap] armed={count}'.encode() not in logs[rx]:raise RuntimeError('Capture not armed')
        print(f'Capturing {args.mode}: {args.seconds}s at gain {args.gain}',flush=True)
        collect(args.seconds+2);send(rx,'RXDIAG')
        send(tx,'ROLE RX');collect(3)
        logs[rx].clear();send(rx,'CAPBIN',0)
        deadline=time.monotonic()+180
        while time.monotonic()<deadline:
            collect(.2)
            if b'[capbin-end]\n' in logs[rx]:break
        raw=bytes(logs[rx]);(out/'capture-usb.raw').write_bytes(raw)
        header=re.search(rb'\[capbin\] samples=(\d+) crc=([0-9a-f]{8})\n',raw)
        if not header:raise RuntimeError('Missing capture header')
        n=int(header[1]);wanted_crc=int(header[2],16)
        pcm=raw[header.end():header.end()+n*2]
        if n!=count or len(pcm)!=count*2 or zlib.crc32(pcm)!=wanted_crc:
            raise RuntimeError(f'Invalid dump n={n}/{count} bytes={len(pcm)} CRC={zlib.crc32(pcm):08x}/{wanted_crc:08x}')
        if raw[header.end()+n*2:][:13]!=b'[capbin-end]\n':raise RuntimeError('Missing end marker')
        with wave.open(str(out/'adc.wav'),'wb') as wav:
            wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(48000);wav.writeframes(pcm)
        import numpy as np
        x=np.frombuffer(pcm,dtype='<i2').astype(float)
        result=dict(mode=args.mode,gain=args.gain,samples=n,crc=f'{wanted_crc:08x}',
            rms=float(np.sqrt(np.mean(x*x))),peak=float(np.max(np.abs(x))),
            clipped_samples=int(np.count_nonzero(np.abs(x)>=32760)),post_filter=True)
        (out/'capture.json').write_text(json.dumps(result,indent=2))
        print(result,flush=True)
    finally:
        try:send(tx,'ROLE RX')
        finally:
            (out/'hpm.log').write_bytes(logs[tx]);(out/'p4.log').write_bytes(logs[rx]);tx.close();rx.close()

if __name__=='__main__':main()
