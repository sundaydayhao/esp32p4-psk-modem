#!/usr/bin/env python3
"""Offline WAV regression for the ESP32-S3 PSK modem mode table.

The test reads every WAV in C:\\projects\\psk-script\\generated-wav and checks
whether PSK modes in main/rx_modem.c can recover a
recognizable fragment from C:\\projects\\psk-script\\input.txt.

This is intentionally a host-side smoke test. It uses the same PSK varicode
table and the same non-FEC single-carrier BPSK bit polarity as the firmware:
no phase change is bit 1, and a 180-degree phase change is bit 0.
"""

from __future__ import annotations

import argparse
import math
import re
import sys
import wave
from dataclasses import dataclass
from pathlib import Path

import numpy as np


DEFAULT_PROJECT = Path(r"C:\projects\esp32s3-psk-modem")
DEFAULT_WAV_DIR = Path(r"C:\projects\psk-script\generated-wav")
DEFAULT_INPUT = Path(r"C:\projects\psk-script\input.txt")
DEFAULT_TONE_HZ = 1500.0
PUNCTURE = None


@dataclass(frozen=True)
class ModeInfo:
    enum_name: str
    name: str
    family: str
    baud: float
    bits_per_symbol: int
    carriers: int
    fec: bool
    punctured: bool
    decode_supported: bool


def parse_float_expr(expr: str) -> float:
    expr = expr.strip().rstrip("fF")
    if "/" in expr:
        left, right = expr.split("/", 1)
        return parse_float_expr(left) / parse_float_expr(right)
    return float(expr.strip().rstrip("fF"))


def load_modes(project: Path) -> dict[str, ModeInfo]:
    (project / "main" / "rx_modem.c").read_text(encoding="utf-8")

    def m(name: str, family: str, baud: float, symbits: int, carriers: int,
          fec: bool = False, punctured: bool = False) -> ModeInfo:
        return ModeInfo(
            enum_name=f"MODEM_MODE_{name.upper().replace('X', 'X')}",
            name=name,
            family=family,
            baud=baud,
            bits_per_symbol=symbits,
            carriers=carriers,
            fec=fec,
            punctured=punctured,
            decode_supported=True,
        )

    entries = [
        m("BPSK31", "BPSK", 31.25, 1, 1),
        m("BPSK63", "BPSK", 62.5, 1, 1),
        m("BPSK63F", "BPSK", 62.5, 1, 1, True),
        m("BPSK125", "BPSK", 125.0, 1, 1),
        m("BPSK250", "BPSK", 250.0, 1, 1),
        m("BPSK500", "BPSK", 500.0, 1, 1),
        m("BPSK1000", "BPSK", 1000.0, 1, 1),
        m("12xPSK125", "BPSK", 125.0, 1, 12),
        m("6xPSK250", "BPSK", 250.0, 1, 6),
        m("2xPSK500", "BPSK", 500.0, 1, 2),
        m("4xPSK500", "BPSK", 500.0, 1, 4),
        m("2xPSK800", "BPSK", 800.0, 1, 2),
        m("2xPSK1000", "BPSK", 1000.0, 1, 2),
        m("QPSK31", "QPSK", 31.25, 2, 1, True),
        m("QPSK63", "QPSK", 62.5, 2, 1, True),
        m("QPSK125", "QPSK", 125.0, 2, 1, True),
        m("QPSK250", "QPSK", 250.0, 2, 1, True),
        m("QPSK500", "QPSK", 500.0, 2, 1, True),
        m("8PSK125", "8PSK", 125.0, 3, 1),
        m("8PSK250", "8PSK", 250.0, 3, 1),
        m("8PSK500", "8PSK", 500.0, 3, 1),
        m("8PSK1000", "8PSK", 1000.0, 3, 1),
        m("8PSK125F", "8PSK", 125.0, 3, 1, True),
        m("8PSK250F", "8PSK", 250.0, 3, 1, True),
        m("8PSK500F", "8PSK", 500.0, 3, 1, True, True),
        m("8PSK1000F", "8PSK", 1000.0, 3, 1, True, True),
        m("8PSK1200F", "8PSK", 16000.0 / 13.0, 3, 1, True, True),
    ]
    return {entry.name: entry for entry in entries}


