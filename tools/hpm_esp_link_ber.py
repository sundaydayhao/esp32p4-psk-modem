#!/usr/bin/env python3
"""Run HPM6750 TX -> ESP32-P4 RX PSK BER statistics over two serial ports."""

from __future__ import annotations

import argparse
import math
import re
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import BinaryIO

import serial

IDF_PYSERIAL_SITE = Path(r"C:\Espressif\tools\python\v5.5.5\venv\Lib\site-packages")
if not hasattr(serial, "Serial") and IDF_PYSERIAL_SITE.exists():
    sys.path.insert(0, str(IDF_PYSERIAL_SITE))
    sys.modules.pop("serial", None)
    serial = __import__("serial")


@dataclass(frozen=True)
class ModeSpec:
    name: str
    baud: float
    bits_per_symbol: int
    carriers: int
    dcd_symbols: int
    fec: bool = False


MODES = (
    ModeSpec("BPSK31", 31.25, 1, 1, 32),
    ModeSpec("BPSK63", 62.5, 1, 1, 64),
    ModeSpec("BPSK63F", 62.5, 1, 1, 64, True),
    ModeSpec("BPSK125", 125.0, 1, 1, 128),
    ModeSpec("BPSK250", 250.0, 1, 1, 256),
    ModeSpec("BPSK500", 500.0, 1, 1, 512),
    ModeSpec("BPSK1000", 1000.0, 1, 1, 128),
    ModeSpec("12xPSK125", 125.0, 1, 12, 128),
    ModeSpec("6xPSK250", 250.0, 1, 6, 512),
    ModeSpec("2xPSK500", 500.0, 1, 2, 512),
    ModeSpec("4xPSK500", 500.0, 1, 4, 512),
    ModeSpec("2xPSK800", 800.0, 1, 2, 512),
    ModeSpec("2xPSK1000", 1000.0, 1, 2, 1024),
    ModeSpec("QPSK31", 31.25, 2, 1, 32, True),
    ModeSpec("QPSK63", 62.5, 2, 1, 64, True),
    ModeSpec("QPSK125", 125.0, 2, 1, 128, True),
    ModeSpec("QPSK250", 250.0, 2, 1, 256, True),
    ModeSpec("QPSK500", 500.0, 2, 1, 512, True),
    ModeSpec("8PSK125", 125.0, 3, 1, 128),
    ModeSpec("8PSK250", 250.0, 3, 1, 256),
    ModeSpec("8PSK500", 500.0, 3, 1, 512),
    ModeSpec("8PSK1000", 1000.0, 3, 1, 1024),
    ModeSpec("8PSK125F", 125.0, 3, 1, 128, True),
    ModeSpec("8PSK250F", 250.0, 3, 1, 256, True),
    ModeSpec("8PSK500F", 500.0, 3, 1, 512, True),
    ModeSpec("8PSK1000F", 1000.0, 3, 1, 1024, True),
    ModeSpec("8PSK1200F", 16000.0 / 13.0, 3, 1, 2048, True),
)
MODE_BY_NAME = {mode.name.lower(): mode for mode in MODES}

LOG_FRAGMENT_RE = re.compile(
    r"(?:^|\n)# (?:TX|RX): [^\n]*(?:\n|$)|"
    r"[EWIDV] \(\d+\) [^\n]*(?:\n|$)|"
    r"(?:ESP-ROM:|Build:|rst:|Saved PC:|SPIWP:|mode:|load:|entry )[^\n]*(?:\n|$)|"
    r"\[(?:cmd|status|role|mode|txmode|tx|tone|gain|cap|capdump|diag|peaks|boot|touch|rx|txtext|fault)\]"
    r"[^\n]*(?:\n|$)"
)


@dataclass
class Stats:
    decoded_len: int
    ref_chars: int
    char_errors: int
    cer: float
    ref_bits: int
    bit_errors: int
    ber: float
    decoded_start: int
    decoded_end: int
    substitutions: int
    insertions: int
    deletions: int


