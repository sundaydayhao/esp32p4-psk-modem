"""Export the headless LVGL framebuffer captures without third-party packages."""
import struct
import sys
import zlib
from pathlib import Path


def chunk(name, payload):
    return (struct.pack('>I', len(payload)) + name + payload
            + struct.pack('>I', zlib.crc32(name + payload)))


for path in Path(sys.argv[1]).glob('lcd-*.ppm'):
    magic, dimensions, depth, pixels = path.read_bytes().split(b'\n', 3)
    width, height = map(int, dimensions.split())
    assert magic == b'P6' and depth == b'255'
    assert len(pixels) == width * height * 3
    rows = b''.join(b'\0' + pixels[y * width * 3:(y + 1) * width * 3]
                    for y in range(height))
    data = (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
    path.with_suffix('.png').write_bytes(data)
