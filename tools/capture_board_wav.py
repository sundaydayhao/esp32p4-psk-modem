import argparse
import importlib
import math
import re
import sys
import threading
import time
import wave
from pathlib import Path

import numpy as np
import sounddevice as sd

import serial

IDF_PYSERIAL_SITE = Path(r"C:\Espressif\tools\python\v5.5.5\venv\Lib\site-packages")
if not hasattr(serial, "Serial") and IDF_PYSERIAL_SITE.exists():
    sys.path.insert(0, str(IDF_PYSERIAL_SITE))
    sys.modules.pop("serial", None)
    serial = importlib.import_module("serial")


def read_wav(path: Path, gain: float) -> tuple[int, np.ndarray]:
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        sample_rate = wav.getframerate()
        frames = wav.readframes(wav.getnframes())

    if sample_width != 2:
        raise ValueError(f"unsupported WAV sample width: {sample_width}")

    audio = np.frombuffer(frames, dtype="<i2")
    if channels > 1:
        audio = audio.reshape(-1, channels)
    else:
        audio = np.repeat(audio.reshape(-1, 1), 2, axis=1)
    if gain != 1.0:
        audio = np.clip(audio.astype(np.float32) * gain, -32768.0, 32767.0).astype(np.int16)
    return sample_rate, audio


def write_wav(path: Path, sample_rate: int, samples: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(samples.astype("<i2").tobytes())


def reader_loop(ser: serial.Serial, chunks: list[bytes], stop: threading.Event) -> None:
    while not stop.is_set():
        data = ser.read(4096)
        if data:
            chunks.append(data)


def parse_capdump(raw: str) -> np.ndarray:
    samples: list[int] = []
    for line in raw.replace("\r", "\n").split("\n"):
        if not line.startswith("[cap] "):
            continue
        _, _, payload = line.partition(":")
        for token in payload.split():
            if not re.fullmatch(r"[0-9a-fA-F]{4}", token):
                continue
            value = int(token, 16)
            if value >= 0x8000:
                value -= 0x10000
            samples.append(value)
    return np.asarray(samples, dtype=np.int16)


def parse_capdump_count(raw: str) -> int | None:
    match = re.search(r"^\[capdump\] samples=(\d+)", raw, flags=re.MULTILINE)
    if not match:
        return None
    return int(match.group(1))


def peak_hz(samples: np.ndarray, sample_rate: int) -> tuple[float, float]:
    if samples.size < 256:
        return 0.0, 0.0
    x = samples.astype(np.float64)
    x -= np.mean(x)
    window = np.hanning(x.size)
    spectrum = np.fft.rfft(x * window)
    mags = np.abs(spectrum)
    if mags.size <= 1:
        return 0.0, 0.0
    idx = int(np.argmax(mags[1:]) + 1)
    return idx * sample_rate / float(x.size), float(mags[idx])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wav", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", default="COM15")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--mode", default="QPSK125")
    parser.add_argument("--tone", type=int, default=1500)
    parser.add_argument("--device", type=int, default=5)
    parser.add_argument("--gain", type=float, default=0.12)
    parser.add_argument("--capture-delay", type=float, default=4.0)
    parser.add_argument("--capture-samples", type=int, default=32768)
    parser.add_argument("--capture-before-play", action="store_true")
    parser.add_argument("--sample-rate", type=int, default=48000)
    args = parser.parse_args()

    sample_rate, audio = read_wav(args.wav, args.gain)
    ser = serial.Serial(args.port, baudrate=args.baud, timeout=0.05)
    chunks: list[bytes] = []
    stop = threading.Event()
    reader = threading.Thread(target=reader_loop, args=(ser, chunks, stop), daemon=True)
    reader.start()
    try:
        time.sleep(1.0)
        ser.reset_input_buffer()
        chunks.clear()
        for command in ("ROLE RX", f"TONE {args.tone}", f"@{args.mode}"):
            ser.write((command + "\r").encode("ascii"))
            ser.flush()
            time.sleep(0.4)
        chunks.clear()

        capture_blocks = max(1, (args.capture_samples + 255) // 256)
        expected_samples = capture_blocks * 256
        if args.capture_before_play:
            ser.write((f"DUMP {capture_blocks}\r").encode("ascii"))
            ser.flush()
            time.sleep(0.2)
        player = threading.Thread(target=sd.play, args=(audio,), kwargs={
            "samplerate": sample_rate,
            "device": args.device,
            "blocking": True,
        }, daemon=True)
        player.start()
        if not args.capture_before_play:
            time.sleep(args.capture_delay)
            ser.write((f"DUMP {capture_blocks}\r").encode("ascii"))
            ser.flush()
        dump_bytes = expected_samples * 6 + (expected_samples // 64 + 4) * 16
        deadline = time.monotonic() + expected_samples / float(args.sample_rate) + \
            max(2.0, dump_bytes * 10.0 / float(args.baud) + 40.0)
        while time.monotonic() < deadline:
            raw_so_far = b"".join(chunks).decode("utf-8", errors="ignore")
            dumped_count = parse_capdump_count(raw_so_far)
            if dumped_count is not None and parse_capdump(raw_so_far).size >= dumped_count:
                break
            time.sleep(0.25)
        sd.stop()
        player.join(timeout=1.0)
    finally:
        stop.set()
        reader.join(timeout=1.0)
        ser.close()

    raw = b"".join(chunks).decode("utf-8", errors="ignore")
    samples = parse_capdump(raw)
    write_wav(args.out, args.sample_rate, samples)

    if samples.size:
        abs_avg = float(np.mean(np.abs(samples.astype(np.float64))))
        rms = float(math.sqrt(np.mean(samples.astype(np.float64) ** 2.0)))
        peak, peak_mag = peak_hz(samples, args.sample_rate)
        print(
            f"captured={samples.size} out={args.out} min={int(samples.min())} "
            f"max={int(samples.max())} abs={abs_avg:.1f} rms={rms:.1f} "
            f"peak={peak:.1f}Hz peak_mag={peak_mag:.1f}"
        )
    else:
        print("captured=0")
        print(raw[-2000:])
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
