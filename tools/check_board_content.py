"""P4 content smoke test. No AP580 output or RF/PSK transmission is enabled."""
import argparse
import time
from pathlib import Path
import serial
from esptool.reset import HardReset

arg = argparse.ArgumentParser()
arg.add_argument('--port',default='COM21')
args=arg.parse_args()
s=serial.Serial(port=None,baudrate=115200,timeout=0.1)
s.dtr=False; s.rts=False; s.port=args.port; s.open()
log=bytearray()
def capture(seconds):
    until=time.monotonic()+seconds
    while time.monotonic()<until: log.extend(s.read(4096))
def command(text,delay=1.0):
    s.write((text+'\n').encode()); capture(delay)
try:
    HardReset(s,uses_usb=True)(); capture(12)
    command('STATUS')
    command('CONTENT IMAGE',2)
    command('STATUS')
    command('RXDIAG')
    command('CONTENT AUDIO',2)
    command('STATUS')
    command('RXDIAG')
    command('CONTENT TEXT',2)
    command('STATUS')
finally:
    s.close()
path=Path(__file__).resolve().parents[1]/'build/board-content-com21.log'
path.write_bytes(log)
text=log.decode(errors='replace')
for line in text.splitlines():
    if any(k in line for k in ('session ', 'STATUS ', 'content=', 'AP ', 'started', 'UART', 'Guru', 'ERROR', 'assert', 'Brownout')):
        print(line.encode('ascii',errors='backslashreplace').decode())
assert all(x in text for x in ('Image/RX','Audio/RX','Text/RX','AP ESP32P4-PSK ready'))
assert not any(x in text for x in ('Guru Meditation','assert failed','Stack canary','Brownout','Task watchdog got triggered'))
print('PASS: board boot/AP, Image/RX, Audio/RX, back to Text/RX; log',path)
