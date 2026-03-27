# OpenCompanderX / OCX Type 2

OpenCompanderX is a **firmware-first project** for **Teensy 4.1 + SGTL5000** (Teensy Audio Adaptor Rev D/D2).

Main goal: practical dbx Type II–style cassette decoding on **real hardware** with a clear workflow (flash firmware -> connect source -> check telemetry -> calibrate when needed -> run playback).

> This project is independent and is not affiliated with, endorsed by, or sponsored by dbx, HARMAN, or any trademark owner.

## What this project is

For normal users, the primary product is the Teensy firmware workflow on hardware.
Simulator/harness tools exist for technical validation, but they are not the main entry path for first-time users.

## Target hardware

- **Teensy 4.1**
- **Teensy Audio Adaptor Rev D/D2 (SGTL5000)**
- Stereo **Line-In** -> stereo **Line-Out / Headphone**
- USB data cable to the host PC

## Main features

- `UNIVERSAL`: baseline playback preset.
- `AUTO_CAL`: static calibration mode.
- `PLAYBACK_GUARD_DYNAMIC`: conservative runtime protection for playback.
- Runtime telemetry and serial commands for everyday operation.

## Calibration basics (short)

- `AUTO_CAL` is **not** a permanent music auto-tuner.
- `AUTO_CAL` uses a **dbx Type II encoded 1 kHz reference tone from a measurement tape** as static base.
- The **400 Hz tone is not the AUTO_CAL tone**.
- The **400 Hz tone** is a **post-decoder workflow/output calibration tone**.
- `0 VU` is the **recording reference** of the measurement tape.
- Actual decoder input level depends on the output level of your deck/walkman/player.

## Windows quickstart: flash with Arduino IDE

### What you need

- Windows PC
- Arduino IDE (current stable release)
- Teensy board support for Arduino IDE (Teensyduino / PJRC integration)
- Teensy 4.1 + Teensy Audio Adaptor Rev D/D2
- Repository checkout (or at least `ocx_type2_teensy41_decoder.ino` in the correct sketch folder)

### Step-by-step

1. Install Arduino IDE.
2. Open Arduino IDE Boards/Package Manager and add/install **Teensy board support** (PJRC / Teensyduino integration).
3. Confirm that the **Teensy package** is installed and that `Teensy 4.1` appears as a selectable board.
4. Open `ocx_type2_teensy41_decoder.ino` (from the repository checkout, in its sketch folder).
5. In Arduino IDE, set:
   - **Board:** `Teensy 4.1`
   - **USB Type:** `Serial`
6. Click **Verify/Compile**.
7. Click **Upload**.
8. If upload does not start automatically, open Teensy Loader and press the Teensy **Program** button once.
9. Open the Arduino IDE **Serial Monitor** after upload.

## Alternative for advanced users: PlatformIO

```bash
pio run -e teensy41
pio run -e teensy41 -t upload
pio device monitor
```

## First steps after flashing

1. Send `0` -> load factory preset.
2. Send `p` -> full status.
3. Send `m` -> compact telemetry.
4. Optional measurement workflow: send `l` -> start `AUTO_CAL`.
5. Check `J`, `K`, `L` -> AUTO_CAL status/raw telemetry/locked values.

## Guard behavior (short)

`PLAYBACK_GUARD_DYNAMIC` is a conservative safety layer:

- lowers output trim if needed,
- increases headroom if needed,
- can reduce boost cap if needed.

It does **not** retune the decoder core live during playback.

## Important commands

- `p` full status
- `m` compact telemetry
- `n` diagnostics snapshot
- `N` reset diagnostics counters
- `v` new clip events since last check
- `X` reset runtime/clip/diag counters
- `l` start AUTO_CAL
- `J` AUTO_CAL status
- `K` AUTO_CAL raw telemetry
- `L` AUTO_CAL locked values
- `H` guard toggle
- `T` periodic DIAG mode toggle (3 s)
- `0` reload factory preset
- `P` persist settings

## Honest status / limitations

- No claim of historical dbx Type II exact equivalence.
- No claim of complete standards compliance without dedicated hard measurement chain.
- No claim of full validation on all decks/hardware paths.
- SGTL5000 analog I/O and headroom limits remain real constraints.

## Further docs

- Technical validation/methodology: [`FINAL_VALIDATION_ocx_type2_teensy.md`](FINAL_VALIDATION_ocx_type2_teensy.md)
- AI/Codex/contributor doc boundaries: [`DOCS_FOR_AI_AND_CONTRIBUTORS.md`](DOCS_FOR_AI_AND_CONTRIBUTORS.md)

## Discoverability (concise)

Keywords: **dbx Type II decoder**, **dbx type 2 decoder**, **cassette decoder**, **Teensy cassette decoder**, **SGTL5000 tape decoder**.
