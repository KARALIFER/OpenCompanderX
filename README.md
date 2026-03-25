# OpenCompanderX / OCX Type 2

This repository contains a robust **Type-II compander decoder** for a fixed target hardware platform:

- Teensy 4.1
- Teensy Audio Adaptor Rev D/D2 (SGTL5000)
- Stereo analog Line-In to stereo analog Line-Out/Headphone

## Current, reproducible status

The current state verifiable in the repository includes:

- The firmware builds with PlatformIO for `teensy41`.
- The firmware decoder path is explicitly **stereo-coupled**: a shared detector based on `max(L/R)` sidechain power and a shared gain stage. This reduces stereo image drift on asymmetric material.
- Output finalization is centralized in `finalizeOutput`: output trim, headroom, soft clip, and clip counting run consistently across both the decode and bypass paths.
- An offline simulator for WAV files is included.
- The harness explicitly separates:
  - **reference-free stability and plausibility evaluation** (clipping, channel mismatch, ballistics/pumping indicators, spectral coloration, transient loss, soft-clip dependence)
  - **optional reference evaluation** (only when real reference files are available)
- The reference-free scoring logic also evaluates **level-tracking plausibility** via gain-vs-input slope/R² and penalizes overly flat or implausible tracking.
- Tuning uses a **two-stage search**: fast coarse selection (for example at 4 kHz) followed by final re-ranking at **44.1 kHz**.
- The simulator can additionally compare an **RMS-closer detector path** (`detector_mode=rms`) against the previous energy-like path.
- The harness now includes an explicit **cassette-primary** validation path with a mandatory frequency/level matrix, music-like compound cases, and broadband cases.

## Hardware-specific points

The following aspects can only be fully evaluated on real hardware:

- Analog headroom and noise behavior of the SGTL5000 stage
- Final tuning for specific source devices (for example TEAC W-1200, FiiO CP13, We Are Rewind)
- Subjective sound evaluation of real Type-II encoded material

## Key files

- `ocx_type2_teensy41_decoder.ino` – firmware
- `ocx_type2_profile.json` – shared profile for firmware and simulator
- `ocx_type2_wav_sim.py` – offline simulator
- `ocx_type2_harness.py` – regression, harness, and tuning
- `tests/test_ocx_type2.py` – tests
- `FINAL_VALIDATION_ocx_type2_teensy.md` – validation methodology and hardware workflow

## Profile principle

There is **one conservative universal profile** for the fixed target hardware.  
There is no multi-profile system and no auto-detection.

Current state `ocx_type2_universal_v2`:

- `sample_rate_hz = 44100`
- `audio_memory_blocks = 64`
- `codec.line_in_level = 0`
- `codec.line_out_level = 29`
- `decoder.input_trim_db = -3.0`
- `decoder.output_trim_db = -1.0`
- `decoder.strength = 0.76`
- `decoder.reference_db = -18.0`
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

Note on sample rate: the profile and harness operate at **44.1 kHz nominal**.  
At runtime, the Teensy firmware runs at `AUDIO_SAMPLE_RATE_EXACT` (a slightly different exact I2S clock), without automatically deriving different profile parameters from that.

## Calibration level vs. decoder reference

These two values are intentionally kept separate:

- `tone.level_dbfs` is the **calibration test tone level** used for real deck/tape alignment.
- `decoder.reference_db` is an **internal model parameter** of the decoder control loop.

In the firmware, the calibration tone is injected **post-decoder** into the output mix.  
It is therefore used for output and workflow calibration, not as a decoder input test tone.

Also, `bypass` is intentionally **not** a transparent hard-relay bypass: headroom and soft clip remain active so that analog output protection is preserved even in bypass mode.

### Current hardware reference point for the documented user setup
(Mixtape Nerd + TEAC W-1200 + RTM tape)

- 0 VU / tape reference is practically located at about **-9.8 dBFS**  
  (roundable to -10 dBFS)
- That is why the default test tone is set to **400 Hz @ -9.8 dBFS**

This is a **setup-specific measurement value**, not a universal historical dbx standard.

`decoder.reference_db` remains at `-18.0` for now, until simulator, harness, and hardware comparison justify an independently supported adjustment of this model parameter.

## Methodology: digital reference vs. analog target

### 1) Digital reference path (black-box comparison)

Example: USB playback, PC reference decoder, or simulator.  
This path is used to evaluate decoder logic, ballistics, and Type-II tendencies.

The following are **not part of this path**:

- SGTL5000 `line_in_level`
- analog frontend headroom

### 2) Analog target path (real Teensy hardware)

Example: analog out (TEAC/FiiO/WAR) → Teensy Line-In → OCX → analog out

The focus here is on:

- `line_in_level`
- input trim
- clipping margin
- noise/hum
- runtime stability

These two paths are kept methodologically separate and are not mixed.

