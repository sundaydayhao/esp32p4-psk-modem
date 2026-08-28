import argparse
import importlib
import re
import sys
import threading
import time
import wave
import winsound
from pathlib import Path
from tempfile import TemporaryDirectory

import serial

IDF_PYSERIAL_SITE = Path(r"C:\Espressif\tools\python\v5.5.5\venv\Lib\site-packages")
if not hasattr(serial, "Serial") and IDF_PYSERIAL_SITE.exists():
    sys.path.insert(0, str(IDF_PYSERIAL_SITE))
    sys.modules.pop("serial", None)
    serial = importlib.import_module("serial")


DEFAULT_MODES = [
    "BPSK63",
    "BPSK125",
    "BPSK250",
    "BPSK500",
    "BPSK1000",
    "QPSK63",
    "QPSK125",
    "QPSK250",
    "QPSK500",
]

ALL_BPSK_QPSK = [
    "BPSK31",
    "BPSK63",
    "BPSK63F",
    "BPSK125",
    "BPSK250",
    "BPSK500",
    "BPSK1000",
    "QPSK31",
    "QPSK63",
    "QPSK125",
    "QPSK250",
    "QPSK500",
]

ALL_PSK = [
    "BPSK31",
    "BPSK63",
    "BPSK63F",
    "BPSK125",
    "BPSK250",
    "BPSK500",
    "BPSK1000",
    "12xPSK125",
    "6xPSK250",
    "2xPSK500",
    "4xPSK500",
    "2xPSK800",
    "2xPSK1000",
    "QPSK31",
    "QPSK63",
    "QPSK125",
    "QPSK250",
    "QPSK500",
    "8PSK125",
    "8PSK250",
    "8PSK500",
    "8PSK1000",
    "8PSK125F",
    "8PSK250F",
    "8PSK500F",
    "8PSK1000F",
    "8PSK1200F",
]


def normalize_text(text: str) -> str:
    out = []
    in_space = True
    for ch in text:
        code = ord(ch)
        if ch.isspace():
            if not in_space:
                out.append(" ")
                in_space = True
        elif 32 <= code < 127:
            out.append(ch)
            in_space = False
    return "".join(out).strip()


def strip_logs(text: str) -> str:
    text = re.sub(r"[EWIDV] \(\d+\) [^\r\n]*(?:\r?\n)?", "\n", text)
    kept = []
    for line in text.replace("\r", "\n").split("\n"):
        if re.match(r"^[EWIDV] \(\d+\) ", line):
            continue
        if line.startswith(("ESP-ROM:", "Build:", "rst:", "Saved PC:", "SPIWP:", "mode:", "load:", "entry ")):
            continue
        if "serial mode command:" in line or "psk-modem" in line:
            continue
        kept.append(line)
    return "\n".join(kept)


def levenshtein(a: str, b: str) -> int:
    if len(a) > len(b):
        a, b = b, a
    prev = list(range(len(a) + 1))
    for j, cb in enumerate(b, 1):
        curr = [j]
        for i, ca in enumerate(a, 1):
            curr.append(min(prev[i] + 1, curr[i - 1] + 1, prev[i - 1] + (ca != cb)))
        prev = curr
    return prev[-1]


def bit_count(value: int) -> int:
    value &= 0xff
    count = 0
    while value:
        value &= value - 1
        count += 1
    return count