def load_varicode(project: Path) -> dict[int, str]:
    text = (project / "components" / "psk_rtty" / "include" / "PSKTables.h").read_text(
        encoding="utf-8"
    )
    body_match = re.search(r"VARICODE_TABLE\[256\]\s*=\s*\{(.*?)\};", text, re.S)
    if not body_match:
        raise RuntimeError("VARICODE_TABLE not found")
    values = [int(token, 0) for token in re.findall(r"0x[0-9A-Fa-f]+|\b\d+\b", body_match.group(1))]
    if len(values) != 256:
        raise RuntimeError(f"Expected 256 varicode entries, got {len(values)}")
    return {code: chr(ch) for ch, code in enumerate(values) if code != 0}


def load_mfsk_varicode() -> dict[int, str]:
    # IZ8BLY MFSK varicode decode table from fldigi mfskvaricode.cxx.
    values = [
        0x75C, 0x760, 0x768, 0x76C, 0x770, 0x774, 0x778, 0x77C,
        0x0A8, 0x780, 0x7A0, 0x7A8, 0x7AC, 0x0AC, 0x7B0, 0x7B4,
        0x7B8, 0x7BC, 0x7C0, 0x7D0, 0x7D4, 0x7D8, 0x7DC, 0x7E0,
        0x7E8, 0x7EC, 0x7F0, 0x7F4, 0x7F8, 0x7FC, 0x800, 0xA00,
        0x004, 0x1C0, 0x1FC, 0x2D8, 0x2A8, 0x2A0, 0x200, 0x1BC,
        0x1F4, 0x1F0, 0x2B4, 0x1E0, 0x0A0, 0x1D8, 0x1D4, 0x1E8,
        0x0E0, 0x0F0, 0x140, 0x154, 0x174, 0x160, 0x16C, 0x1A0,
        0x180, 0x1AC, 0x1EC, 0x1F8, 0x2C0, 0x1DC, 0x2BC, 0x1D0,
        0x280, 0x0BC, 0x100, 0x0D4, 0x0DC, 0x0B8, 0x0F8, 0x150,
        0x158, 0x0C0, 0x1B4, 0x17C, 0x0F4, 0x0E8, 0x0FC, 0x0D0,
        0x0EC, 0x1B0, 0x0D8, 0x0B4, 0x0B0, 0x15C, 0x1A8, 0x168,
        0x170, 0x178, 0x1B8, 0x2E8, 0x2D0, 0x2EC, 0x2D4, 0x2B0,
        0x2AC, 0x014, 0x060, 0x038, 0x034, 0x008, 0x050, 0x058,
        0x030, 0x018, 0x080, 0x070, 0x02C, 0x040, 0x01C, 0x010,
        0x054, 0x078, 0x020, 0x028, 0x00C, 0x03C, 0x06C, 0x068,
        0x074, 0x05C, 0x07C, 0x2DC, 0x2B8, 0x2E0, 0x2F0, 0xA80,
        0xAA0, 0xAA8, 0xAAC, 0xAB0, 0xAB4, 0xAB8, 0xABC, 0xAC0,
        0xAD0, 0xAD4, 0xAD8, 0xADC, 0xAE0, 0xAE8, 0xAEC, 0xAF0,
        0xAF4, 0xAF8, 0xAFC, 0xB00, 0xB40, 0xB50, 0xB54, 0xB58,
        0xB5C, 0xB60, 0xB68, 0xB6C, 0xB70, 0xB74, 0xB78, 0xB7C,
        0x2F4, 0x2F8, 0x2FC, 0x300, 0x340, 0x350, 0x354, 0x358,
        0x35C, 0x360, 0x368, 0x36C, 0x370, 0x374, 0x378, 0x37C,
        0x380, 0x3A0, 0x3A8, 0x3AC, 0x3B0, 0x3B4, 0x3B8, 0x3BC,
        0x3C0, 0x3D0, 0x3D4, 0x3D8, 0x3DC, 0x3E0, 0x3E8, 0x3EC,
        0x3F0, 0x3F4, 0x3F8, 0x3FC, 0x400, 0x500, 0x540, 0x550,
        0x554, 0x558, 0x55C, 0x560, 0x568, 0x56C, 0x570, 0x574,
        0x578, 0x57C, 0x580, 0x5A0, 0x5A8, 0x5AC, 0x5B0, 0x5B4,
        0x5B8, 0x5BC, 0x5C0, 0x5D0, 0x5D4, 0x5D8, 0x5DC, 0x5E0,
        0x5E8, 0x5EC, 0x5F0, 0x5F4, 0x5F8, 0x5FC, 0x600, 0x680,
        0x6A0, 0x6A8, 0x6AC, 0x6B0, 0x6B4, 0x6B8, 0x6BC, 0x6C0,
        0x6D0, 0x6D4, 0x6D8, 0x6DC, 0x6E0, 0x6E8, 0x6EC, 0x6F0,
        0x6F4, 0x6F8, 0x6FC, 0x700, 0x740, 0x750, 0x754, 0x758,
    ]
    return {code: chr(ch) for ch, code in enumerate(values) if code != 0}