## Reference material

### General notes

- **Cassette A (uncompanded)** is useful for level, linearity, and headroom checks, but it is not sufficient on its own for Type-II decoder alignment.
- **Cassette B (Type-II encoded)** is required for actual decoder tuning.

For black-box reference comparison, the following applies:

- Reference files are compared after length alignment.
- MSE, MAE, correlation, frequency comparison, and transient comparison are recorded separately as a reference score.
- Without a reference, **no claim of reference closeness** is made.
- Optional aids such as play trim, azimuth correction, gap-loss compensation, or EQ conversion (IEC 120 µs ↔ 70 µs) are treated as **reference-path aids**, not as implicit OCX core logic.

### Recommended cassette-primary reference layout

Clear separation between:

- `refs/type2_cassette_real/` = real, legally usable references with documented origin/license
- `refs/type2_cassette_synth/` = synthetic or approximate references generated reproducibly

Per case:

- `<name>_encoded.wav` (required)
- `<name>_source.wav` (required)
- `<name>_reference_decode.wav` (optional)
- `<name>.json` (required metadata: origin, license, trust level, `source_type`)

Only cases containing at least `*_encoded.wav` and `*_source.wav` are included as `cassette_reference` in the evaluation.  
This structure improves practical relevance, but on its own it does not establish historical standard equivalence.

By default, the repository contains **no real licensed dbx Type-II reference recordings**.  
Therefore, the included primary basis is initially synthetic or approximate and marked accordingly.

### Known provided candidates

The following candidates can be imported separately:

- `musik_enc.wav` (if present): resampled to 44.1 kHz for the offline path and treated as `encoded_candidate_only`
- `musicfox_shopping_street.mp3` (if decodable): also converted to 44.1 kHz, but used only as an additional or stress case

The import process searches for these files first in the specified search path and then recursively below it if they are not located at the root.

Without a documented encoder and license path, neither case is treated as a hard gold reference.

Reference pairs `*_encoded.wav` and `*_source.wav` are also normalized by the harness to the target profile rate (44.1 kHz) when needed, so mixed source sample rates do not block execution.  
The original files remain unchanged.

## Local checks

```bash
python -m py_compile ocx_type2_wav_sim.py
python -m py_compile ocx_type2_harness.py
python -m py_compile tests/test_ocx_type2.py
pytest -q
pio run -e teensy41
```

## Harness and tuning examples

```bash
# Standard offline evaluation
python ocx_type2_harness.py --out-dir artifacts/harness

# Generate synthetic reference package (reproducible)
python ocx_type2_harness.py --generate-synth-refs --reference-dir refs --out-dir artifacts/harness_refs

# Index real references (if legally available)
python ocx_type2_harness.py --index-real-refs --reference-dir refs --out-dir artifacts/harness_refs

# Import known local music candidates (if files are available locally)
python ocx_type2_harness.py --prepare-known-music-candidates --reference-dir refs --out-dir artifacts/harness_refs

# Optional: fetch real references from manifest (only legally clean URLs)
python ocx_type2_harness.py --fetch-real-refs-manifest refs/type2_cassette_real/manifest.example.json --reference-dir refs --out-dir artifacts/harness_refs

# Evaluate cassette-priority cases only, separated by source type (real/synthetic/all)
python ocx_type2_harness.py --out-dir artifacts/harness_ref --reference-dir refs --cassette-priority-only --reference-source all

# Decoder overrides
python ocx_type2_harness.py --override strength=0.80 --override release_ms=160

# Two-stage tuning: coarse (4 kHz) + final (44.1 kHz)
python ocx_type2_harness.py --tune --tune-fs 4000 --tune-final-fs 44100 --tune-top-k 6

# Detector methodology comparison (energy vs. more RMS-like)
python ocx_type2_harness.py --detector-study --out-dir artifacts/harness_detector
```

## Hardware telemetry and evaluation

### Firmware commands

- `p` = full status
- `m` = compact telemetry status  
  (including `bypass=ON/OFF`, last `gDb/envDb`, tone status, and L/R mode)
- `n` = signal diagnostics snapshot  
  (input/output peak + RMS + mean, gain/env, decode activity, in/out delta, L/R balance)
- `N` = reset signal diagnostics counters only
- `v` = print only new clip events since the last `v` / `m` / `p` query
- `X` = reset clip/runtime counters, signal diagnostics, and usage maxima
- `k` = cycle test-tone channel mode (`BOTH -> LEFT -> RIGHT`)
- `0` = reload factory preset

### Telemetry meaning

- `AudioProcessorUsage()` / `AudioProcessorUsageMax()`  
  → current and maximum DSP CPU load
- `AudioMemoryUsage()` / `AudioMemoryUsageMax()`  
  → currently used audio blocks and maximum usage