@dataclass
class Result:
    mode: str
    elapsed: float
    stats: Stats
    passed: bool
    detail: str


class Port:
    def __init__(self, name: str, port_name: str, baud: int, log_file: BinaryIO) -> None:
        self.name = name
        self.port = serial.Serial(port_name, baudrate=baud, timeout=0.05, write_timeout=2.0)
        self.log_file = log_file

    def marker(self, text: str) -> None:
        self.log_file.write(f"\r\n# {self.name}: {text}\r\n".encode("ascii", errors="replace"))
        self.log_file.flush()

    def read(self) -> bytes:
        waiting = self.port.in_waiting
        data = self.port.read(waiting if waiting > 0 else 1)
        if data:
            self.log_file.write(data)
            self.log_file.flush()
        return data

    def drain(self, seconds: float) -> bytes:
        deadline = time.monotonic() + seconds
        chunks: list[bytes] = []
        while time.monotonic() < deadline:
            chunk = self.read()
            if chunk:
                chunks.append(chunk)
            else:
                time.sleep(0.02)
        return b"".join(chunks)

    def clear(self) -> None:
        self.port.reset_input_buffer()

    def send(self, command: str) -> None:
        self.marker(f"> {command}")
        data = (command + "\r\n").encode("ascii", errors="replace")
        for offset in range(0, len(data), 8):
            self.port.write(data[offset:offset + 8])
            self.port.flush()
            if offset + 8 < len(data):
                time.sleep(0.05)

    def wait_for(self, pattern: re.Pattern[str], timeout: float, command: str | None = None) -> tuple[bool, str]:
        deadline = time.monotonic() + timeout
        response = ""
        next_send = 0.0
        while time.monotonic() < deadline:
            now = time.monotonic()
            if command and now >= next_send:
                self.send(command)
                next_send = now + 0.75
            chunk = self.read()
            if chunk:
                response += chunk.decode("ascii", errors="replace")
                if pattern.search(response):
                    return True, response
                if len(response) > 32768:
                    response = response[-32768:]
            else:
                time.sleep(0.02)
        return False, response

    def close(self) -> None:
        if self.port.is_open:
            self.port.close()


def normalize_text(text: str) -> str:
    text = text.replace("\r", "\n")
    text = re.sub(r"\s+", " ", text)
    return "".join(ch for ch in text if 32 <= ord(ch) < 127).strip()


def convert_escapes(text: str) -> str:
    out: list[str] = []
    i = 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            if text[i + 1] in ("r", "n"):
                out.append(" ")
                i += 2
                continue
            if text[i + 1] == "x" and i + 3 < len(text):
                try:
                    out.append(chr(int(text[i + 2:i + 4], 16)))
                    i += 4
                    continue
                except ValueError:
                    pass
        out.append(text[i])
        i += 1
    return "".join(out)


def extract_decoded(raw: bytes) -> str:
    decoded = raw.decode("utf-8", errors="ignore").replace("\r\n", "\n").replace("\r", "\n")
    decoded = LOG_FRAGMENT_RE.sub("", decoded)
    return normalize_text(convert_escapes(decoded))


def bit_errors_for_pair(a: str, b: str) -> int:
    return ((ord(a) & 0xff) ^ (ord(b) & 0xff)).bit_count()


