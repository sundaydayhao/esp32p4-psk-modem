#!/usr/bin/env bash
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
exe="${1:-$root/build/host-tools/host_psk_rtty_wav.exe}"
wav_dir="${2:-/cygdrive/c/projects/psk-script/generated-wav-48000}"
expected="${3:-/cygdrive/c/projects/psk-script/input.txt}"

modes=(
  BPSK31 BPSK63 BPSK63F BPSK125 BPSK250 BPSK500 BPSK1000
  12xPSK125 6xPSK250 2xPSK500 4xPSK500 2xPSK800 2xPSK1000
  QPSK31 QPSK63 QPSK125 QPSK250 QPSK500
  8PSK125 8PSK250 8PSK500 8PSK1000
  8PSK125F 8PSK250F 8PSK500F 8PSK1000F 8PSK1200F
)

pass=0
fail=0
no_wav=0

printf 'EXE=%s\nWAV_DIR=%s\nEXPECTED=%s\n\n' "$exe" "$wav_dir" "$expected"
for mode in "${modes[@]}"; do
  wav="$wav_dir/$mode.wav"
  if [[ ! -f "$wav" ]]; then
    printf '%-10s %-6s %s\n' "$mode" "NO-WAV" "$wav"
    no_wav=$((no_wav + 1))
    continue
  fi

  output="$("$exe" "$mode" "$wav" "$expected" 2>&1)"
  rc=$?
  summary="$(printf '%s\n' "$output" | head -n 1)"
  if [[ $rc -eq 0 ]]; then
    printf '%-10s %-6s %s\n' "$mode" "PASS" "$summary"
    pass=$((pass + 1))
  else
    printf '%-10s %-6s %s\n' "$mode" "FAIL" "$summary"
    fail=$((fail + 1))
  fi
done

printf '\nPASS=%d FAIL=%d NO-WAV=%d TOTAL=%d\n' "$pass" "$fail" "$no_wav" "$((pass + fail + no_wav))"
