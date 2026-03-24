# OCX Type 2 validation and audit report

## 1. Scope and honesty boundary

This report separates four very different things:

1. **real tool installation**,
2. **real firmware build validation**,
3. **real offline DSP execution and regression measurements**,
4. **hardware-only unknowns that cannot be proven without a physical Teensy 4.1 + SGTL5000 setup**.

No bit-exact claim is made.
No proprietary reference decoder was available inside this repo for black-box comparison.

## 2. Repo audit baseline

### What was already good

- Firmware structure was broadly plausible for a stereo decoder-only playback chain.
- DSP work stayed inside a custom `AudioStream`, which is appropriate for Teensy real-time audio.
- No heap allocation occurred inside `update()`.
- Serial I/O stayed outside the audio callback.
- SGTL5000 setup calls were syntactically valid for the Teensy Audio library.
- The algorithm already used dual-mono detection, sidechain filtering, envelope tracking, expansion, de-emphasis, and soft limiting.

### What was risky or incomplete

- The simulator had a real runtime bug in plotting (`n`, `plot_audio`, and `plt` handling were incomplete).
- Firmware and simulator defaults were duplicated, making silent drift likely.
- The previous defaults were somewhat aggressive/dark for a one-profile universal analog target.
- No automated regression harness existed for broad synthetic edge cases.
- No formal pytest regression layer existed.
- Documentation mixed practical guidance with implied validation that had not been reproducibly automated in-repo.
- There was no reproducible command-line firmware build configuration in the repo itself.

### What needed changing before first hardware tests

1. Add a real reproducible build path.
2. Synchronize defaults across firmware and simulator.
3. Add regression coverage for silence, hot inputs, DC/rumble, stereo mismatch, and clipped material.
4. Improve numerical safety around DC, NaN/Inf sanitation, and avoidable output overdrive.
5. Re-document the exact limits of offline validation.

### What can wait until real hardware is present

- final gain staging against the three named source devices,
- subjective listening refinement on actual encoded tapes,
- any optional future black-box comparison against legal reference outputs,
- final line-out/headphone loudness recommendations.

## 3. Installed tools and dependencies

### Firmware/build

- PlatformIO installed successfully.
- Teensy platform packages were downloaded and installed successfully through PlatformIO.
- Teensy 4.1 firmware build completed successfully.

### Python / analysis

Installed and used:

- Python 3
- numpy
- scipy
- soundfile
- matplotlib (optional only when `--plot` is used)
- pytest
- pandas

### Standard inspection tools available

- `file`
- `strings`
- `objdump`
- `nm`
- `git`

### Arduino CLI status

- `arduino-cli` was present in the environment.
- `arduino-cli core update-index` failed due network behavior inside that specific Go-based fetch path.
- Because the task required real practical validation rather than a theoretical note, PlatformIO was used as an equivalent reproducible Teensy build environment and succeeded.

## 4. Firmware build validation

### Real build target

Validated in practice with:

- **Board:** Teensy 4.1
- **USB Type:** Serial
- **CPU Speed:** 600 MHz
- **Optimization intent:** Fastest-style build flags via PlatformIO

### Build result

The firmware built successfully into `firmware.elf` and `firmware.hex`.

This confirms, at compile/link level:

- Teensy 4.1 compatibility,
- includes resolve correctly,
- Audio library objects resolve correctly,
- SGTL5000 API calls used in the sketch exist in the installed library set,
- no hidden type/API mismatch blocked the target build.

## 5. Firmware changes made

### Robustness improvements

- Added a synchronized profile file for shared defaults.
- Switched the universal codec input default to `lineInLevel(0)` because there is no harder counter-evidence in this repo for a hotter default.
- Removed unneeded SerialFlash dependency from the firmware/build path.
- Increased `AudioMemory` from 48 to 64 blocks for safer runtime margin.
- Added a one-pole DC blocker before detector/audio gain application.
- Added float sanitation in DSP paths to reduce NaN/Inf propagation risk.
- Added explicit headroom gain in addition to soft clipping.
- Added a DSP-state reset command and preset reset path that clears detector/filter history.

### Universal-profile tuning changes

The default profile was moved to a more conservative single-profile setting:

