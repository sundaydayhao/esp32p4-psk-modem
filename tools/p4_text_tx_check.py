"""P4 TX -> unchanged HPM RX, complete repeated ASCII console-window checks.

Only recognized HPM diagnostic lines are removed, using context outside window
edges. Escaped console errors are retained. This is not a raw channel BER test.
"""
import argparse
import json
import math
import re
import time
from pathlib import Path

from joint_psk_check import frame, port, score

MODES = ("BPSK500", "BPSK1000", "QPSK500", "12xPSK125", "6xPSK250",
         "2xPSK500", "4xPSK500", "2xPSK800", "2xPSK1000", "8PSK250",
         "8PSK500", "8PSK1000", "8PSK125F", "8PSK250F", "8PSK500F",
         "8PSK1000F", "8PSK1200F", "2x8PSK1200F")
DIAGNOSTIC = re.compile(rb"\[(?:diag|carrier)\] [^\r\n]*\r+\n")


def console_window(log, start, end):
    """Use complete log context so partial diagnostics at edges aren't payload."""
    chunks, cursor = [], start
    for match in DIAGNOSTIC.finditer(log):
        if match.end() <= start or match.start() >= end:
            continue
        chunks.append(log[cursor:max(cursor, match.start())])
        cursor = min(end, match.end())
    chunks.append(log[cursor:end])
    # Payload CR/LF are printed as literal backslash+n by HPM. Actual CR/LF
    # belong to its console formatting, so this doesn't erase payload errors.
    return b"".join(chunks).replace(b"\r", b"").replace(b"\n", b"")


def diagnostics(log):
    lines = re.findall(rb"\[diag\] [^\r\n]*", log)
    result = {}
    for name in ("ov", "rxdrop", "txud", "in", "rawq"):
        values = [int(m.group(1)) for line in lines
                  if (m := re.search(rb" " + name.encode() + rb"=(\d+)", line))]
        if values:
            result[name] = {"first": values[0], "last": values[-1], "max": max(values)}
    loads = [float(m.group(1)) for m in re.finditer(rb" load=([\d.]+)%", log)]
    result["max_load_percent"] = max(loads, default=None)
    return result


def summarize_console(clean, expected):
    matches = re.findall(rb"\[BEGIN-P4-[0-9A-F]{4}\].*?\[END-P4-[0-9A-F]{4}\]", clean)
    result = dict(**score(clean, expected), complete_frames=len(matches),
                  incorrect_complete_frames=sum(m != expected for m in matches),
                  has_console_escapes=b"\\" in clean)
    result["diagnostic_contamination"] |= b"[diag] core=" in clean or b"[carrier] c0=" in clean
    result["clean"] = bool(result["all_received_bytes_exact"] and result["exact_frames"] > 0
                           and not result["has_console_escapes"] and not result["diagnostic_contamination"])
    return result