def parity(value: int) -> int:
    return value.bit_count() & 1


class FullViterbiDecoder:
    def __init__(self, k: int, poly1: int, poly2: int) -> None:
        self.k = k
        self.poly1 = poly1
        self.poly2 = poly2
        self.nstates = 1 << (k - 1)
        self.traceback = min(k * 12, 64)
        self.chunksize = 8
        self.step = 0
        self.pair_count = 0
        self.metrics = np.zeros(self.nstates, dtype=np.int16)
        self.decisions = np.zeros((64, self.nstates), dtype=np.bool_)
        states = np.arange(self.nstates, dtype=np.uint32)
        self.p0 = states >> 1
        self.p1 = (states + self.nstates) >> 1
        self.o0 = np.array([parity(int(s) & poly1) | (parity(int(s) & poly2) << 1) for s in states], dtype=np.uint8)
        self.o1 = np.array([parity(int(s + self.nstates) & poly1) | (parity(int(s + self.nstates) & poly2) << 1) for s in states], dtype=np.uint8)

    @staticmethod
    def branch(outputs: np.ndarray, b0: int | None, b1: int | None) -> np.ndarray:
        metric = np.zeros(outputs.shape, dtype=np.int16)
        if b0 is not None:
            b0s = b0 if b0 > 1 else (255 if b0 else 0)
            metric += np.where((outputs & 1) != 0, b0s - 128, 128 - b0s).astype(np.int16)
        if b1 is not None:
            b1s = b1 if b1 > 1 else (255 if b1 else 0)
            metric += np.where(((outputs >> 1) & 1) != 0, b1s - 128, 128 - b1s).astype(np.int16)
        return metric

    def traceback_bits(self) -> list[int]:
        state = int(np.argmax(self.metrics))
        seq = [0] * (self.traceback + 1)
        seq[self.traceback] = state
        step = (self.step - 1) % 64
        for i in range(self.traceback - 1, -1, -1):
            decision = bool(self.decisions[step, state])
            state = (state + (self.nstates if decision else 0)) >> 1
            seq[i] = state
            step = (step - 1) % 64
        return [seq[i] & 1 for i in range(self.chunksize)]

    def decode_pair(self, b0: int | None, b1: int | None) -> list[int]:
        m0 = self.metrics[self.p0] + self.branch(self.o0, b0, b1)
        m1 = self.metrics[self.p1] + self.branch(self.o1, b0, b1)
        decision = m1 >= m0
        self.metrics = np.where(decision, m1, m0).astype(np.int16)
        mx = int(self.metrics.max())
        if mx > 12000:
            self.metrics = (self.metrics - mx).astype(np.int16)
        self.decisions[self.step, :] = decision
        self.step = (self.step + 1) % 64
        self.pair_count += 1
        if self.pair_count % self.chunksize == 0 and self.pair_count > self.traceback:
            return self.traceback_bits()
        return []


class Interleave:
    def __init__(self, size: int, depth: int, fill: int | None = PUNCTURE) -> None:
        self.size = size
        self.depth = depth
        self.table = [[[fill for _ in range(size)] for _ in range(size)] for _ in range(depth)]

    def symbols_rev(self, syms: list[int | None]) -> list[int | None]:
        syms = list(syms)
        for k in range(self.depth):
            for i in range(self.size):
                row = self.table[k][i]
                for j in range(self.size - 1):
                    row[j] = row[j + 1]
                row[self.size - 1] = syms[i]
            syms = [self.table[k][i][i] for i in range(self.size)]
        return syms


def read_wav_mono(path: Path) -> tuple[int, np.ndarray]:
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        width = wav.getsampwidth()
        rate = wav.getframerate()
        frames = wav.getnframes()
        raw = wav.readframes(frames)
    if width != 2:
        raise RuntimeError(f"{path.name}: only 16-bit PCM WAV is supported")
    data = np.frombuffer(raw, dtype="<i2").astype(np.float64)
    if channels > 1:
        data = data.reshape(-1, channels).mean(axis=1)
    return rate, data


