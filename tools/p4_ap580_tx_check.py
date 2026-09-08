"""Verify live AP580 -> P4 -> audio cable -> HPM reception; never flashes HPM.

AP580 must already be physically configured for 2400 bit/s. UART is 115200.
Counter deltas assess transport health, not bit-exact audio or audible quality.
"""
import argparse
import json
import re
import time
from pathlib import Path

import serial


def open_port(name):
    port = serial.Serial(port=None, baudrate=115200, timeout=0.01)
    port.dtr = False
    port.rts = False
    port.port = name
    port.open()
    return port


def p4_status(data):
    text = data.decode(errors="replace")
    content = re.findall(r"content=Audio epoch=(\d+) image=\d+/96 bad=\d+ voice=\d+ packets=\d+ crc=\d+ UART=(\w+) in=(\d+) out=(\d+)", text)
    ap = re.findall(r"ap580 tx_packets=(\d+) tx_bytes=(\d+) pending=(\d+) drops=(\d+) uart_bytes=(\d+) badlen=(\d+) syncdrop=(\d+) nonvoice=(\d+) timeouts=(\d+) rate=(\d+)", text)
    if not content or not ap:
        raise RuntimeError("Missing P4 Audio/AP580 diagnostics; enable UART and build current firmware")
    result = dict(zip(("tx_packets", "tx_bytes", "pending", "drops", "uart_bytes", "badlen", "syncdrop", "nonvoice", "timeouts", "rate"), map(int, ap[-1])))
    result.update(epoch=int(content[-1][0]), ready=content[-1][1] == "ready", frames=int(content[-1][2]))
    return result