def reanalyze(root):
    """Reproduce scores from saved original logs without operating either board."""
    path = root / "results.json"
    results = json.loads(path.read_text(encoding="utf-8"))
    for row in results:
        mode = row["mode"]
        log = (root / f"{mode}-hpm.log").read_bytes()
        raw = (root / f"{mode}.raw").read_bytes()
        start = log.find(raw) if raw else -1
        if start < 0:
            raise RuntimeError(f"Cannot locate original {mode} window")
        clean = console_window(log, start, start + len(raw))
        row.update(summarize_console(clean, (root / f"{mode}.expected").read_bytes()))
        (root / f"{mode}.console").write_bytes(clean)
        print(json.dumps(row), flush=True)
    path.write_text(json.dumps(results, indent=2), encoding="utf-8")
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tx-port", default="COM21")
    ap.add_argument("--rx-port", default="COM23")
    ap.add_argument("--modes", nargs="+", choices=MODES, default=list(MODES))
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--warmup", type=float, default=15)
    ap.add_argument("--volume", type=int, default=60)
    ap.add_argument("--tone", type=int, default=2000)
    ap.add_argument("--adc", type=int, default=195)
    ap.add_argument("--analog", type=int, default=11)
    ap.add_argument("--squelch", type=int, choices=(0, 1), default=1)
    ap.add_argument("--afc", type=int, choices=(0, 1), default=1)
    ap.add_argument("--rx-decim", type=int, choices=(1, 2, 3, 4), default=1)
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x6A31)
    ap.add_argument("--label", required=True)
    ap.add_argument("--require-clean", action="store_true")
    ap.add_argument("--reanalyze", action="store_true", help="Only rescore saved logs under --label; no serial access")
    args = ap.parse_args()
    if (not re.fullmatch(r"[A-Za-z0-9_-]+", args.label) or not 0 <= args.volume <= 100
            or not 100 <= args.tone <= 3500 or not 0 <= args.adc <= 255
            or not 0 <= args.analog <= 63 or not 0 <= args.seed <= 65535
            or not math.isfinite(args.seconds) or args.seconds < 10
            or not math.isfinite(args.warmup) or args.warmup < 0
            or args.tx_port.upper() == args.rx_port.upper()):
        ap.error("Invalid measurement arguments")
    root = Path(__file__).resolve().parents[1] / "build/p4-text-tx-20260908" / args.label
    if args.reanalyze:
        results = reanalyze(root)
        if args.require_clean and not all(row["clean"] for row in results):
            raise SystemExit(1)
        return
    root.mkdir(parents=True, exist_ok=True)
    ports, logs, results = {}, {"p4": bytearray(), "hpm": bytearray()}, []

    def collect(seconds):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            for name, stream in ports.items():
                if stream.in_waiting:
                    logs[name].extend(stream.read(min(65536, stream.in_waiting)))
            time.sleep(.001)

    def command(name, text, settle=.65):
        offset = len(logs[name])
        encoded = (text + "\n").encode()
        for at in range(0, len(encoded), 8):
            ports[name].write(encoded[at:at + 8])
            collect(.05)
        collect(settle)
        answer = bytes(logs[name][offset:])
        if b"[cmd] error" in answer:
            raise RuntimeError(f"{name} rejected {text}")
        return answer

    def save_logs(prefix):
        for name, log in logs.items():
            (root / f"{prefix}-{name}.log").write_bytes(log)

    try:
        ports["p4"] = port(args.tx_port)
        ports["hpm"] = port(args.rx_port)
        collect(1)
        for name in ports:
            command(name, "STATUS")
        save_logs("initial")
        command("p4", "ROLE RX")
        command("p4", "CONTENT TEXT")
        for text in ("ROLE RX", "LOG OFF", "CONTENT TEXT", "INPUT 2", "LANE AUTO",
                     f"GAIN {args.adc} {args.analog}", "AGC 0", f"AFC {args.afc}",
                     f"SQL {args.squelch}", f"RXDECIM {args.rx_decim}", "LOG TEXT"):
            command("hpm", text)
        for mode in args.modes:
            command("p4", "ROLE RX")
            command("hpm", "MODE " + mode, 1)
            command("hpm", f"TONE {args.tone}", 1)
            for text in ("TXMODE " + mode, f"TXTONE {args.tone}", f"OUTVOL 70 {args.volume}"):
                command("p4", text)
            expected = frame(args.seed)
            (root / f"{mode}.expected").write_bytes(expected)
            echoed = command("p4", "TX " + expected.decode())
            if b"serial control command: TX " + expected not in echoed:
                raise RuntimeError("P4 did not echo the complete requested payload")
            command("p4", "ROLE TX")
            p4 = command("p4", "STATUS")
            hpm = command("hpm", "STATUS")
            if (not re.search(rb" txmode=" + re.escape(mode.encode()) + rb"\s", p4)
                    or not re.search(rb" mode=" + re.escape(mode.encode()) + rb"\s", hpm)
                    or b"role=TX" not in p4 or b"role=RX content=Text" not in hpm):
                raise RuntimeError(f"Mode {mode} not confirmed")
            warmup = max(args.warmup, 35 if mode == "8PSK125F" else 20 if mode in ("8PSK250F", "8PSK500F") else 0)
            print(f"WARM {mode} {warmup:g}s volume={args.volume} tone={args.tone}", flush=True)
            collect(warmup)
            start = len(logs["hpm"])
            began = time.monotonic()
            print(f"RUN {mode} {args.seconds:g}s", flush=True)
            collect(args.seconds)
            elapsed = time.monotonic() - began
            end = len(logs["hpm"])
            collect(.8)  # complete any diagnostic that straddles the window end
            clean = console_window(bytes(logs["hpm"]), start, end)
            window = bytes(logs["hpm"][start:end])
            (root / f"{mode}.raw").write_bytes(window)
            (root / f"{mode}.console").write_bytes(clean)
            row = dict(mode=mode, seconds=elapsed, warmup=warmup, volume=args.volume,
                       tone=args.tone, adc=args.adc, analog=args.analog, seed=args.seed,
                       squelch=args.squelch, afc=args.afc, rx_decim=args.rx_decim,
                       **summarize_console(clean, expected),
                       hpm_diagnostics=diagnostics(window),
                       scope="Whole cyclic ASCII console window, not bit BER; whole-repeat losses not detectable")
            # HPM's console escapes nonprinting errors and cannot preserve exact
            # binary bytes. Keep escapes as errors; never call this raw bit BER.
            command("p4", "STATUS")
            command("hpm", "STATUS")
            save_logs(mode)
            results.append(row)
            (root / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
            print(json.dumps(row), flush=True)
            for log in logs.values():
                log.clear()
    finally:
        for name, text in (("p4", "ROLE RX"), ("hpm", "LOG OFF")):
            if name in ports:
                try:
                    command(name, text)
                except Exception as error:
                    print(f"Cleanup {name}: {error}", flush=True)
        for stream in ports.values():
            stream.close()
        save_logs("final")
    if args.require_clean and not all(row["clean"] for row in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
