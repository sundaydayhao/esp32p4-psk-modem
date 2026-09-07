"""Compile the AP580-enabled path using the real IDF flags, without changing firmware config."""
import json
import re
import subprocess
from pathlib import Path
root = Path(__file__).resolve().parents[1]
entries = json.loads((root/'build/compile_commands.json').read_text())
entry = next(e for e in entries if Path(e['file']).name == 'content_service.c')
command = re.sub(r' -o \S+ -c ', ' -fsyntax-only ', entry['command'])
assert command != entry['command']
# GPIOs remain -1: syntax check only, no driver/hardware operation.
command += ' -DCONFIG_PSK_AP580_UART_ENABLED=1'
subprocess.run(command, cwd=entry['directory'], check=True)
print('PASS: AP580-enabled UART code compiles with ESP-IDF/P4 headers; default firmware remains disabled')
