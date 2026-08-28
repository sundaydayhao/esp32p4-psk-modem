#!/usr/bin/env python3
"""Capture ESP32-P4 input samples while HPM6750 transmits and print spectrum stats."""

from __future__ import annotations

import argparse
import csv
import math
import re
import sys
import time
import wave
from datetime import datetime
from pathlib import Path

import numpy as np
import serial

IDF_PYSERIAL_SITE = Path(r"C:\Espressif\tools\python\v5.5.5\venv\Lib\site-packages")
if not hasattr(serial, "Serial") and IDF_PYSERIAL_SITE.exists():
    sys.path.insert(0, str(IDF_PYSERIAL_SITE))
    sys.modules.pop("serial", None)
    serial = __import__("serial")


def send_line(port: serial.Serial, command: str) -> None:
    data = (command + "\n").encode("ascii")
    for offset in range(0, len(data), 8):
        port.write(data[offset:offset + 8])
        port.flush()
        if offset + 8 < len(data):
            time.sleep(0.05)


def read_available(port: serial.Serial) -> bytes:
    waiting = port.in_waiting
    return port.read(waiting if waiting > 0 else 1)


def drain(port: serial.Serial, seconds: float) -> bytes:
    deadline = time.monotonic() + seconds
    chunks: list[bytes] = []
    while time.monotonic() < deadline:
        data = read_available(port)
        if data:
            chunks.append(data)
        else:
            time.sleep(0.02)
    return b"".join(chunks)


def parse_capdump(raw: str) -> np.ndarray:
    samples: list[int] = []
    for line in raw.replace("\r", "\n").split("\n"):
        if not line.startswith("[cap] "):
            continue
        _, _, payload = line.partition(":")
        for token in payload.split():
            if re.fullmatch(r"[0-9a-fA-F]{4}", token):
                value = int(token, 16)
                if value >= 0x8000:
                    value -= 0x10000
                samples.append(value)
    return np.asarray(samples, dtype=np.int16)


def parse_capdump_count(raw: str) -> int | None:
    match = re.search(r"^\[capdump\] samples=(\d+)", raw, flags=re.MULTILINE)
    return int(match.group(1)) if match else None


