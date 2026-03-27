# OpenCompanderX / OCX Type 2

OpenCompanderX is a **firmware-first project for Teensy 4.1 + SGTL5000** (Teensy Audio Adaptor Rev D/D2).

The main purpose of this repository is practical dbx Type II–style cassette decoding on **real hardware**: flash firmware, connect your deck/player, calibrate when needed, and run playback with telemetry and conservative protection.

Offline simulator/harness tooling exists for validation work, but for normal users the main product is the Teensy firmware workflow on hardware.

> This project is independent and is not affiliated with, endorsed by, or sponsored by dbx, HARMAN, or any trademark owner.

## What this project does

- Main focus: **decode** Type II encoded cassette playback on real hardware.
- `UNIVERSAL`: baseline preset for general playback.
- `AUTO_CAL`: static calibration mode based on a **dbx Type II encoded 1 kHz measurement tape tone**.
- `PLAYBACK_GUARD_DYNAMIC`: conservative protection layer for playback runtime.
- Encode/roundtrip paths exist, but decode is the main user entry path.

## Target hardware

- **Teensy 4.1**
- **Teensy Audio Adaptor Rev D/D2 (SGTL5000)**
- Stereo **Line-In** -> stereo **Line-Out / Headphone**

## Quickstart: flash firmware (Windows + Arduino IDE first)

### Recommended for normal users: Windows + Arduino IDE + Teensy Loader

1. Install **Arduino IDE** (current stable release).
2. Install **Teensy board support** (Teensyduino / PJRC integration for Arduino IDE).
3. Open `ocx_type2_teensy41_decoder.ino` in Arduino IDE.
4. Set board to **Teensy 4.1**.
5. Set USB Type to **Serial**.
6. Keep CPU speed at the default Teensy 4.1 setting unless you have a specific reason to change it.
7. Click **Verify/Compile**, then **Upload** (Teensy Loader flow).
8. Open Arduino IDE **Serial Monitor**.

### Optional path for advanced users: PlatformIO

1. Build firmware:
   ```bash
   pio run -e teensy41
   ```
2. Upload firmware:
   ```bash
   pio run -e teensy41 -t upload
   ```
3. Open serial monitor:
   ```bash
   pio device monitor
   ```

## First boot / first real use

After flashing and opening serial monitor, use this minimal path:

1. Send `0` to load factory preset.
2. Send `p` for full status.
3. Send `m` for compact telemetry.
4. If using measurement tape workflow, send `l` to start `AUTO_CAL`.
5. Check `J` / `K` / `L` (AUTO_CAL status / raw telemetry / locked values).

## Calibration basics (must-know)

- `AUTO_CAL` is **not** a permanent music auto-tuner.
- AUTO_CAL base tone = **dbx Type II encoded 1 kHz measurement tape tone**.
- 400 Hz tone = **workflow/output calibration tone** (post-decoder), not AUTO_CAL tone.
- `0 VU` = **recording reference** of the measurement tape.
- Actual decoder input level depends on deck/player line output level.

## Guard behavior (short)

`PLAYBACK_GUARD_DYNAMIC` is conservative by design:

- lowers output trim if needed,
- increases headroom if needed,
- can reduce boost cap if needed.

It does **not** retune decoder core behavior live during playback.

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

For technical baseline/default parameter details, use the validation/contributor docs instead of this quickstart README.

## Discoverability (concise)

Keywords: **dbx Type II decoder**, **dbx type 2 decoder**, **cassette decoder**, **Teensy cassette decoder**, **SGTL5000 tape decoder**.