def hpm_status(data):
    lines = re.findall(r"\[status\][^\r\n]+", data.decode(errors="replace"))
    if not lines:
        raise RuntimeError("No HPM STATUS response")
    line = lines[-1]
    pattern = (r"aprate=(\d+) recover:(\d+) reacq:(\d+) ap580=uart_rx:(\d+) syncdrop:(\d+) rx:(\d+) non:(\d+) "
               r"badlen:(\d+) badcrc:(\d+) int:(\d+)ms psk:(\d+)/(\d+) rxbytes:(\d+) uarttx:(\d+)/(\d+) q:(\d+)/(\d+) drop:(\d+)")
    match = re.search(pattern, line)
    if not match:
        raise RuntimeError("HPM AP580 counters missing")
    keys = ("rate", "recovery", "reacq", "uart_rx", "syncdrop", "frames", "nonvoice", "badlen", "badcrc", "interval_ms",
            "tx_packets", "tx_bytes", "rx_bytes", "uart_tx_frames", "uart_tx_bytes", "payload_q", "uart_q", "drops")
    result = dict(zip(keys, map(int, match.groups())))
    for key, pattern in {"overflow": r" ov=(\d+)", "rxdrop": r" rxdrop=(\d+)"}.items():
        m = re.search(pattern, line)
        if m:
            result[key] = int(m[1])
    result["audio_rx"] = "role=RX content=Audio" in line
    result["line"] = line
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tx-port", default="COM21")
    parser.add_argument("--rx-port", default="COM23")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--warmup", type=float, default=20)
    parser.add_argument("--volume", type=int, default=60, help="P4 ES8311 output volume, 0..100")
    parser.add_argument("--label", default="live")
    parser.add_argument("--keep-tx", action="store_true", help="Leave P4 Audio/TX running after a completed measurement")
    args = parser.parse_args()
    if not 0 <= args.volume <= 100 or args.seconds < 10 or args.warmup < 0 or not re.fullmatch(r"[A-Za-z0-9_-]+", args.label):
        parser.error("Invalid volume, duration, warmup, or label")
    if args.tx_port == args.rx_port:
        parser.error("P4 and HPM must have different ports")
    root = Path(__file__).resolve().parents[1] / "build" / "ap580-tx-2400" / args.label
    root.mkdir(parents=True, exist_ok=True)
    ports = {}
    logs = {"p4": bytearray(), "hpm": bytearray()}
    completed = False

    def collect(seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            for name, port in ports.items():
                if port.in_waiting:
                    logs[name].extend(port.read(min(65536, port.in_waiting)))
            time.sleep(0.002)

    def command(name, text, delay=0.5):
        start = len(logs[name])
        data = (text + "\n").encode()
        for off in range(0, len(data), 8):
            ports[name].write(data[off:off + 8])
            collect(0.05)
        collect(delay)
        response = bytes(logs[name][start:])
        if b"[cmd] error" in response:
            raise RuntimeError(f"{name} rejected {text}")
        return response

    def snapshot():
        p4 = p4_status(command("p4", "RXDIAG", 0.7))
        hpm = hpm_status(command("hpm", "STATUS", 0.7))
        return {"time": time.monotonic(), "p4": p4, "hpm": hpm}

    try:
        ports["p4"] = open_port(args.tx_port)
        ports["hpm"] = open_port(args.rx_port)
        collect(0.5)
        command("p4", "ROLE RX")
        for text in ("ROLE RX", "LOG OFF", "CONTENT AUDIO", "APRATE 2400", "APBAUD 115200", "APRECOVERY 0", "TONE 2000"):
            command("hpm", text)
        for text in ("CONTENT AUDIO", "APRATE 2400", "TXTONE 2000", f"OUTVOL 70 {args.volume}"):
            command("p4", text)
        if not p4_status(command("p4", "RXDIAG", 0.7))["ready"]:
            raise RuntimeError("P4 AP580 UART is not enabled")
        command("p4", "ROLE TX")
        print(f"P4 Audio/TX at 2400 bit/s, volume {args.volume}; warmup {args.warmup:g}s", flush=True)
        collect(args.warmup)
        before = snapshot()
        print("start", json.dumps(before, ensure_ascii=True), flush=True)
        remaining = args.seconds
        while remaining > 0:
            interval = min(15, remaining)
            collect(interval)
            remaining -= interval
            print(f"measured {args.seconds - remaining:g}/{args.seconds:g}s", flush=True)
        after = snapshot()
        deltas = {}
        for board in ("p4", "hpm"):
            deltas[board] = {key: value - before[board][key] for key, value in after[board].items()
                             if type(value) is int and type(before[board].get(key)) is int}
        seconds = after["time"] - before["time"]
        problems = []
        if after["p4"]["epoch"] != before["p4"]["epoch"]:
            problems.append("P4 session changed")
        for board in ("p4", "hpm"):
            if after[board]["rate"] != 2400:
                problems.append(f"{board} rate is not 2400")
            if not 40 <= deltas[board]["frames"] / seconds <= 49:
                problems.append(f"{board} voice frame cadence is not near 44.4/s")
        if deltas["p4"]["tx_packets"] <= 0 or deltas["p4"]["tx_bytes"] <= 0:
            problems.append("P4 modulator is not consuming voice packets")
        for board, keys in (("p4", ("drops", "badlen", "syncdrop", "nonvoice", "timeouts")),
                            ("hpm", ("drops", "badcrc", "badlen", "reacq", "overflow", "rxdrop"))):
            for key in keys:
                if deltas[board].get(key, 0):
                    problems.append(f"{board} {key} increased by {deltas[board][key]}")
        if not after["hpm"]["audio_rx"] or after["hpm"]["recovery"]:
            problems.append("HPM must be Audio/RX with CRC recovery disabled")
        if deltas["hpm"]["uart_tx_frames"] < seconds * 40:
            problems.append("HPM is not forwarding speech at the expected cadence")
        if abs(deltas["p4"]["frames"] - deltas["hpm"]["frames"]) > 24:
            problems.append("TX/RX frame count gap exceeds snapshot and pipeline tolerance")
        report = {"before": before, "after": after, "deltas": deltas, "seconds": seconds, "problems": problems,
                  "passed": not problems, "volume": args.volume,
                  "unavailable_hpm_counters": [key for key in ("overflow", "rxdrop") if key not in after["hpm"]],
                  "scope": "Counter-based hardware transport check; not bit-exact audio or listening confirmation"}
        (root / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps(report, indent=2), flush=True)
        completed = True
    finally:
        if "p4" in ports and not (completed and args.keep_tx):
            try:
                command("p4", "ROLE RX")
            except (serial.SerialException, RuntimeError) as error:
                print(f"Could not stop P4 TX during cleanup: {error}", flush=True)
        for name, port in ports.items():
            port.close()
            (root / f"{name}.log").write_bytes(logs[name])
    if problems:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