def find_signal_start(samples: np.ndarray, rate: int) -> int:
    window = max(128, int(rate * 0.02))
    if len(samples) < window:
        return 0
    energy = np.convolve(np.abs(samples), np.ones(window) / window, mode="valid")
    threshold = max(200.0, float(np.max(energy)) * 0.05)
    hits = np.flatnonzero(energy > threshold)
    if hits.size == 0:
        return 0
    return int(hits[0])


def bits_to_text(bits: list[int], decode: dict[int, str]) -> str:
    shreg = 0
    out: list[str] = []
    for bit in bits:
        shreg = ((shreg << 1) | (1 if bit else 0)) & 0xFFFF
        if (shreg & 0x3) == 0:
            code = shreg >> 2
            ch = decode.get(code)
            if ch is not None:
                out.append(ch)
            shreg = 0
    return "".join(out)


def mfsk_bits_to_text(bits: list[int], decode: dict[int, str]) -> str:
    shreg = 1
    out: list[str] = []
    for bit in bits:
        shreg = ((shreg << 1) | (1 if bit else 0)) & 0xFFFF
        if (shreg & 0x7) == 1:
            ch = decode.get(shreg >> 1)
            if ch is not None and ch != "\x00":
                out.append(ch)
            shreg = 1
    return "".join(out)


def nearest_phase_index(angle: float, count: int) -> int:
    step = 2.0 * math.pi / count
    return int((angle / step) + 0.5) & (count - 1)


def inverse_symbol(mode: ModeInfo, angle: float) -> int:
    if mode.family == "BPSK":
        return 2 if math.cos(angle) >= 0.0 else 0
    if mode.family == "QPSK":
        idx = nearest_phase_index(angle, 4)
        return {2: 0, 3: 1, 0: 2, 1: 3}[idx]
    if mode.family == "8PSK" and mode.fec:
        idx = nearest_phase_index(angle, 8)
        return {0: 0, 7: 1, 5: 2, 6: 3, 1: 4, 2: 5, 4: 6, 3: 7}[idx]
    if mode.family == "8PSK":
        idx = nearest_phase_index(angle, 8)
        return {4: 0, 3: 1, 2: 2, 1: 3, 0: 4, 7: 5, 6: 6, 5: 7}[idx]
    return 0