def best_payload_stats(expected: str, decoded: str) -> Stats:
    expected = normalize_text(expected)
    decoded = normalize_text(decoded)
    n = len(expected)
    m = len(decoded)
    if n == 0:
        return Stats(m, 0, 0, 0.0, 0, 0, 0.0, 0, 0, 0, 0, 0)
    if m == 0:
        return Stats(0, n, n, 1.0, n * 8, n * 8, 1.0, 0, 0, 0, 0, n)

    dp = [[0] * (m + 1) for _ in range(n + 1)]
    parent = [[""] * (m + 1) for _ in range(n + 1)]
    for j in range(1, m + 1):
        dp[0][j] = 0
        parent[0][j] = "skip"
    for i in range(1, n + 1):
        dp[i][0] = i
        parent[i][0] = "del"

    for i in range(1, n + 1):
        ec = expected[i - 1]
        for j in range(1, m + 1):
            dc = decoded[j - 1]
            sub_cost = 0 if ec == dc else 1
            choices = (
                (dp[i - 1][j - 1] + sub_cost, "match" if sub_cost == 0 else "sub"),
                (dp[i - 1][j] + 1, "del"),
                (dp[i][j - 1] + 1, "ins"),
            )
            cost, op = min(choices, key=lambda item: item[0])
            dp[i][j] = cost
            parent[i][j] = op

    decoded_end = min(range(m + 1), key=lambda j: (dp[n][j], abs(j - n), j))
    char_errors = dp[n][decoded_end]
    i = n
    j = decoded_end
    substitutions = 0
    insertions = 0
    deletions = 0
    bit_errors = 0
    while i > 0:
        op = parent[i][j]
        if op == "match":
            i -= 1
            j -= 1
        elif op == "sub":
            substitutions += 1
            bit_errors += bit_errors_for_pair(expected[i - 1], decoded[j - 1])
            i -= 1
            j -= 1
        elif op == "del":
            deletions += 1
            bit_errors += 8
            i -= 1
        elif op == "ins":
            insertions += 1
            bit_errors += 8
            j -= 1
        else:
            break
    decoded_start = j
    ref_bits = n * 8
    return Stats(
        decoded_len=m,
        ref_chars=n,
        char_errors=char_errors,
        cer=char_errors / n,
        ref_bits=ref_bits,
        bit_errors=bit_errors,
        ber=bit_errors / ref_bits,
        decoded_start=decoded_start,
        decoded_end=decoded_end,
        substitutions=substitutions,
        insertions=insertions,
        deletions=deletions,
    )


def parse_modes(values: list[str] | None) -> list[ModeSpec]:
    if not values:
        return list(MODES)
    names: list[str] = []
    for value in values:
        names.extend(item.strip() for item in value.split(",") if item.strip())
    modes: list[ModeSpec] = []
    for name in names:
        mode = MODE_BY_NAME.get(name.lower())
        if not mode:
            raise SystemExit(f"unknown mode {name!r}")
        modes.append(mode)
    return modes


def estimate_timeout(mode: ModeSpec, text_len: int, minimum: int, scale: float) -> int:
    encoded_bits = text_len * 12 * (2 if mode.fec else 1)
    payload_rate = mode.baud * mode.bits_per_symbol * mode.carriers
    payload_seconds = encoded_bits / payload_rate
    preamble_seconds = (2.0 * mode.dcd_symbols) / mode.baud
    return max(minimum, math.ceil((payload_seconds + preamble_seconds) * scale + 5.0))


def configure_link(tx: Port, rx: Port, args: argparse.Namespace) -> None:
    tx.wait_for(re.compile(r"\[tx\]\s+stop requested\b", re.I), 3.0, "TXSTOP")
    rx.send("STOP")
    time.sleep(0.2)
    tx.wait_for(re.compile(r"\[role\]\s+requested\s+TX\b", re.I), 5.0, "ROLE TX")
    rx.send("ROLE RX")
    tx.send(f"TONE {args.tone}")
    rx.send(f"TONE {args.tone}")
    if args.outvol is not None:
        tx.send(f"OUTVOL {args.outvol[0]} {args.outvol[1]}")
    if args.ingain is not None:
        rx.send(f"INGAIN {args.ingain:g}")
    time.sleep(args.settle)


