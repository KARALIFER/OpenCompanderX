# OpenCompanderX / OCX Type 2

OpenCompanderX is a **Teensy 4.1 firmware project** for practical dbx Type II–style cassette decoding on real hardware using the SGTL5000 audio adaptor.

The main goal of this repository is a usable firmware workflow on Teensy 4.1: flash the device, connect your cassette source, calibrate when needed, and run playback decoding with telemetry and conservative output protection.

> This project is independent and is not affiliated with, endorsed by, or sponsored by dbx, HARMAN, or any trademark owner.

## What this project does

- Main focus: **decode** Type II encoded cassette playback on real hardware.
- `AUTO_CAL`: static calibration workflow based on a **dbx Type II encoded 1 kHz measurement tape tone**.
- `PLAYBACK_GUARD_DYNAMIC`: conservative runtime protection layer for playback.
- Encode/roundtrip paths exist, but decode is the main entry path for users.

## Target hardware

- **Teensy 4.1**
- **Teensy Audio Adaptor Rev D/D2 (SGTL5000)**
- Stereo **Line-In** -> stereo **Line-Out / Headphone**

## Flash firmware

### PlatformIO (recommended build path)

```bash
pio run -e teensy41
```

Upload with your usual Teensy workflow (PlatformIO upload or Teensy Loader).

### Arduino IDE / Teensy Loader (alternative)

1. Open `ocx_type2_teensy41_decoder.ino`.
2. Select board: **Teensy 4.1**.
3. Set USB type: **Serial**.
4. Compile and upload.

## First start (real usage path)

1. Open serial monitor.
2. Send `0` to load the factory preset.
3. Send `p` for full status.
4. Send `m` for compact telemetry.
5. Send `l` to start `AUTO_CAL` when using the measurement tape workflow.
6. Check `J` / `K` / `L` for AUTO_CAL status, raw telemetry, and locked values.

## Calibration basics

- `AUTO_CAL` is **not** a permanent music auto-tuner.
- AUTO_CAL base tone: **dbx Type II encoded 1 kHz reference tone from a measurement tape**.
- The **400 Hz tone is not the AUTO_CAL tone**.
- The **400 Hz tone** is a **post-decoder workflow/output calibration tone**.
- `0 VU` is the **recording reference** of the measurement tape.
- Actual decoder input level depends on your source deck/player line output.

## Guard explained (short)

`PLAYBACK_GUARD_DYNAMIC` is conservative by design:

- trims output down if needed,
- increases headroom if needed,
- can reduce boost cap if needed.

It does **not** retune the decoder core on the fly.

## Important commands (quick reference)

- `p` = full status
- `m` = compact telemetry
- `n` = signal diagnostics snapshot
- `N` = reset signal diagnostics counters
- `v` = new clip events since last query
- `X` = reset clip/runtime/diagnostic counters
- `l` = start AUTO_CAL
- `J` = AUTO_CAL status
- `K` = AUTO_CAL raw telemetry
- `L` = locked AUTO_CAL values
- `H` = guard toggle
- `T` = periodic DIAG mode (3 s), default off
- `0` = reload factory preset
- `P` = persist settings

## Current default baseline values

Current repository defaults (subject to change through validated tuning):

- `sample_rate_hz = 44100`
- `audio_memory_blocks = 64`
- `codec.line_in_level = 0`
- `codec.line_out_level = 29`
- `decoder.input_trim_db = -3.0`
- `decoder.output_trim_db = -1.0`
- `decoder.strength = 0.76`
- `decoder.reference_db = -18.0`
- `decoder.max_boost_db = 9.0`
- `decoder.max_cut_db = 24.0`
- `decoder.attack_ms = 3.5`
- `decoder.release_ms = 140.0`
- `decoder.sidechain_hp_hz = 90.0`
- `decoder.sidechain_shelf_hz = 2800.0`
- `decoder.sidechain_shelf_db = 16.0`
- `decoder.deemph_hz = 1850.0`
- `decoder.deemph_db = -6.0`
- `decoder.soft_clip_drive = 1.08`
- `decoder.dc_block_hz = 12.0`
- `decoder.headroom_db = 1.0`
- `tone.frequency_hz = 400.0`
- `tone.level_dbfs = -9.8`

## Honest status / limitations

- No claim of historical dbx Type II exact equivalence.
- No claim of perfect standards compliance without dedicated hard measurement chain.
- No claim that all decks/hardware paths are fully validated.
- SGTL5000 analog I/O and headroom limits remain real system constraints.

## Further docs

- Technical validation and methodology: [`FINAL_VALIDATION_ocx_type2_teensy.md`](FINAL_VALIDATION_ocx_type2_teensy.md)
- AI/Codex/contributor doc boundaries: [`DOCS_FOR_AI_AND_CONTRIBUTORS.md`](DOCS_FOR_AI_AND_CONTRIBUTORS.md)

## Discoverability (concise)

Keywords: **dbx Type II decoder**, **dbx type 2 decoder**, **cassette decoder**, **Teensy cassette decoder**, **SGTL5000 tape decoder**.
