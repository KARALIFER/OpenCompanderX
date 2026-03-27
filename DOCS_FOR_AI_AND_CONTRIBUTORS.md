# DOCS_FOR_AI_AND_CONTRIBUTORS

This file is for AI agents and contributors working on repository documentation/process quality.
It is **not** an end-user quickstart.

## 1) Project scope summary

OpenCompanderX / OCX Type 2 targets practical dbx Type II–style compander workflows on:

- Teensy 4.1
- Teensy Audio Adaptor Rev D/D2
- SGTL5000 analog frontend

Current playback path is the main operational focus.

## 2) Current architecture facts to keep consistent

- `UNIVERSAL` is the baseline preset.
- `AUTO_CAL` is a **static** measurement workflow.
- `PLAYBACK_GUARD_DYNAMIC` is a conservative protection layer.
- No fixed `W1200` main preset in current primary architecture.

## 3) Calibration truth rules (must stay explicit)

- `AUTO_CAL` is **not** an always-on music auto-tuner.
- `AUTO_CAL` uses a **dbx Type II encoded 1 kHz measurement tape tone** as static base.
- The 400 Hz tone is **not** the AUTO_CAL tone.
- The 400 Hz tone is a **post-decoder output/workflow calibration tone**.
- `0 VU` is the **recording reference** of the measurement tape.
- Actual decoder input level depends on source deck/player output level.

## 4) Guard wording rules

Allowed framing:

- conservative playback protection
- output trim down (if needed)
- headroom up (if needed)
- optional temporary boost-cap reduction
- no live retuning of decoder core

Do not claim perfection, mastering AI, or guaranteed CD-like results.

## 5) Current default baseline values to document consistently

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

## 6) Documentation boundaries (must remain separated)

- `README.md`: short human-facing entry point.
- `FINAL_VALIDATION_ocx_type2_teensy.md`: technical validation/methodology/runtime interpretation.
- `DOCS_FOR_AI_AND_CONTRIBUTORS.md` (this file): contributor/AI documentation logic and wording constraints.

README should not be overloaded with internal process gates or long methodology sections.

## 7) Honesty policy

Never present unverified claims as repository facts. In particular, avoid claims of:

- historical exact dbx Type II equivalence,
- complete standards compliance without hard measurement evidence,
- universal validation across all decks/hardware.

## 8) Rule for doc-only tasks

If a task is explicitly documentation-only:

- do not modify `.ino`, `.py`, `.json`, tests, CI, or build logic;
- keep changes limited to documentation files;
- preserve factual consistency with the current repository state.
