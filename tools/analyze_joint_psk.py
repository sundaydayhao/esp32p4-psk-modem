"""Explain errors in complete text frames; edit distance is not channel bit BER."""
import argparse
import json
from pathlib import Path


def distance(a, b):
    row = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        next_row = [i]
        for j, y in enumerate(b, 1):
            next_row.append(min(row[j] + 1, next_row[j-1] + 1, row[j-1] + (x != y)))
        row = next_row
    return row[-1]


def analyze(raw, expected):
    begin = expected[:expected.index(b']') + 1]
    end = expected[expected.index(b'[END-'):]
    positions = []
    pos = raw.find(begin)
    while pos >= 0:
        positions.append(pos)
        pos = raw.find(begin, pos + len(begin))
    complete = 0
    bad = []
    for i, pos in enumerate(positions):
        limit = positions[i+1] if i+1 < len(positions) else len(raw)
        stop = raw.find(end, pos + len(begin), limit)
        if stop < 0:
            continue
        payload = raw[pos:stop + len(end)]
        complete += 1
        if payload != expected:
            bad.append(dict(offset=pos, bytes=len(payload), edits=distance(payload, expected),
                            data=payload.decode('ascii', errors='backslashreplace')))
    return dict(complete_frames=complete, correct_frames=complete-len(bad),
                bad_frames=len(bad), minimum_byte_edits=sum(f['edits'] for f in bad),
                bad=bad)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('raw', type=Path)
    parser.add_argument('expected', type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.raw.read_bytes(), args.expected.read_bytes()), indent=2))
