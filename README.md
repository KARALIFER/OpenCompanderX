# OpenCompanderX / OCX Type 2

OpenCompanderX is firmware for **Teensy 4.1 + SGTL5000** focused on practical **dbx Type II–style cassette decoding** on real hardware.

If you want the fastest start: flash the firmware with **Arduino IDE on Windows**, open Serial Monitor, run a few startup commands, then decide whether you need `AUTO_CAL`.

> This project is independent and is not affiliated with, endorsed by, or sponsored by dbx, HARMAN, or any trademark owner.

## What this project is

- A real-device playback decoder workflow (not just simulation).
- Built for people who want to run cassette decoding on Teensy hardware.
- The main target is firmware usage on device; validation/methodology docs are available separately.

## Target hardware

- **Teensy 4.1**
- **Teensy Audio Adaptor Rev D/D2** with **SGTL5000**
- Stereo line input source (deck/walkman/player)
- Stereo line-out/headphone destination
- USB data cable to a host PC

## Current architecture (simple)

- `UNIVERSAL`: baseline playback preset.
- `AUTO_CAL`: **static** calibration workflow.
- `PLAYBACK_GUARD_DYNAMIC`: conservative playback protection layer.

## Calibration basics

- `AUTO_CAL` is **not** a permanent music auto-tuner.
- `AUTO_CAL` uses a **dbx Type II encoded 1 kHz measurement tape tone**.
- The **400 Hz tone is not the AUTO_CAL tone**.
- The 400 Hz tone is a **post-decoder workflow/output calibration tone**.
- `0 VU` is the **recording reference** of the measurement tape.
- Real decoder input level still depends on your deck/player output level.

## Flash firmware on Windows with Arduino IDE (main path)

### You need

- Windows PC
- Arduino IDE (stable)
- Teensy board support (PJRC / Teensyduino integration)
- This repository checkout (especially `ocx_type2_teensy41_decoder.ino`)

### Steps

1. Install Arduino IDE.
2. In Boards/Package Manager, add/install **Teensy board support**.
3. Confirm `Teensy 4.1` is available as board option.
4. Open `ocx_type2_teensy41_decoder.ino` from this repository.
5. Set in Arduino IDE:
   - **Board:** `Teensy 4.1`
   - **USB Type:** `Serial`
6. Click **Verify/Compile**.
7. Click **Upload**.
8. If upload does not start, open Teensy Loader and press the Teensy **Program** button once.
9. Open **Serial Monitor** after upload.

## Alternative: PlatformIO / advanced workflow

```bash
pio run -e teensy41
pio run -e teensy41 -t upload
pio device monitor
```

## First steps after flashing

1. `0` -> reload factory preset.
2. `p` -> print full status.
3. `m` -> print compact telemetry.
4. If you have the correct measurement tape, run `l` -> start `AUTO_CAL`.
5. Check `J`, `K`, `L` -> AUTO_CAL status, telemetry, locked values.
6. Use `H` -> toggle Guard behavior if needed for your material/path.

## Key serial commands

- `0` reload factory preset
- `p` full status
- `m` compact telemetry
- `l` start AUTO_CAL
- `J` AUTO_CAL status
- `K` AUTO_CAL raw telemetry
- `L` AUTO_CAL locked values
- `H` guard toggle
- `P` persist settings

Additional diagnostics are available (`n`, `N`, `v`, `X`, `T`) once the basic workflow is running.

## Honest status / limitations

- No claim of historical dbx Type II exact equivalence.
- No claim of complete standards compliance without dedicated hard measurement chain.
- No claim of full validation on all decks/hardware paths.
- SGTL5000 analog I/O and headroom limits are real constraints.

## Further documentation

- Technical validation & methodology: [`FINAL_VALIDATION_ocx_type2_teensy.md`](FINAL_VALIDATION_ocx_type2_teensy.md)
- AI/contributor documentation rules: [`DOCS_FOR_AI_AND_CONTRIBUTORS.md`](DOCS_FOR_AI_AND_CONTRIBUTORS.md)

## Discoverability (concise)

Keywords: **dbx Type II decoder**, **dbx type 2 decoder**, **cassette decoder**, **Teensy cassette decoder**, **SGTL5000 tape decoder**.
