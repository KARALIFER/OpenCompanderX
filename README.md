# OpenCompanderX / OCX Type 2

OpenCompanderX (OCX Type 2) is an independent Teensy-based compander project focused on practical **dbx Type II–style cassette decoding** on real hardware.

> This project is independent and is not affiliated with, endorsed by, or sponsored by dbx, HARMAN, or any trademark owner.

## What this project is

OCX Type 2 is a firmware + tooling repository for a reproducible, hardware-focused decoder workflow:

- Primary focus: **playback decoding** of Type II encoded cassette material
- Secondary path: encode/roundtrip support exists, but entry-level use is decoder-first
- Practical runtime telemetry and calibration workflow for real decks/players

## Target hardware

- **Teensy 4.1**
- **Teensy Audio Adaptor Rev D/D2 (SGTL5000)**
- Stereo **Line-In** -> stereo **Line-Out / Headphone**

## Current architecture (quick view)

- **`UNIVERSAL`**: default base preset
- **`AUTO_CAL`**: static measurement-tape based calibration preset
- **`PLAYBACK_GUARD_DYNAMIC`**: conservative playback protection layer

There is no fixed legacy `W1200` main preset in the current primary architecture.

## Calibration basics (important)

- `AUTO_CAL` is **not** a permanent music auto-tuner.
- `AUTO_CAL` uses a **dbx Type II encoded 1 kHz reference tone from a measurement tape** as a **static calibration base**.
- The **400 Hz tone is not the AUTO_CAL tone**.
- The **400 Hz tone** is a **post-decoder workflow/output calibration tone**.
- `0 VU` is the **recording reference of the measurement tape**.
- Actual playback level at decoder input depends on the output stage of your deck/player.

## Guard behavior (honest)

`PLAYBACK_GUARD_DYNAMIC` is intentionally conservative. If needed, it:

- reduces output trim,
- increases headroom,
- optionally reduces boost cap.

It does **not** continuously retune the decoder core during music playback.

## Current default baseline values

These are the current repository defaults (not an eternal truth):

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

## Key serial commands (quick reference)

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

## Flash / first start (short)

```bash
pio run -e teensy41
```

Then flash with your normal Teensy workflow (PlatformIO or Arduino/Teensy Loader), open serial monitor, load factory preset (`0`), and check status (`p` / `m`).

## Further documentation

- Technical validation and methodology: [`FINAL_VALIDATION_ocx_type2_teensy.md`](FINAL_VALIDATION_ocx_type2_teensy.md)
- AI/Codex/contributor work-logic and doc boundaries: [`DOCS_FOR_AI_AND_CONTRIBUTORS.md`](DOCS_FOR_AI_AND_CONTRIBUTORS.md)

## Honest status / limitations

- No claim of historically exact dbx Type II equivalence.
- No claim of perfect standards compliance without dedicated measurement chain.
- No claim that all decks/hardware paths are fully validated.
- SGTL5000 analog I/O and headroom limits remain real system constraints.

## Discoverability (concise)

OpenCompanderX can also be found via terms such as: **dbx Type II decoder**, **dbx type 2 decoder**, **cassette decoder**, **Teensy cassette decoder**, **SGTL5000 tape decoder**.