- codec line input level: `0`
- input trim: `-3 dB`
- output trim: `-1 dB`
- strength: `0.76`
- attack: `3.5 ms`
- release: `140 ms`
- sidechain HP: `90 Hz`
- sidechain shelf: `+16 dB @ 2.8 kHz`
- de-emphasis: `-6 dB @ 1.85 kHz`
- headroom: `1 dB`

### Why these changes were technically justified

- Portable headphone outputs can be materially hotter than true line-level, so extra analog and DSP-side input margin is safer.
- Slight output attenuation and dedicated headroom reduce unnecessary limiter engagement.
- Higher sidechain HP reduces rumble/bass pumping.
- Milder de-emphasis is a better universal starting point than a much darker shelf.
- Resettable detector/filter state makes A/B tests and recovery from abrupt source changes more deterministic.

## 6. Offline simulator and harness work

### Simulator improvements

- Reworked the simulator to load defaults from `ocx_type2_profile.json`.
- Fixed the mono/stereo input path so `(N,)`, `(N,1)`, and `(N,2)` inputs are handled consistently.
- Made plotting optional so `matplotlib` is only imported for `--plot`.
- Kept harness reference data optional and ensured JSON metrics always emit without requiring reference files.
- Added DC blocking, headroom, clip telemetry, and finite-value guarding to match firmware intent more closely.
- Kept the same broad topology as firmware: dual-mono detector, sidechain filters, envelope detector, gain law, de-emphasis, soft clip.

### New automated harness

A new offline harness was added and executed across these categories:

1. silence
2. 1 kHz sine at multiple levels
3. logarithmic sweep
4. pink noise
5. white noise
6. bursts
7. slow envelope steps
8. stereo-identical
9. stereo-different
10. bass-heavy
11. treble-heavy
12. clipped input
13. too-quiet input
14. too-hot input
15. DC/rumble contamination
16. synthetic music-like material

### Metrics produced

Per case, the harness now records:

- input peak / RMS / crest factor
- output peak / RMS / crest factor
- channel deviation
- gain-curve mean/std
- null residual
- MSE / MAE / max abs error
- correlation
- frequency-response delta
- transient delta
- reference-comparison fields when a legal reference directory is supplied later

## 7. Reference / proprietary plugin status

- No proprietary plugin/archive/reference output set was present in this repo.
- Therefore, **no black-box reference run was actually performed in this environment**.
- The new harness supports later comparison against externally supplied legal reference WAV outputs, but that did not occur in this run.

## 8. What was really executed

The following were run for real:

- Python dependency installation
- PlatformIO installation
- Teensy 4.1 firmware build
- `pytest`
- offline harness run
- offline single-file simulator path

## 9. What could only be simulated offline

Only the following were validated offline:

- DSP numerical behavior
- clipping containment
- DC/rumble rejection behavior
- stereo consistency behavior
- broad response to quiet/hot/clipped synthetic material
- consistency between documented default profile and simulator/firmware defaults

## 10. What could not be verified without physical hardware

Not verifiable here:

- true analog line-input overload point on the exact shield/board stack,
- headphone-output margin and noise on the actual board,
- source-specific compatibility with TEAC W-1200 / FiiO CP13 / We Are Rewind,
- real encoded-tape playback quality,
- hum susceptibility from temporary cable wiring,
- whether the current universal profile is already "final" subjectively.

## 11. Open risks

1. Portable sources may still exceed comfortable shield headroom depending on their volume knob position.
2. Real encoded program material may justify small input-trim or de-emphasis adjustments.
3. A proprietary reference may later reveal measurable voicing or detector-ballistic differences.
4. Offline synthetic tests cannot fully predict analog hiss, crosstalk, ground loops, or source impedance interactions.

## 12. Final assessment

### Practical status

**Praktisch brauchbare Offline-Basis**, but still short of true hardware/reference validation.

Reason:

- the firmware now builds reproducibly for Teensy 4.1,
- the DSP path is more robust and better bounded numerically,
- the repo now contains a real offline validation harness instead of only ad-hoc inspection,
- the project is better prepared for first hardware listening tests.

### Reference closeness

**Not currently quantifiable against a proprietary reference**, because no legal reference output set was available in this run.

### Bit-exact claim

**Not proven and not claimed.**