- `allocFailCount`  
  → audio block allocation failures (must remain 0)
- `inputClipCount`  
  → frontend/decoder input overloaded
- `outputClipCount`  
  → output path overloaded
- `inClipNew` / `outClipNew`  
  → new clipping events since the last status query
- `gain clamp hits` / `near-limit` in the snapshot  
  → how often `maxCutDb` / `maxBoostDb` were reached hard or nearly reached

## Recommended workflow on real hardware

1. Boot the device and load the factory preset with `0`.
2. Reset clip/runtime counters and maxima using `X`.
3. Calibration: feed a 400 Hz test tone at `tone.level_dbfs = -9.8` and align it to the setup’s own 0 VU / reference point in the deck workflow.
4. Play the defined test source for several minutes.
5. Read the compact status cyclically with `m`, and check the full status with `p`.
6. Evaluation: CPU and memory reserve sufficient, `allocFailCount == 0`, clip counters plausible relative to input level and headroom.

The compact `m` line explicitly shows `cpuRes=OK/TIGHT` and `memRes=OK/TIGHT` alongside clip/allocation counters and a clearly visible `bypass=ON/OFF`.

In addition, `m` now reports `inClipNew/outClipNew`, so new overload events since the last report are immediately visible.

The `n` snapshot is intended as a live diagnostics window:  
press `N`, play material, read `n`, and evaluate based on in/out delta, gain min/max, and decode activity.

It also includes practical cassette-related indicators with very low CPU cost:

- RMS and peak L/R balance in/out
- L/R difference mean and normalized L/R correlation  
  (indication of channel or phase issues)
- simple sidechain HF/LF proxy (`high-vs-low`)  
  as a rough indicator of low- or high-frequency detector stimulus
- activity classification (`LOW` / `MODERATE` / `HIGH`) plus `Cassette quick hints`  
  (diagnostic aid only, not a proof of standard compliance)
- clamp evaluation (`cut/boost hits`, `near-limit`, compact interpretation),  
  helping to contextualize cases such as `minGainDb = -24 dB`

## Arduino `.ino` compatibility

`toneChannelModeLabel(...)` intentionally uses a `uint8_t` signature so that the Arduino IDE / `.ino` prototype preprocessor does not trigger enum ordering errors.

## Test cassette methodology

- **Cassette A (not companded, base reference):**  
  400 Hz (here: -9.8 dBFS as the setup-specific 0 VU reference), 1 kHz, 10 kHz, 3.15 kHz for level, channel matching, HF/azimuth, and speed checks
- **Cassette B (Type-II encoded, decoder tuning):**  
  multi-level 1 kHz, bursts, envelope steps, pink/white noise, sweep, bass+HF, music
- **Cassette-primary harness (offline):**
  - single-tone / multi-level matrix: **400 Hz, 1 kHz, 3.15 kHz, 10 kHz** across multiple levels
  - multi-frequency and music-like compound cases: two-tone, bass+HF, burst/transient train, fast level switches, `music_like`
  - broadband cases: pink/white noise, log sweep

Always use exactly **one** Type-II encoding path. No double encoding.

## Scope of claims

This project makes **no** claim of bit-exactness, original equivalence, or reference equivalence without solid measurement evidence.

The extended cassette-primary harness primarily increases practical validation depth; it does not replace a fully documented dbx Type-II standards-conformance measurement.

## Compatibility status (dbx Type II cassette)

- The goal remains maximum practical decoder compatibility for real dbx Type-II cassettes within the described hardware setup.
- The current state is a methodically tuned, stable decoder path with Type-II-oriented ballistics and sidechain shaping.
- Historical standard accuracy of a dbx Type-II decoder is not currently established.
- Disc-specific assumptions, such as implicit LF roll-off carry-over, are not silently adopted as cassette defaults.
- Remaining deviations are tracked through harness and hardware measurements rather than being masked by equivalence claims.

## Ordered Parts

| Part | Link |
|---|---|
| Teensy 4.1 (with pins) | [Amazon.de](https://www.amazon.de/dp/B08CTM3279?ref=ppx_yo2ov_dt_b_fed_asin_title) |
| Audio Adaptor Board for Teensy 4.0 | [Amazon.de](https://www.amazon.de/dp/B07Z6NW913?ref=ppx_yo2ov_dt_b_fed_asin_title) |
| RUNCCI-YUN 6pcs 3.5mm TRRS Female Jack to Bare Wire Cable (30 cm) | [Amazon.de](https://www.amazon.de/dp/B0CWR1CPNG?smid=AT0FJ7CZCB0G9&ref_=chk_typ_imgToDp&th=1) |
| 2.54 mm 40-Pin Male/Female Header Set | [Amazon.de](https://www.amazon.de/dp/B0CWR2TZNX?ref=ppx_yo2ov_dt_b_fed_asin_title) |
