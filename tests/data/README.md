# HPM6750 → ESP32-P4 physical audio regression

`hpm_2x8psk1200f_adc.wav` is an 8-second slice (seconds 5 through 13) of a
CRC32-verified 30-second capture taken on 2026-09-08. This is board audio,
not a waveform synthesized by the receiver's own transmitter.

- Source: HPM6750, unmodified commit `c84c683`, COM23, continuous text TX.
- Receiver: ESP32-P4 Function EV Board, COM21, ES7210 input, 10 dB input gain.
- Mode: `2x8PSK1200F`, center 2000 Hz. HPM runtime OUTVOL was `244 122`.
- Mono signed 16-bit PCM, 48000 Hz, already filtered/DC-blocked by P4.
  Replay must not apply the board input filter again.
- Parent capture: 1440000 samples, CRC32 `56baef67`; acquired with the lightweight
  BPSK31 decoder active to keep recording independent of FEC processing load.
- WAV SHA256: `505b8a3b7b508c0215745d9d3eebf75937906199de676488228a5108eaebfd22`.
- Expected cyclic text is stored verbatim in `hpm_2x8psk1200f_expected.txt`.

The fixture begins midway through the TX stream. After acquisition, the bounded
interval from the first BEGIN marker through the last END marker contains exactly
27 complete frames (4212 bytes). Check every byte and that exact frame count;
a matching substring is insufficient. This regression does not measure raw RF
channel BER or certify arbitrary audio levels/noise conditions.

`hpm_2x8psk1200f_clock_adc.wav` is a separate 40-second capture under the same
physical settings, using a different payload (seed `0x5CA9`, expected bytes in
`hpm_2x8psk1200f_clock_expected.txt`). PCM CRC32 is `f08126af`, WAV SHA256 is
`ef4bb5cb28500fd3c82792c01d7645e435c9491245528f743f0a4495bd001ba1`.
The original dual-carrier timing average (alpha 0.2) yields 2 corrupt frames in
the bounded 143-frame interval. The current two-sample baseband average and
alpha 0.05 yield all 143 frames / 22308 bytes exact and also pass the first
independent recording. Keep the full 40 seconds:
the failing frames depend on clock history, not just their immediate waveform.