def write_wav(path: Path, samples: np.ndarray, sample_rate: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(samples.astype("<i2").tobytes())


def spectrum_stats(samples: np.ndarray, sample_rate: int, top_n: int) -> tuple[list[tuple[float, float]], dict[str, float]]:
    x = samples.astype(np.float64)
    x -= np.mean(x)
    window = np.hanning(x.size)
    spectrum = np.fft.rfft(x * window)
    mag = np.abs(spectrum)
    freqs = np.fft.rfftfreq(x.size, 1.0 / sample_rate)
    mag[0] = 0.0

    candidates: list[int] = []
    max_bin = int(np.searchsorted(freqs, 3000.0, side="right"))
    for i in range(1, max(1, min(max_bin, mag.size - 1))):
        if mag[i] >= mag[i - 1] and mag[i] >= mag[i + 1]:
            candidates.append(i)
    candidates.sort(key=lambda idx: mag[idx], reverse=True)
    peaks = [(float(freqs[idx]), float(mag[idx])) for idx in candidates[:top_n]]

    total = float(np.sum(mag[1:max_bin] ** 2.0))
    band_stats: dict[str, float] = {}
    for lo, hi in ((0, 500), (500, 1000), (1000, 2000), (2000, 3000), (3000, 6000)):
        lidx = int(np.searchsorted(freqs, float(lo), side="left"))
        hidx = int(np.searchsorted(freqs, float(hi), side="right"))
        energy = float(np.sum(mag[lidx:hidx] ** 2.0))
        band_stats[f"{lo}-{hi}"] = energy / total if total > 0.0 and hi <= 3000 else energy
    return peaks, band_stats


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tx-port", default="COM20")
    parser.add_argument("--rx-port", default="COM15")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--mode", default="BPSK125")
    parser.add_argument("--tone", type=int, default=1500)
    parser.add_argument("--outvol", nargs=2, type=int, metavar=("DAC", "HP"))
    parser.add_argument("--ingain", type=float)
    parser.add_argument("--samples", type=int, default=32768)
    parser.add_argument("--delay", type=float, default=3.0)
    parser.add_argument("--sample-rate", type=int, default=48000)
    parser.add_argument("--text", default="BER TEST FRAME HPM6750 ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 END")
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--top", type=int, default=16)
    args = parser.parse_args()

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = args.out_dir or (Path("tools") / f"spectrum-hpm2esp-{args.mode}-{stamp}")
    out_dir.mkdir(parents=True, exist_ok=True)
    raw_log = out_dir / f"{args.mode}.capdump.log"
    wav_path = out_dir / f"{args.mode}.wav"
    csv_path = out_dir / f"{args.mode}.peaks.csv"

    tx = serial.Serial(args.tx_port, baudrate=args.baud, timeout=0.05, write_timeout=2.0)
    rx = serial.Serial(args.rx_port, baudrate=args.baud, timeout=0.05, write_timeout=2.0)
    try:
        drain(tx, 0.5)
        drain(rx, 0.5)
        for cmd in (
            "TXSTOP",
            "ROLE TX",
            f"TONE {args.tone}",
            *([] if args.outvol is None else [f"OUTVOL {args.outvol[0]} {args.outvol[1]}"]),
            f"TXMODE {args.mode}",
        ):
            send_line(tx, cmd)
            time.sleep(0.25)
        for cmd in (
            "ROLE RX",
            f"TONE {args.tone}",
            *([] if args.ingain is None else [f"INGAIN {args.ingain:g}"]),
            f"RXMODE {args.mode}",
        ):
            send_line(rx, cmd)
            time.sleep(0.25)
        tx.reset_input_buffer()
        rx.reset_input_buffer()

        send_line(tx, f"TX {args.text}")
        time.sleep(args.delay)
        blocks = max(1, (args.samples + 255) // 256)
        expected = blocks * 256
        send_line(rx, f"DUMP {blocks}")

        deadline = time.monotonic() + max(30.0, expected * 70.0 / args.baud + expected / args.sample_rate + 5.0)
        raw = bytearray()
        while time.monotonic() < deadline:
            data = read_available(rx)
            if data:
                raw.extend(data)
                text = raw.decode("utf-8", errors="ignore")
                dumped_count = parse_capdump_count(text)
                if dumped_count is not None and parse_capdump(text).size >= dumped_count:
                    break
            else:
                time.sleep(0.05)
        raw_log.write_bytes(bytes(raw))
    finally:
        try:
            send_line(tx, "TXSTOP")
        except Exception:
            pass
        tx.close()
        rx.close()

    samples = parse_capdump(raw_log.read_text(encoding="utf-8", errors="ignore"))
    if samples.size == 0:
        print(f"captured=0 raw_log={raw_log}")
        return 1
    write_wav(wav_path, samples, args.sample_rate)
    peaks, bands = spectrum_stats(samples, args.sample_rate, args.top)
    with csv_path.open("w", newline="", encoding="ascii") as f:
        writer = csv.writer(f)
        writer.writerow(("rank", "freq_hz", "magnitude"))
        for index, (freq, mag) in enumerate(peaks, 1):
            writer.writerow((index, f"{freq:.3f}", f"{mag:.6f}"))

    x = samples.astype(np.float64)
    clipped = int(np.count_nonzero((samples >= 32760) | (samples <= -32760)))
    print(
        f"mode={args.mode} samples={samples.size} min={int(samples.min())} max={int(samples.max())} "
        f"mean={float(np.mean(x)):.1f} abs={float(np.mean(np.abs(x))):.1f} "
        f"rms={math.sqrt(float(np.mean(x * x))):.1f} clipped={clipped} "
        f"wav={wav_path} peaks={csv_path}"
    )
    print("bands rel_to_0_3000: " + " ".join(
        f"{band}={value:.3f}" for band, value in bands.items() if band != "3000-6000"
    ) + f" abs_3000_6000={bands['3000-6000']:.3e}")
    print("top_peaks: " + " ".join(f"{freq:.1f}Hz:{mag:.3e}" for freq, mag in peaks))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