def run_mode(tx: Port, rx: Port, mode: ModeSpec, text: str, args: argparse.Namespace) -> Result:
    timeout = estimate_timeout(mode, len(text), args.min_timeout, args.timeout_scale)
    tx.marker(f"BEGIN {mode.name}")
    rx.marker(f"BEGIN {mode.name}")
    tx.clear()
    rx.clear()

    tx.wait_for(re.compile(r"\[tx\]\s+stop requested\b", re.I), 3.0, "TXSTOP")
    tx.clear()
    rx.clear()
    txmode_pattern = re.compile(rf"\[txmode\]\s+requested\s+{re.escape(mode.name)}\b", re.I)
    txmode_ok, txmode_response = tx.wait_for(txmode_pattern, args.mode_wait, f"TXMODE {mode.name}")
    rx.send(f"RXMODE {mode.name}")
    time.sleep(args.mode_settle)
    if args.fec_branch:
        rx.send(f"FECBRANCH {args.fec_branch}")
        time.sleep(0.25)
    rx_status_pattern = re.compile(rf"\[status\].*\bpsk={re.escape(mode.name)}\b", re.I)
    rxmode_ok, rxmode_response = rx.wait_for(rx_status_pattern, args.mode_wait, "STATUS")
    if not txmode_ok or not rxmode_ok:
        stats = best_payload_stats(text, "")
        detail = "config failed"
        if not txmode_ok:
            detail += f"; TXMODE ack missing tail={txmode_response[-120:]!r}"
        if not rxmode_ok:
            detail += f"; RXMODE status missing tail={rxmode_response[-120:]!r}"
        return Result(mode.name, 0.0, stats, False, detail)
    tx.clear()
    rx.clear()

    tx.send(f"TX {text}")
    queued, tx_response = tx.wait_for(re.compile(rf"\[tx\]\s+queued\s+{re.escape(mode.name)}\b", re.I), args.tx_ack_wait)
    if not queued:
        stats = best_payload_stats(text, "")
        return Result(mode.name, 0.0, stats, False, f"TX queue ack missing tail={tx_response[-120:]!r}")

    started = time.monotonic()
    raw = bytearray()
    best: Stats | None = None
    stable_since: float | None = None
    last_len = -1
    while time.monotonic() - started < timeout:
        chunk = rx.read()
        if chunk:
            raw.extend(chunk)
            decoded = extract_decoded(bytes(raw))
            stats = best_payload_stats(text, decoded)
            best = stats
            if stats.char_errors == 0 and stats.decoded_len >= stats.ref_chars:
                break
            if stats.decoded_len >= stats.ref_chars:
                if stats.decoded_len == last_len:
                    if stable_since is None:
                        stable_since = time.monotonic()
                    elif time.monotonic() - stable_since >= args.post_capture:
                        break
                else:
                    stable_since = time.monotonic()
                    last_len = stats.decoded_len
        else:
            time.sleep(0.02)

    elapsed = time.monotonic() - started
    if args.rxdiag:
        rx.send("RXDIAG")
        rx.drain(args.diag_wait)
    decoded = extract_decoded(bytes(raw))
    if best is None:
        best = best_payload_stats(text, decoded)
    passed = best.char_errors == 0
    detail = (
        f"decoded={best.decoded_len} errors={best.char_errors}/{best.ref_chars} "
        f"bits={best.bit_errors}/{best.ref_bits} ops=s{best.substitutions}/i{best.insertions}/d{best.deletions} "
        f"window={best.decoded_start}:{best.decoded_end}"
    )
    if args.log_dir:
        args.log_dir.mkdir(parents=True, exist_ok=True)
        (args.log_dir / f"{mode.name}.rx.raw.log").write_bytes(bytes(raw))
        (args.log_dir / f"{mode.name}.rx.text.txt").write_text(decoded, encoding="utf-8")
    tx.send("TXSTOP")
    return Result(mode.name, elapsed, best, passed, detail)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tx-port", default="COM20")
    parser.add_argument("--rx-port", default="COM15")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--modes", nargs="+")
    parser.add_argument("--input", type=Path, default=Path("tools/short_regression_input.txt"))
    parser.add_argument("--tone", type=int, default=1500)
    parser.add_argument("--outvol", nargs=2, type=int, metavar=("DAC", "HP"))
    parser.add_argument("--ingain", type=float)
    parser.add_argument("--settle", type=float, default=1.0)
    parser.add_argument("--mode-wait", type=float, default=6.0)
    parser.add_argument("--mode-settle", type=float, default=2.5)
    parser.add_argument("--tx-ack-wait", type=float, default=5.0)
    parser.add_argument("--min-timeout", type=int, default=18)
    parser.add_argument("--timeout-scale", type=float, default=1.35)
    parser.add_argument("--post-capture", type=float, default=1.5)
    parser.add_argument("--rxdiag", action="store_true")
    parser.add_argument("--diag-wait", type=float, default=0.5)
    parser.add_argument("--fec-branch", choices=("AUTO", "0", "1"))
    parser.add_argument("--tx-log", type=Path)
    parser.add_argument("--rx-log", type=Path)
    parser.add_argument("--log-dir", type=Path)
    args = parser.parse_args()

    modes = parse_modes(args.modes)
    text = normalize_text(args.input.read_text(encoding="ascii"))
    if not text:
        raise SystemExit("input text is empty")
    if len(f"TX {text}") > 255:
        raise SystemExit("TX command exceeds firmware serial line limit")

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    if args.log_dir is None:
        args.log_dir = Path("tools") / f"hw-logs-hpm2esp-ber-{stamp}"
    if args.tx_log is None:
        args.tx_log = args.log_dir / "tx.serial.log"
    if args.rx_log is None:
        args.rx_log = args.log_dir / "rx.serial.log"
    args.tx_log.parent.mkdir(parents=True, exist_ok=True)
    args.rx_log.parent.mkdir(parents=True, exist_ok=True)

    results: list[Result] = []
    with args.tx_log.open("ab") as tx_log, args.rx_log.open("ab") as rx_log:
        tx = Port("TX", args.tx_port, args.baud, tx_log)
        rx = Port("RX", args.rx_port, args.baud, rx_log)
        try:
            tx.drain(0.5)
            rx.drain(0.5)
            configure_link(tx, rx, args)
            print(
                f"TX={args.tx_port} RX={args.rx_port} tone={args.tone} "
                f"OUTVOL={'default' if args.outvol is None else f'{args.outvol[0]},{args.outvol[1]}'} "
                f"INGAIN={'default' if args.ingain is None else f'{args.ingain:g}'} "
                f"modes={len(modes)} text_chars={len(text)} log_dir={args.log_dir}",
                flush=True,
            )
            for index, mode in enumerate(modes, 1):
                timeout = estimate_timeout(mode, len(text), args.min_timeout, args.timeout_scale)
                print(f"[{index:02d}/{len(modes):02d}] {mode.name} timeout={timeout}s", flush=True)
                result = run_mode(tx, rx, mode, text, args)
                results.append(result)
                verdict = "PASS" if result.passed else ("HIGH" if result.stats.ber > 0.10 else "WARN")
                print(
                    f"{verdict:<4} {result.mode:<10} elapsed={result.elapsed:5.1f}s "
                    f"CER={result.stats.cer:8.3%} BER={result.stats.ber:9.4%} {result.detail}",
                    flush=True,
                )
        finally:
            try:
                tx.send("TXSTOP")
                tx.drain(0.2)
            finally:
                tx.close()
                rx.close()

    print()
    print(f"{'MODE':<12} {'RESULT':<6} {'SECONDS':>8} {'CER':>9} {'BER':>10} {'CHAR_ERR':>9} {'BIT_ERR':>9}")
    for result in results:
        verdict = "PASS" if result.passed else ("HIGH" if result.stats.ber > 0.10 else "WARN")
        print(
            f"{result.mode:<12} {verdict:<6} {result.elapsed:>8.1f} "
            f"{result.stats.cer:>8.3%} {result.stats.ber:>9.4%} "
            f"{result.stats.char_errors:>4}/{result.stats.ref_chars:<4} "
            f"{result.stats.bit_errors:>4}/{result.stats.ref_bits:<4}"
        )
    passed = sum(1 for result in results if result.passed)
    high = sum(1 for result in results if result.stats.ber > 0.10)
    print(f"SUMMARY PASS={passed} HIGH_BER={high} TOTAL={len(results)} log_dir={args.log_dir}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
