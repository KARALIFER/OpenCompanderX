# OCX Type 2 final validation notes

## Compile gate

- The Arduino IDE screenshot shows a successful compile for **Teensy 4.1**.
- The output reports memory usage, which indicates the sketch passed syntax and link stages.

## What was re-checked

- Renamed public-facing project strings from `dbx` / `dbx2` to **OCX Type 2**.
- Kept the firmware on a single universal playback preset.
- Re-checked SGTL5000 setup calls used by the sketch:
  - `inputSelect(AUDIO_INPUT_LINEIN)`
  - `lineInLevel(5)`
  - `lineOutLevel(29)`
  - `headphoneSelect(AUDIO_HEADPHONE_DAC)`
  - `volume(0.45f)`
- Kept the decoder dual-mono, which matches public Type II decode references.

## Offline simulation cases run against the mirrored Python model

Results from the current OCX Type 2 simulator:

| Case | Input peak | Output peak | Limited? | Notes |
|---|---:|---:|---|---|
| nominal | 0.529982 | 0.943008 | no | stable |
| hot | 0.939080 | 1.000000 | yes | limiter engaged |
| quiet | 0.079865 | 0.053471 | no | expected on non-encoded test material |
| mismatch | 0.249990 | 0.402320 | no | stable dual-mono behavior |
| bassheavy | 0.476902 | 1.000000 | yes | limiter engaged |
| clipped_in | 1.000000 | 1.000000 | yes | clipped source stays constrained |
| silence | 0.000000 | 0.000000 | no | no NaN / no instability |

## Important interpretation

A Type 2 decoder is an **expander**. If you feed it ordinary non-encoded audio, some material can become darker and quieter in lower-level passages. That does **not** by itself indicate a bug. Judge it with encoded source material.

## Remaining real-world risks

1. Portable headphone outputs may overdrive the shield input if their volume is too high.
2. Long analog wiring can add hum and instability in listening tests.
3. The universal preset is a practical compromise, not a hardware-clone promise.
4. First listening tests should use TEAC analog line out before portable players.

## First bring-up rule

1. Compile.
2. Flash.
3. Test bypass.
4. Test encoded playback at conservative source level.
5. Only then touch serial tuning.