def demod_symbols(samples: np.ndarray, rate: int, mode: ModeInfo, tone_hz: float) -> list[int]:
    sps = rate / mode.baud
    start = find_signal_start(samples, rate)
    start = int((start // sps) * sps)
    if mode.carriers > 1:
        win0, win1 = 0.45, 0.95
    elif mode.name == "QPSK31":
        win0, win1 = 0.25, 0.75
    else:
        win0, win1 = 0.62, 0.96
    tail0 = int(sps * win0)
    tail1 = max(tail0 + 1, int(sps * win1))
    spacing = 1.4 * mode.baud
    first = tone_hz + ((-1 * mode.carriers) + 1) * spacing / 2.0
    prev: list[complex | None] = [None] * mode.carriers
    out: list[int] = []

    pos = float(start)
    while int(pos + sps) < len(samples):
        base = int(pos)
        n = np.arange(base + tail0, base + tail1, dtype=np.float64)
        if n.size == 0:
            break
        block = samples[base + tail0 : base + tail1]
        frame_symbols: list[int] = []
        good = True
        for car in range(mode.carriers):
            freq = first + car * spacing
            symbol = complex(np.dot(block, np.exp(-1j * 2.0 * math.pi * freq * n / rate)))
            if abs(symbol) < 1.0:
                prev[car] = None
                good = False
                continue
            if prev[car] is None:
                prev[car] = symbol
                good = False
                continue
            diff = symbol * prev[car].conjugate()
            angle = math.atan2(diff.imag, diff.real)
            if angle < 0.0:
                angle += 2.0 * math.pi
            idx_count = 4 if mode.family == "QPSK" else (8 if mode.family == "8PSK" else 2)
            idx = nearest_phase_index(angle, idx_count)
            frame_symbols.append(inverse_symbol(mode, angle) | (idx << 4))
            prev[car] = symbol
        if good and len(frame_symbols) == mode.carriers:
            out.extend(frame_symbols)
        pos += sps
    return out


def fec_params(mode: ModeInfo) -> tuple[int, int, int]:
    if mode.family == "QPSK":
        return 5, 0x17, 0x19
    if mode.name == "BPSK63F" or mode.name in ("8PSK1000F", "8PSK1200F"):
        return 7, 0x6D, 0x4F
    if mode.name in ("8PSK125F", "8PSK250F"):
        return 16, 0o152711, 0o126723
    if mode.name == "8PSK500F":
        return 13, 0o16461, 0o12767
    return 7, 0x6D, 0x4F


def interleave_depth(mode: ModeInfo) -> int:
    if mode.name == "8PSK125F":
        return 384
    if mode.name == "8PSK250F":
        return 512
    if mode.name == "8PSK500F":
        return 640
    if mode.name in ("8PSK1000F", "8PSK1200F"):
        return 512
    return 2


def conv_bits_to_payload(bits: list[int | None], mode: ModeInfo) -> list[int]:
    k, p1, p2 = fec_params(mode)
    dec = FullViterbiDecoder(k, p1, p2)
    deinlv = None if mode.name == "BPSK63F" or mode.family == "QPSK" else Interleave(2, interleave_depth(mode))
    out: list[int] = []
    pair_buf: list[int | None] = []
    for bit in bits:
        pair_buf.append(bit)
        if len(pair_buf) < 2:
            continue
        pair = pair_buf
        pair_buf = []
        if deinlv is not None:
            pair = deinlv.symbols_rev([pair[1], pair[0]])
            pair = [pair[1], pair[0]]
        out.extend(dec.decode_pair(pair[0], pair[1]))
    return out


def decode_symbols(symbols: list[int], mode: ModeInfo, psk_decode: dict[int, str], mfsk_decode: dict[int, str]) -> str:
    if mode.family == "BPSK" and not mode.fec:
        bits = [1 if (sym & 0x0f) == 2 else 0 for sym in symbols]
        return bits_to_text(bits, psk_decode)
    if mode.family == "BPSK" and mode.fec:
        enc_bits = [255 if (sym & 0x0f) == 2 else 0 for sym in symbols]
        payload = conv_bits_to_payload(enc_bits, mode)
        return mfsk_bits_to_text(payload, mfsk_decode)
    if mode.family == "QPSK":
        enc_bits: list[int | None] = []
        for sym in symbols:
            sym &= 0x0f
            enc_bits.extend([sym & 1, (sym >> 1) & 1])
        payload = conv_bits_to_payload(enc_bits, mode)
        return bits_to_text(payload, psk_decode)
    if mode.family == "8PSK" and not mode.fec:
        bits: list[int] = []
        for sym in symbols:
            sym &= 0x0f
            bits.extend([sym & 1, (sym >> 1) & 1, (sym >> 2) & 1])
        return mfsk_bits_to_text(bits, mfsk_decode)
    if mode.family == "8PSK":
        soft = (
            (25, 0, 25), (0, 25, 230), (0, 230, 230), (25, 255, 25),
            (230, 255, 25), (255, 230, 230), (255, 25, 230), (230, 0, 25),
        )
        enc_bits2: list[int | None] = []
        for sym in symbols:
            phase_idx = (sym >> 4) & 7
            soft_idx = (-phase_idx) & 7
            enc_bits2.extend([soft[soft_idx][2], soft[soft_idx][1], soft[soft_idx][0]])
            if mode.punctured:
                enc_bits2.append(PUNCTURE)
        payload = conv_bits_to_payload(enc_bits2, mode)
        return mfsk_bits_to_text(payload, mfsk_decode)
    return ""


def decode_single_carrier_bpsk(
    samples: np.ndarray,
    rate: int,
    baud: float,
    tone_hz: float,
    decode: dict[int, str],
) -> str:
    samples_per_symbol = rate / baud
    if abs(samples_per_symbol - round(samples_per_symbol)) > 1.0e-6:
        raise RuntimeError(f"sample rate {rate} is not an integer multiple of baud {baud}")
    sps = int(round(samples_per_symbol))
    start = find_signal_start(samples, rate)
    start = (start // sps) * sps

    # Use the stable end of each shaped PSK symbol. During a bit-0 transition
    # the middle of the symbol crosses through zero, so integrating the tail
    # gives a cleaner differential phase decision.
    tail0 = max(0, int(sps * 0.62))
    tail1 = max(tail0 + 1, int(sps * 0.96))
    bits: list[int] = []
    prev: complex | None = None
    phase_inc = 2.0 * math.pi * tone_hz / rate

    usable = len(samples) - sps
    for pos in range(start, usable, sps):
        block = samples[pos + tail0 : pos + tail1]
        if block.size == 0:
            continue
        n = np.arange(pos + tail0, pos + tail1, dtype=np.float64)
        carrier = np.exp(-1j * phase_inc * n)
        symbol = complex(np.dot(block, carrier))
        if abs(symbol) < 1.0:
            prev = None
            continue
        if prev is not None:
            diff = symbol * prev.conjugate()
            bits.append(1 if diff.real >= 0.0 else 0)
        prev = symbol

    return bits_to_text(bits, decode)


def normalize_text(text: str) -> str:
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    return "".join(ch if ch == "\n" or 32 <= ord(ch) <= 126 else " " for ch in text)


def longest_expected_fragment(decoded: str, expected: str, min_len: int) -> tuple[int, str]:
    decoded = normalize_text(decoded)
    expected = normalize_text(expected)
    best = ""
    max_probe = min(220, len(expected))
    for size in range(max_probe, min_len - 1, -1):
        for start in range(0, max(1, len(expected) - size + 1), max(1, size // 4)):
            fragment = expected[start : start + size]
            if fragment and fragment in decoded:
                return size, fragment
    return len(best), best


def mode_skip_reason(mode: ModeInfo) -> str | None:
    return None


def run_one(
    wav_path: Path,
    mode: ModeInfo,
    expected: str,
    varicode: dict[int, str],
    mfsk_varicode: dict[int, str],
    tone_hz: float,
    min_fragment: int,
) -> tuple[str, str, int, str]:
    reason = mode_skip_reason(mode)
    if reason:
        return "SKIP", reason, 0, ""
    rate, samples = read_wav_mono(wav_path)
    symbols = demod_symbols(samples, rate, mode, tone_hz)
    decoded = decode_symbols(symbols, mode, varicode, mfsk_varicode)
    matched_len, fragment = longest_expected_fragment(decoded, expected, min_fragment)
    if matched_len >= min_fragment:
        return "PASS", f"matched {matched_len} chars, decoded {len(decoded)} chars", matched_len, fragment
    preview = normalize_text(decoded[:160]).replace("\n", "\\n")
    return "FAIL", f"no {min_fragment}-char input.txt fragment found; decoded preview: {preview!r}", 0, ""


def main() -> int:
    parser = argparse.ArgumentParser(description="Run PSK WAV decode regression tests.")
    parser.add_argument("--project", type=Path, default=DEFAULT_PROJECT)
    parser.add_argument("--wav-dir", type=Path, default=DEFAULT_WAV_DIR)
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--tone-hz", type=float, default=DEFAULT_TONE_HZ)
    parser.add_argument("--min-fragment", type=int, default=60)
    parser.add_argument("--mode", action="append", help="Only test this mode name; can be repeated.")
    args = parser.parse_args()

    modes = load_modes(args.project)
    varicode = load_varicode(args.project)
    mfsk_varicode = load_mfsk_varicode()
    expected = args.input.read_text(encoding="utf-8")
    wanted = set(args.mode or [])

    failures = 0
    passed = 0
    skipped = 0

    for wav_path in sorted(args.wav_dir.glob("*.wav")):
        mode_name = wav_path.stem
        if wanted and mode_name not in wanted:
            continue
        mode = modes.get(mode_name)
        if not mode:
            print(f"SKIP {mode_name:11s} not present in firmware mode table")
            skipped += 1
            continue
        try:
            status, detail, _, _ = run_one(
                wav_path=wav_path,
                mode=mode,
                expected=expected,
                varicode=varicode,
                mfsk_varicode=mfsk_varicode,
                tone_hz=args.tone_hz,
                min_fragment=args.min_fragment,
            )
        except Exception as exc:
            status, detail = "FAIL", f"{type(exc).__name__}: {exc}"
        print(f"{status:4s} {mode_name:11s} {detail}")
        if status == "PASS":
            passed += 1
        elif status == "SKIP":
            skipped += 1
        else:
            failures += 1

    print(f"\nsummary: pass={passed} fail={failures} skip={skipped}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