def best_error_stats(decoded: str, expected: str) -> dict:
    decoded = normalize_text(decoded)
    expected = normalize_text(expected)
    period = expected + " "
    stats = {
        "decoded_len": len(decoded),
        "best_offset": -1,
        "compared_chars": 0,
        "char_errors": 0,
        "bit_errors": 0,
        "total_bits": 0,
        "cer": 1.0,
        "ber": 1.0,
    }
    if not decoded:
        return stats

    def eval_offset(offset: int) -> tuple[int, int, int, int]:
        char_errors = 0
        bit_errors = 0
        plen = len(period)
        for i, ch in enumerate(decoded):
            expected_byte = ord(period[(offset + i) % plen]) & 0xff
            decoded_byte = ord(ch) & 0xff
            if expected_byte != decoded_byte:
                char_errors += 1
                bit_errors += bit_count(expected_byte ^ decoded_byte)
        total_bits = len(decoded) * 8
        return bit_errors, char_errors, len(decoded), total_bits

    candidates = {0}
    plen = len(period)
    for size in range(min(len(expected), 220), 39, -20):
        found = False
        for start in range(0, max(1, len(expected) - size + 1), max(1, size // 4)):
            pos = decoded.find(expected[start:start + size])
            if pos >= 0:
                base = (start - pos) % plen
                for delta in range(-2, 3):
                    candidates.add((base + delta) % plen)
                found = True
                break
        if found:
            break

    if len(candidates) == 1:
        sample_len = min(len(decoded), 2048)
        sample = decoded[:sample_len]
        sample_scores = []
        for offset in range(plen):
            char_errors = 0
            bit_errors = 0
            for i, ch in enumerate(sample):
                expected_byte = ord(period[(offset + i) % plen]) & 0xff
                decoded_byte = ord(ch) & 0xff
                if expected_byte != decoded_byte:
                    char_errors += 1
                    bit_errors += bit_count(expected_byte ^ decoded_byte)
            sample_scores.append((bit_errors, char_errors, offset))
        for _, _, offset in sorted(sample_scores)[:8]:
            for delta in range(-2, 3):
                candidates.add((offset + delta) % plen)

    best_bit_errors = 10**9
    best_char_errors = 10**9
    best_compared = 0
    best_total_bits = 0
    best_offset = -1
    for offset in sorted(candidates):
        bit_errors, char_errors, compared, total_bits = eval_offset(offset)
        if bit_errors < best_bit_errors or (bit_errors == best_bit_errors and char_errors < best_char_errors):
            best_bit_errors = bit_errors
            best_char_errors = char_errors
            best_compared = compared
            best_total_bits = total_bits
            best_offset = offset
            if bit_errors == 0:
                break

    if best_total_bits:
        stats.update({
            "best_offset": best_offset,
            "compared_chars": best_compared,
            "char_errors": best_char_errors,
            "bit_errors": best_bit_errors,
            "total_bits": best_total_bits,
            "cer": best_char_errors / max(1, max(len(decoded), best_compared)),
            "ber": best_bit_errors / best_total_bits,
        })
    return stats


def has_expected_fragment(decoded: str, expected: str, min_len: int) -> bool:
    decoded = normalize_text(decoded)
    expected = normalize_text(expected)
    if len(decoded) < min_len:
        return False
    for size in range(min(len(expected), 220), min_len - 1, -20):
        for start in range(0, max(1, len(expected) - size + 1), max(1, size // 4)):
            if expected[start:start + size] in decoded:
                return True
    return False


class SerialCapture:
    def __init__(self, port: str, baud: int):
        self.ser = serial.Serial(port, baudrate=baud, timeout=0.05)
        self.lock = threading.Lock()
        self.buf = bytearray()
        self.running = True
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self) -> None:
        while self.running:
            data = self.ser.read(4096)
            if data:
                with self.lock:
                    self.buf.extend(data)

    def clear(self) -> None:
        with self.lock:
            self.buf.clear()
        self.ser.reset_input_buffer()

    def snapshot(self) -> bytes:
        with self.lock:
            return bytes(self.buf)

    def write_line(self, text: str) -> None:
        self.ser.write((text + "\r").encode("ascii"))
        self.ser.flush()

    def close(self) -> None:
        self.running = False
        self.thread.join(timeout=1.0)
        self.ser.close()


def wav_duration(path: Path) -> float:
    with wave.open(str(path), "rb") as wav:
        return wav.getnframes() / float(wav.getframerate())


def make_wav_clip(src: Path, dst: Path, offset_seconds: float, max_seconds: float, gain: float) -> float:
    with wave.open(str(src), "rb") as inp:
        rate = inp.getframerate()
        total_frames = inp.getnframes()
        start = max(0, min(total_frames, int(offset_seconds * rate)))
        frames = total_frames - start
        if max_seconds > 0:
            frames = min(frames, int(max_seconds * rate))
        inp.setpos(start)
        data = inp.readframes(frames)
        if gain != 1.0:
            samples = bytearray(data)
            for i in range(0, len(samples), 2):
                value = int.from_bytes(samples[i:i + 2], "little", signed=True)
                scaled = int(round(value * gain))
                if scaled > 32767:
                    scaled = 32767
                elif scaled < -32768:
                    scaled = -32768
                samples[i:i + 2] = int(scaled).to_bytes(2, "little", signed=True)
            data = bytes(samples)
        with wave.open(str(dst), "wb") as out:
            out.setnchannels(inp.getnchannels())
            out.setsampwidth(inp.getsampwidth())
            out.setframerate(rate)
            out.writeframes(data)
    return frames / float(rate)


def play_wav(path: Path, device: int | None, differential_stereo: bool, left_only: bool) -> None:
    if device is None:
        winsound.PlaySound(str(path), winsound.SND_FILENAME)
        return

    import numpy as np
    import sounddevice as sd

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
        mono = audio.reshape(-1, 1)
        audio = np.repeat(mono, 2, axis=1)

    if differential_stereo:
        mono = audio[:, 0].astype(np.int16)
        audio = np.column_stack((mono, np.clip(-mono.astype(np.int32), -32768, 32767).astype(np.int16)))
    elif left_only:
        mono = audio[:, 0].astype(np.int16)
        audio = np.column_stack((mono, np.zeros_like(mono)))

    sd.play(audio, samplerate=sample_rate, device=device, blocking=True)
    sd.stop()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM14")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--wav-dir", type=Path, default=Path(r"C:\projects\psk-script\generated-wav-48000"))
    parser.add_argument("--expected", type=Path, default=Path(r"C:\projects\psk-script\input.txt"))
    parser.add_argument("--mode", action="append")
    parser.add_argument("--all-bpsk-qpsk", action="store_true")
    parser.add_argument("--all-psk", action="store_true")
    parser.add_argument("--settle", type=float, default=3.0)
    parser.add_argument("--tail", type=float, default=4.0)
    parser.add_argument("--offset", type=float, default=0.0)
    parser.add_argument("--max-seconds", type=float, default=45.0)
    parser.add_argument("--gain", type=float, default=0.25)
    parser.add_argument("--min-fragment", type=int, default=60)
    parser.add_argument("--device", type=int, help="sounddevice output device index; defaults to winsound")
    parser.add_argument("--differential-stereo", action="store_true")
    parser.add_argument("--left-only", action="store_true")
    parser.add_argument("--raw-log-dir", type=Path, help="directory for unfiltered serial logs")
    args = parser.parse_args()

    modes = args.mode or (ALL_PSK if args.all_psk else (ALL_BPSK_QPSK if args.all_bpsk_qpsk else DEFAULT_MODES))
    expected = args.expected.read_text(encoding="utf-8")
    if args.raw_log_dir:
        args.raw_log_dir.mkdir(parents=True, exist_ok=True)

    cap = SerialCapture(args.port, args.baud)
    try:
        time.sleep(2.0)
        with TemporaryDirectory(prefix="psk_hw_wav_") as tmp:
            tmp_dir = Path(tmp)
            results = []
            for mode in modes:
                wav_path = args.wav_dir / f"{mode}.wav"
                if not wav_path.exists():
                    results.append((mode, "NO-WAV", None, ""))
                    print(f"{mode:9s} NO-WAV {wav_path}", flush=True)
                    continue

                print(f"{mode:9s} set-mode", flush=True)
                cap.clear()
                cap.write_line(f"@{mode}")
                time.sleep(args.settle)
                cap.clear()

                full_dur = wav_duration(wav_path)
                clip_path = tmp_dir / f"{mode}.wav"
                clip_dur = make_wav_clip(wav_path, clip_path, args.offset, args.max_seconds, args.gain)
                print(f"{mode:9s} play {clip_dur:.1f}s/{full_dur:.1f}s offset={args.offset:.1f}s gain={args.gain:.2f}", flush=True)
                play_wav(clip_path, args.device, args.differential_stereo, args.left_only)
                time.sleep(args.tail)

                raw_bytes = cap.snapshot()
                if args.raw_log_dir:
                    (args.raw_log_dir / f"{mode}.log").write_bytes(raw_bytes)
                raw = raw_bytes.decode("utf-8", errors="ignore")
                decoded = strip_logs(raw)
                stats = best_error_stats(decoded, expected)
                ok = has_expected_fragment(decoded, expected, args.min_fragment)
                status = "PASS" if ok else "FAIL"
                preview = normalize_text(decoded)[:120]
                results.append((mode, status, stats, preview))
                print(
                    f"{mode:9s} {status:4s} BER={stats['ber'] * 100:6.2f}% "
                    f"CER={stats['cer'] * 100:6.2f}% decoded={stats['decoded_len']} "
                    f"compared={stats['compared_chars']} preview={preview!r}",
                    flush=True,
                )

        print("\nSUMMARY")
        for mode, status, stats, preview in results:
            if stats is None:
                print(f"{mode:9s} {status:6s}")
            else:
                print(
                    f"{mode:9s} {status:6s} BER={stats['ber'] * 100:6.2f}% "
                    f"CER={stats['cer'] * 100:6.2f}% decoded={stats['decoded_len']} "
                    f"compared={stats['compared_chars']}"
                )
        return 0 if all(r[1] in ("PASS", "NO-WAV") for r in results) else 1
    finally:
        cap.close()


if __name__ == "__main__":
    raise SystemExit(main())
