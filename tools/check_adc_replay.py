"""Strict check of the fixed physical audio fixture, including total frame count."""
import argparse
from pathlib import Path
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('raw',type=Path)
parser.add_argument('--expected',type=Path,default=root/'tests/data/hpm_2x8psk1200f_expected.txt')
parser.add_argument('--frames',type=int,default=27)
args=parser.parse_args()
expected=args.expected.read_bytes()
raw=args.raw.read_bytes()
start=raw.find(expected[:15]);finish=raw.rfind(expected[-13:])
assert start>=0 and finish>=start, 'Missing frame boundaries'
checked=raw[start:finish+13]
assert checked==expected*args.frames, f'Physical fixture differs: {len(checked)} bytes; expected {len(expected)*args.frames} exact bytes'
print(f'PASS physical ADC replay: {args.frames} complete frames / {len(checked)} bytes exact')
