# OpenCompanderX / OCX Type 2



- real Teensy 4.1 firmware for analog line-in to analog line/headphone out,
- a matching offline WAV simulator,
- an automated offline regression/measurement harness,
- a reproducible PlatformIO build path in addition to Arduino IDE use,
- a validation report that clearly separates real execution from hardware-only unknowns.

## Fixed target hardware

This project is intentionally scoped to exactly this playback chain:

- **MCU:** Teensy 4.1
- **Audio board:** PJRC Teensy Audio Adaptor Board Rev D/D2
- **Codec:** SGTL5000
- **Input:** stereo analog line input via the shield's line-in header
- **Output:** stereo analog line-out via the shield's line-out header
- **Optional output:** headphone jack on the Audio Adaptor
- **No required extras:** no display, no encoder, no recorder, no USB-audio side path

## What is practically validated in this repo

### Real executions performed in this environment

- Python analysis stack installed and exercised.
- PlatformIO/Teensy toolchain installed and used to compile the firmware for **Teensy 4.1**.
- Offline simulator executed on generated material.
- Automated regression harness executed across synthetic edge cases.
- Pytest regression tests executed.


### What is **not** fully simulatable offline

The following still require physical hardware and real analog wiring:

- true SGTL5000 analog gain staging,
- noise floor / hum / grounding behavior,
- line-in headroom against TEAC W-1200 / FiiO CP13 / We Are Rewind outputs,
- actual headphone output loudness and clipping margins,
- subjective listening validation on real encoded program material.

## Repository layout

- `ocx_type2_teensy41_decoder.ino` - firmware for Arduino IDE / Teensyduino
- `platformio.ini` + `src/main.cpp` - reproducible command-line build path
- `ocx_type2_profile.json` - synchronized universal default profile for firmware/simulator/docs
- `ocx_type2_wav_sim.py` - offline decoder simulator for WAV files
- `ocx_type2_harness.py` - automated synthetic test/measurement harness
- `tests/test_ocx_type2.py` - regression tests
- `FINAL_VALIDATION_ocx_type2_teensy.md` - audit/build/validation report

## Universal default profile


- **input trim -3 dB:** leaves more margin for portable headphone outputs that can run hotter than line-level.
- **output trim -1 dB + 1 dB headroom:** reduces avoidable soft-clip engagement during difficult material.
- **sidechain HP 90 Hz:** reduces low-frequency pumping from rumble and bass-heavy sources.
- **sidechain shelf +16 dB @ 2.8 kHz:** keeps the detector sensitive to encoded HF energy without pushing hiss excessively.
- **de-emphasis -6 dB @ 1.85 kHz:** a more moderate universal playback voicing than the earlier darker shelf.
- **attack 3.5 ms / release 140 ms:** slightly steadier detector ballistics for general playback.

These defaults are meant to reduce first-power-on failure risk. They are **not** a claim of bit-exact equivalence to any proprietary legacy decoder.

## Wiring

- Connect source left/right/ground to the Audio Adaptor **LINE IN** header.
- Connect amplifier, active speakers, or downstream headphone amp to **LINE OUT**.
- Optionally monitor with the Audio Adaptor headphone jack.
- Keep temporary 3.5 mm breakout wiring short to minimize hum and pickup.

## Arduino IDE 2.x flash path

1. Install Arduino IDE 2.x.
2. Add `https://www.pjrc.com/teensy/package_teensy_index.json` in **Additional boards manager URLs**.
3. Install the PJRC Teensy package.
4. Open `ocx_type2_teensy41_decoder.ino`.
5. Select:
   - **Board:** Teensy 4.1
   - **USB Type:** Serial
   - **CPU Speed:** 600 MHz
   - **Optimize:** Faster or Fastest
6. Upload to the board.
7. Open Serial Monitor at `115200` baud.

## Reproducible command-line build path

The repo also supports a real command-line build with PlatformIO.

### Install tools

```bash

```

### Build firmware for Teensy 4.1

```bash
pio run -e teensy41
```

This build targets:

- board: **Teensy 4.1**
- USB type: **Serial**
- CPU speed: **600 MHz**
- optimization intent: **Fastest**

## Serial commands

- `h` help
- `p` print status
- `x` clear clip flags
- `B` reset DSP state
- `b` bypass on/off
- `0` reload factory preset
- `i/I` input trim -/+ 0.5 dB
- `o/O` output trim -/+ 0.5 dB
- `s/S` strength -/+ 0.05
- `f/F` reference dB -/+ 1 dB
- `a/A` attack -/+ 0.5 ms
- `r/R` release -/+ 5 ms
- `c/C` sidechain HP -/+ 10 Hz
- `q/Q` sidechain shelf gain -/+ 1 dB
- `w/W` sidechain shelf freq -/+ 100 Hz
- `e/E` de-emphasis gain -/+ 1 dB
- `d/D` de-emphasis freq -/+ 50 Hz
- `g/G` headroom -/+ 0.5 dB
- `y/Y` DC block -/+ 1 Hz
- `t` toggle built-in 1 kHz tone
- `z/Z` tone level -/+ 1 dB

## Offline WAV simulation

```bash
python3 ocx_type2_wav_sim.py input.wav decoded.wav --plot
```

Requirements:

- WAV sample rate should be **44.1 kHz** for direct comparability with the Teensy audio path.


## Automated harness / regression measurements

Run the synthetic harness:

```bash
python3 ocx_type2_harness.py --out-dir artifacts/harness --write-wavs
```

This covers at least the following classes:

1. silence
2. 1 kHz sine at multiple levels
3. logarithmic sweep
4. pink noise
5. white noise
6. bursts / sudden peaks
7. slow envelope steps
8. stereo-identical
9. left/right different
10. bass-heavy content
11. treble-heavy content
12. clipped input
13. too-quiet input
14. too-hot input
15. DC / rumble contamination
16. synthetic music-like material

Generated metrics include:

- input/output peak
- RMS
- crest factor
- channel deviation
- estimated gain-curve mean/std
- null residual
- MSE / MAE / max abs error
- correlation
- frequency-response delta
- transient delta
- optional reference comparison metrics if a reference output directory is supplied

## Pytest regression checks

```bash
pytest -q
```

## Test workflow before real hardware listening

1. Build the firmware with `pio run -e teensy41`.
2. Run `pytest -q`.
3. Run `python3 ocx_type2_harness.py --out-dir artifacts/harness`.
4. Review `artifacts/harness/metrics.json` and `metrics.csv`.
5. Flash hardware.
6. Verify bypass first.
7. Start with conservative source volume.
8. Only then compare decoded playback on real encoded material.

## Limits and honesty notes

- No proprietary plugin/reference archive was present in this working tree, so no black-box reference measurement was possible here.
- The harness can compare against a separate reference-output directory if such outputs are legally available later.
- Without real hardware, claims are limited to compile correctness, numerical stability, offline behavior, and configuration consistency.

