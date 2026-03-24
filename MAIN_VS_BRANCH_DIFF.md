# Main-vs-Branch Vergleich (lokale Main-Basis e8149ca)

## README.md
- Added lines: 61
- Removed lines: 177
```diff
diff --git a/README.md b/README.md
index fd1ed51..7c32b9a 100644
--- a/README.md
+++ b/README.md
@@ -2,0 +3 @@
+Dieses Repository enthält einen robusten **Type-II-compander-decoder** für eine feste Zielhardware:
@@ -3,0 +5,4 @@
+- Teensy 4.1
+- PJRC Teensy Audio Adaptor Rev D/D2 (SGTL5000)
+- analog stereo line-in
+- analog stereo line-out / optional headphone
@@ -5,5 +10 @@
-- real Teensy 4.1 firmware for analog line-in to analog line/headphone out,
-- a matching offline WAV simulator,
-- an automated offline regression/measurement harness,
-- a reproducible PlatformIO build path in addition to Arduino IDE use,
-- a validation report that clearly separates real execution from hardware-only unknowns.
+## Scope-Grenzen (bewusst eng)
@@ -11 +12,4 @@
-## Fixed target hardware
+- Kein USB-Audio
+- Kein Encoder/Recorder/Display
+- Ein Universalprofil
+- Keine Autoerkennung / keine Mehrprofile
@@ -13 +17 @@
-This project is intentionally scoped to exactly this playback chain:
+## Was in diesem Repo valide ist
@@ -15,7 +19,4 @@ This project is intentionally scoped to exactly this playback chain:
-- **MCU:** Teensy 4.1
-- **Audio board:** PJRC Teensy Audio Adaptor Board Rev D/D2
-- **Codec:** SGTL5000
-- **Input:** stereo analog line input via the shield's line-in header
-- **Output:** stereo analog line-out via the shield's line-out header
-- **Optional output:** headphone jack on the Audio Adaptor
-- **No required extras:** no display, no encoder, no recorder, no USB-audio side path
+- Firmware-Build mit PlatformIO (`teensy41`) ist reproduzierbar.
+- Offline-Simulator und Harness laufen reproduzierbar.
+- Parameter-Sweeps (offline) sind dokumentiert.
+- Pytest deckt Produktionspfad inkl. 44.1 kHz mit ab.
@@ -23 +24 @@ This project is intentionally scoped to exactly this playback chain:
-## What is practically validated in this repo
+## Was **nicht** offline bewiesen werden kann
@@ -25 +26,3 @@ This project is intentionally scoped to exactly this playback chain:
-### Real executions performed in this environment
+- finale Klangbeurteilung auf realem Type-II-Programmmaterial
+- analoge SGTL5000-Headroom-/Noise-Details unter echter Verkabelung
+- Referenzgleichheit zu proprietären Decodern (keine legalen Referenzausgänge im Repo)
@@ -27,5 +30 @@ This project is intentionally scoped to exactly this playback chain:
-- Python analysis stack installed and exercised.
-- PlatformIO/Teensy toolchain installed and used to compile the firmware for **Teensy 4.1**.
-- Offline simulator executed on generated material.
-- Automated regression harness executed across synthetic edge cases.
-- Pytest regression tests executed.
+## Universalprofil (aktuell empfohlen)
@@ -32,0 +32 @@ This project is intentionally scoped to exactly this playback chain:
+Datei: `ocx_type2_profile.json`
@@ -34 +34 @@ This project is intentionally scoped to exactly this playback chain:
-### What is **not** fully simulatable offline
+Wichtige Decoder-Defaults:
@@ -36 +36,14 @@ This project is intentionally scoped to exactly this playback chain:
-The following still require physical hardware and real analog wiring:
+- `input_trim_db: -3.0`
+- `output_trim_db: -1.0`
+- `strength: 0.76`
+- `reference_db: -16.0`
+- `attack_ms: 3.5`
+- `release_ms: 140.0`
+- `sidechain_hp_hz: 90.0`
+- `sidechain_shelf_hz: 2800.0`
+- `sidechain_shelf_db: 16.0`
+- `deemph_hz: 1850.0`
+- `deemph_db: -6.0`
+- `soft_clip_drive: 1.08`
+- `dc_block_hz: 12.0`
+- `headroom_db: 1.0`
@@ -38,5 +51 @@ The following still require physical hardware and real analog wiring:
-- true SGTL5000 analog gain staging,
-- noise floor / hum / grounding behavior,
-- line-in headroom against TEAC W-1200 / FiiO CP13 / We Are Rewind outputs,
-- actual headphone output loudness and clipping margins,
-- subjective listening validation on real encoded program material.
+Codec-Teil:
@@ -44 +53,2 @@ The following still require physical hardware and real analog wiring:
-## Repository layout
+- `line_in_level: 0` (konservativer universeller Default)
+- `line_out_level: 29`
@@ -46,7 +56 @@ The following still require physical hardware and real analog wiring:
-- `ocx_type2_teensy41_decoder.ino` - firmware for Arduino IDE / Teensyduino
-- `platformio.ini` + `src/main.cpp` - reproducible command-line build path
-- `ocx_type2_profile.json` - synchronized universal default profile for firmware/simulator/docs
-- `ocx_type2_wav_sim.py` - offline decoder simulator for WAV files
-- `ocx_type2_harness.py` - automated synthetic test/measurement harness
-- `tests/test_ocx_type2.py` - regression tests
-- `FINAL_VALIDATION_ocx_type2_teensy.md` - audit/build/validation report
+## CPU/RAM stability notes
@@ -54 +58,7 @@ The following still require physical hardware and real analog wiring:
-## Universal default profile
+- `AudioMemory(64)` bleibt gesetzt; das ist für diese Objekt-/Patchcord-Struktur ein konservativer Startpunkt.
+- Firmware-Telemetrie enthält jetzt:
+  - `AudioProcessorUsage()` / `AudioProcessorUsageMax()`
+  - `AudioMemoryUsage()` / `AudioMemoryUsageMax()`
+  - Allocate-Fehler-Flag + Counter in `update()`
+  - Input/Output-Clip-Flags + Counter
+- Telemetrie wird nur im Statuspfad ausgegeben (nicht im Audio-Callback), damit Echtzeitpfad sauber bleibt.
@@ -55,0 +66 @@ The following still require physical hardware and real analog wiring:
+## Reproduzierbare Offline-Abstimmung (kein Referenz-Claim)
@@ -57,6 +68 @@ The following still require physical hardware and real analog wiring:
-- **input trim -3 dB:** leaves more margin for portable headphone outputs that can run hotter than line-level.
-- **output trim -1 dB + 1 dB headroom:** reduces avoidable soft-clip engagement during difficult material.
-- **sidechain HP 90 Hz:** reduces low-frequency pumping from rumble and bass-heavy sources.
-- **sidechain shelf +16 dB @ 2.8 kHz:** keeps the detector sensitive to encoded HF energy without pushing hiss excessively.
-- **de-emphasis -6 dB @ 1.85 kHz:** a more moderate universal playback voicing than the earlier darker shelf.
-- **attack 3.5 ms / release 140 ms:** slightly steadier detector ballistics for general playback.
+Baseline und Kandidaten werden über `ocx_type2_harness.py` mit gewichteter Stabilitäts-Score-Funktion verglichen.
@@ -64,71 +70 @@ The following still require physical hardware and real analog wiring:
-These defaults are meant to reduce first-power-on failure risk. They are **not** a claim of bit-exact equivalence to any proprietary legacy decoder.
-
-## Wiring
-
-- Connect source left/right/ground to the Audio Adaptor **LINE IN** header.
-- Connect amplifier, active speakers, or downstream headphone amp to **LINE OUT**.
-- Optionally monitor with the Audio Adaptor headphone jack.
-- Keep temporary 3.5 mm breakout wiring short to minimize hum and pickup.
-
-## Arduino IDE 2.x flash path
-
-1. Install Arduino IDE 2.x.
-2. Add `https://www.pjrc.com/teensy/package_teensy_index.json` in **Additional boards manager URLs**.
-3. Install the PJRC Teensy package.
-4. Open `ocx_type2_teensy41_decoder.ino`.
-5. Select:
-   - **Board:** Teensy 4.1
-   - **USB Type:** Serial
-   - **CPU Speed:** 600 MHz
-   - **Optimize:** Faster or Fastest
-6. Upload to the board.
-7. Open Serial Monitor at `115200` baud.
-
-## Reproducible command-line build path
-
-The repo also supports a real command-line build with PlatformIO.
-
-### Install tools
-
-```bash
-
-```
-
-### Build firmware for Teensy 4.1
-
-```bash
-pio run -e teensy41
-```
-
-This build targets:
-
-- board: **Teensy 4.1**
-- USB type: **Serial**
-- CPU speed: **600 MHz**
-- optimization intent: **Fastest**
-
-## Serial commands
-
-- `h` help
-- `p` print status
-- `x` clear clip flags
-- `B` reset DSP state
-- `b` bypass on/off
-- `0` reload factory preset
-- `i/I` input trim -/+ 0.5 dB
-- `o/O` output trim -/+ 0.5 dB
-- `s/S` strength -/+ 0.05
-- `f/F` reference dB -/+ 1 dB
-- `a/A` attack -/+ 0.5 ms
-- `r/R` release -/+ 5 ms
-- `c/C` sidechain HP -/+ 10 Hz
-- `q/Q` sidechain shelf gain -/+ 1 dB
-- `w/W` sidechain shelf freq -/+ 100 Hz
-- `e/E` de-emphasis gain -/+ 1 dB
-- `d/D` de-emphasis freq -/+ 50 Hz
-- `g/G` headroom -/+ 0.5 dB
-- `y/Y` DC block -/+ 1 Hz
-- `t` toggle built-in 1 kHz tone
-- `z/Z` tone level -/+ 1 dB
-
-## Offline WAV simulation
+Beispiel:
@@ -137 +73 @@ This build targets:
-python3 ocx_type2_wav_sim.py input.wav decoded.wav --plot
+python ocx_type2_harness.py --out-dir artifacts/harness --analysis-fs 8000 --tune --tune-mode refine --max-candidates 6 --top-k 3
@@ -140,8 +76 @@ python3 ocx_type2_wav_sim.py input.wav decoded.wav --plot
-Requirements:
-
-- WAV sample rate should be **44.1 kHz** for direct comparability with the Teensy audio path.
-
-
-## Automated harness / regression measurements
-
-Run the synthetic harness:
+Hinweis: Für schnelle Sweeps kann `--analysis-fs` reduziert werden; finale Plausibilisierung sollte zusätzlich bei 44.1 kHz erfolgen.
@@ -149,38 +78 @@ Run the synthetic harness:
-```bash
-python3 ocx_type2_harness.py --out-dir artifacts/harness --write-wavs
-```
-
-This covers at least the following classes:
-
-1. silence
-2. 1 kHz sine at multiple levels
-3. logarithmic sweep
-4. pink noise
-5. white noise
-6. bursts / sudden peaks
-7. slow envelope steps
-8. stereo-identical
-9. left/right different
-10. bass-heavy content
-11. treble-heavy content
-12. clipped input
-13. too-quiet input
... (truncated)
```

## FINAL_VALIDATION_ocx_type2_teensy.md
- Added lines: 64
- Removed lines: 217
```diff
diff --git a/FINAL_VALIDATION_ocx_type2_teensy.md b/FINAL_VALIDATION_ocx_type2_teensy.md
index 04d7e26..3538f6a 100644
--- a/FINAL_VALIDATION_ocx_type2_teensy.md
+++ b/FINAL_VALIDATION_ocx_type2_teensy.md
@@ -1 +1 @@
-# OCX Type 2 validation and audit report
+# OpenCompanderX – OCX Type 2 Validation Report
@@ -3 +3 @@
-## 1. Scope and honesty boundary
+## 1) Ehrlichkeitsgrenze
@@ -5 +5,2 @@
-This report separates four very different things:
+Dieser Stand liefert **Build-/Offline-Messbarkeit**, aber keine Referenz- oder Bitexact-Behauptung.
+Es lagen keine legalen proprietären Referenzausgänge im Repo vor.
@@ -7,4 +8 @@ This report separates four very different things:
-1. **real tool installation**,
-2. **real firmware build validation**,
-3. **real offline DSP execution and regression measurements**,
-4. **hardware-only unknowns that cannot be proven without a physical Teensy 4.1 + SGTL5000 setup**.
+## 2) Reparatur- und Stabilitätsstatus
@@ -12,2 +10 @@ This report separates four very different things:
-No bit-exact claim is made.
-No proprietary reference decoder was available inside this repo for black-box comparison.
+Repariert/erweitert:
@@ -15 +12,5 @@ No proprietary reference decoder was available inside this repo for black-box co
-## 2. Repo audit baseline
+1. Profil/Firmware-Sync (inkl. `codec.line_in_level`).
+2. Vollständiger Simulator mit numerischen Guards und CLI-Overrides.
+3. Harness als Abstimmungs-Harness inkl. zusätzlicher Stressfälle und Score-Funktion.
+4. Tests erweitert (inkl. 44.1-kHz-Pfad und Sync-Checks).
+5. Firmware-Telemetrie für reale CPU/RAM/Allocate/Clip-Stabilitätsprüfung.
@@ -17 +18 @@ No proprietary reference decoder was available inside this repo for black-box co
-### What was already good
+## 3) Firmware-Stabilität (offline + instrumentiert)
@@ -19,6 +20 @@ No proprietary reference decoder was available inside this repo for black-box co
-- Firmware structure was broadly plausible for a stereo decoder-only playback chain.
-- DSP work stayed inside a custom `AudioStream`, which is appropriate for Teensy real-time audio.
-- No heap allocation occurred inside `update()`.
-- Serial I/O stayed outside the audio callback.
-- SGTL5000 setup calls were syntactically valid for the Teensy Audio library.
-- The algorithm already used dual-mono detection, sidechain filtering, envelope tracking, expansion, de-emphasis, and soft limiting.
+### Realtime-Sicherheit
@@ -26 +22,3 @@ No proprietary reference decoder was available inside this repo for black-box co
-### What was risky or incomplete
+- Kein Heap-/malloc-Muster im Audio-Sample-Loop.
+- Keine `Serial`-Ausgabe in `update()`.
+- Allocate-Fehler in `update()` werden gezählt/markiert (statt still zu passieren).
@@ -28,7 +26 @@ No proprietary reference decoder was available inside this repo for black-box co
-- The simulator had a real runtime bug in plotting (`n`, `plot_audio`, and `plt` handling were incomplete).
-- Firmware and simulator defaults were duplicated, making silent drift likely.
-- The previous defaults were somewhat aggressive/dark for a one-profile universal analog target.
-- No automated regression harness existed for broad synthetic edge cases.
-- No formal pytest regression layer existed.
-- Documentation mixed practical guidance with implied validation that had not been reproducibly automated in-repo.
-- There was no reproducible command-line firmware build configuration in the repo itself.
+### Telemetrie (für echte Hardwareläufe)
@@ -36 +28 @@ No proprietary reference decoder was available inside this repo for black-box co
-### What needed changing before first hardware tests
+Statusausgabe zeigt:
@@ -38,5 +30,4 @@ No proprietary reference decoder was available inside this repo for black-box co
-1. Add a real reproducible build path.
-2. Synchronize defaults across firmware and simulator.
-3. Add regression coverage for silence, hot inputs, DC/rumble, stereo mismatch, and clipped material.
-4. Improve numerical safety around DC, NaN/Inf sanitation, and avoidable output overdrive.
-5. Re-document the exact limits of offline validation.
+- Audio-CPU current/max (`AudioProcessorUsage`, `AudioProcessorUsageMax`)
+- AudioMemory current/max (`AudioMemoryUsage`, `AudioMemoryUsageMax`)
+- Allocate-Failure-Flag + Counter
+- Input/Output-Clip-Flags + Counter
@@ -44 +35 @@ No proprietary reference decoder was available inside this repo for black-box co
-### What can wait until real hardware is present
+### RAM-Hinweis
@@ -46,4 +37 @@ No proprietary reference decoder was available inside this repo for black-box co
-- final gain staging against the three named source devices,
-- subjective listening refinement on actual encoded tapes,
-- any optional future black-box comparison against legal reference outputs,
-- final line-out/headphone loudness recommendations.
+`AudioMemory(64)` bleibt als konservativer Startwert; endgültige Reserve sollte auf echter Hardware über Max-Telemetrie verifiziert werden.
@@ -51 +39 @@ No proprietary reference decoder was available inside this repo for black-box co
-## 3. Installed tools and dependencies
+## 4) Offline-Abstimmungsmethodik
@@ -53 +41 @@ No proprietary reference decoder was available inside this repo for black-box co
-### Firmware/build
+Ziel: stabiler Universal-Decoder, **nicht** Referenzklon.
@@ -55,3 +43 @@ No proprietary reference decoder was available inside this repo for black-box co
-- PlatformIO installed successfully.
-- Teensy platform packages were downloaded and installed successfully through PlatformIO.
-- Teensy 4.1 firmware build completed successfully.
+Workflow:
@@ -59 +45,4 @@ No proprietary reference decoder was available inside this repo for black-box co
-### Python / analysis
+A) Baseline messen
+B) begrenzte Kandidaten-Sweeps
+C) Score-Vergleich
+D) plausibelsten Universal-Kandidaten auswählen
@@ -61 +50 @@ No proprietary reference decoder was available inside this repo for black-box co
-Installed and used:
+Score bestraft u. a.:
@@ -63,6 +52,5 @@ Installed and used:
-- Python 3
-- numpy
-- scipy
-- soundfile
-- pytest
-- pandas
+- häufiges Output-Clipping
+- hohe Gain-Schwankung
+- Kanalabweichung
+- starke spektrale/Transienten-Abweichung
+- Instabilität in Burst-/Envelope-/Rapid-Swing-Fällen
@@ -70 +58 @@ Installed and used:
-### Standard inspection tools available
+## 5) Durchgeführte Sweep-Ergebnisse (dieser Lauf)
@@ -72,5 +60 @@ Installed and used:
-- `file`
-- `strings`
-- `objdump`
-- `nm`
-- `git`
+Ausgeführt mit:
@@ -78 +62,3 @@ Installed and used:
-### Arduino CLI status
+```bash
+python ocx_type2_harness.py --out-dir artifacts/harness --analysis-fs 8000 --tune --tune-mode refine --max-candidates 6 --top-k 3
+```
@@ -80,3 +66 @@ Installed and used:
-- `arduino-cli` was present in the environment.
-- `arduino-cli core update-index` failed due network behavior inside that specific Go-based fetch path.
-- Because the task required real practical validation rather than a theoretical note, PlatformIO was used as an equivalent reproducible Teensy build environment and succeeded.
+Ergebnis:
@@ -84 +68,3 @@ Installed and used:
-## 4. Firmware build validation
+- Baseline-Score: `249.8292`
+- Bester Kandidat (aus 6 Kandidaten): Score `230.5766`
+- Dominanter Unterschied des besten Kandidaten: `reference_db = -16.0` (statt `-18.0`)
@@ -86 +72 @@ Installed and used:
-### Real build target
+Zusätzlicher 44.1-kHz-Subsetvergleich (kritische Fälle) bestätigte Richtung:
@@ -88 +74,2 @@ Installed and used:
-Validated in practice with:
+- Baseline: `204.6092`
+- `reference_db=-16.0`: `188.6646` (niedriger = besser im verwendeten Stabilitäts-Score)
@@ -90,4 +77 @@ Validated in practice with:
-- **Board:** Teensy 4.1
-- **USB Type:** Serial
-- **CPU Speed:** 600 MHz
-- **Optimization intent:** Fastest-style build flags via PlatformIO
+## 6) Empfohlenes Universalprofil (offline-basiert)
@@ -95 +79 @@ Validated in practice with:
-### Build result
+Empfohlen für diesen Stand:
@@ -97 +81,3 @@ Validated in practice with:
-The firmware built successfully into `firmware.elf` and `firmware.hex`.
+- `reference_db = -16.0`
+- übrige Kernparameter konservativ belassen
+- `line_in_level = 0`, `line_out_level = 29`
@@ -99 +85 @@ The firmware built successfully into `firmware.elf` and `firmware.hex`.
-This confirms, at compile/link level:
+Begründung: im dokumentierten Offline-Scoring stabiler als Baseline, ohne aggressive Zusatzänderungen.
@@ -101,5 +87 @@ This confirms, at compile/link level:
-- Teensy 4.1 compatibility,
-- includes resolve correctly,
-- Audio library objects resolve correctly,
-- SGTL5000 API calls used in the sketch exist in the installed library set,
-- no hidden type/API mismatch blocked the target build.
+## 7) Offen / nur auf echter Hardware prüfbar
@@ -107 +89,3 @@ This confirms, at compile/link level:
-## 5. Firmware changes made
+- finale Analog-Headroom-/Noise-Grenzen
+- Quellgeräte-spezifisches Feintuning
+- subjektive Hörqualität auf realem Material
@@ -109 +93 @@ This confirms, at compile/link level:
-### Robustness improvements
+## 8) Schlussbewertung
@@ -111,141 +95,4 @@ This confirms, at compile/link level:
-- Added a synchronized profile file for shared defaults.
-
-- Increased `AudioMemory` from 48 to 64 blocks for safer runtime margin.
-- Added a one-pole DC blocker before detector/audio gain application.
-- Added float sanitation in DSP paths to reduce NaN/Inf propagation risk.
-- Added explicit headroom gain in addition to soft clipping.
-- Added a DSP-state reset command and preset reset path that clears detector/filter history.
-
-### Universal-profile tuning changes
-
-The default profile was moved to a more conservative single-profile setting:
-
-
-- input trim: `-3 dB`
-- output trim: `-1 dB`
-- strength: `0.76`
-- attack: `3.5 ms`
-- release: `140 ms`
-- sidechain HP: `90 Hz`
-- sidechain shelf: `+16 dB @ 2.8 kHz`
-- de-emphasis: `-6 dB @ 1.85 kHz`
-- headroom: `1 dB`
-
-### Why these changes were technically justified
-
-
-- Slight output attenuation and dedicated headroom reduce unnecessary limiter engagement.
-- Higher sidechain HP reduces rumble/bass pumping.
-- Milder de-emphasis is a better universal starting point than a much darker shelf.
-- Resettable detector/filter state makes A/B tests and recovery from abrupt source changes more deterministic.
-
-## 6. Offline simulator and harness work
-
-### Simulator improvements
-
-- Reworked the simulator to load defaults from `ocx_type2_profile.json`.
-
-- Kept the same broad topology as firmware: dual-mono detector, sidechain filters, envelope detector, gain law, de-emphasis, soft clip.
-
-### New automated harness
-
-A new offline harness was added and executed across these categories:
-
-1. silence
-2. 1 kHz sine at multiple levels
... (truncated)
```

## ocx_type2_harness.py
- Added lines: 244
- Removed lines: 32
```diff
diff --git a/ocx_type2_harness.py b/ocx_type2_harness.py
index d486ba6..3a387b3 100755
--- a/ocx_type2_harness.py
+++ b/ocx_type2_harness.py
@@ -7,0 +8 @@ import json
+import itertools
@@ -11,0 +13,11 @@ import numpy as np
+from ocx_type2_wav_sim import (
+    DECODER_OVERRIDE_KEYS,
+    PROFILE_PATH,
+    Decoder,
+    Params,
+    db_to_lin,
+    ensure_stereo,
+    read_audio,
+    summarize_signal,
+    write_audio,
+)
@@ -33,2 +45,7 @@ def spectral_delta_db(a: np.ndarray, b: np.ndarray) -> float:
-    A = np.fft.rfft(a_mono * np.hanning(len(a_mono)))
-    B = np.fft.rfft(b_mono * np.hanning(len(b_mono)))
+    n = min(len(a_mono), len(b_mono))
+    if n == 0:
+        return 0.0
+    a_mono = a_mono[:n]
+    b_mono = b_mono[:n]
+    A = np.fft.rfft(a_mono * np.hanning(n))
+    B = np.fft.rfft(b_mono * np.hanning(n))
@@ -40,0 +58,5 @@ def transient_delta(a: np.ndarray, b: np.ndarray) -> float:
+    n = min(len(a), len(b))
+    if n == 0:
+        return 0.0
+    a = a[:n]
+    b = b[:n]
@@ -59,0 +82,25 @@ def gain_curve_stats(inp: np.ndarray, out: np.ndarray) -> dict[str, float]:
+def align_lengths(*arrays: np.ndarray) -> list[np.ndarray]:
+    valid = [a for a in arrays if a is not None]
+    n = min(len(a) for a in valid) if valid else 0
+    aligned = []
+    for a in arrays:
+        aligned.append(None if a is None else a[:n])
+    return aligned
+
+
+def compare(inp: np.ndarray, out: np.ndarray, ref: np.ndarray | None = None) -> dict[str, float | None]:
+    inp = ensure_stereo(inp)
+    out = ensure_stereo(out)
+    inp_a, out_a, ref_a = align_lengths(inp, out, ref)
+
+    residual = out_a - inp_a
+    metrics: dict[str, float | None] = {
+        "residual_rms": float(np.sqrt(np.mean(np.square(residual)))),
+        **gain_curve_stats(inp_a, out_a),
+        "mse": float(np.mean(np.square(residual))),
+        "mae": float(np.mean(np.abs(residual))),
+        "max_abs_error": float(np.max(np.abs(residual))),
+        "correlation": correlation(inp_a, out_a),
+        "freq_response_delta_db": spectral_delta_db(inp_a, out_a),
+        "transient_delta": transient_delta(inp_a, out_a),
+    }
@@ -61 +108,13 @@ def gain_curve_stats(inp: np.ndarray, out: np.ndarray) -> dict[str, float]:
-        })
+    if ref_a is not None:
+        diff = out_a - ref_a
+        metrics.update(
+            {
+                "mse_vs_reference": float(np.mean(np.square(diff))),
+                "mae_vs_reference": float(np.mean(np.abs(diff))),
+                "max_abs_error_vs_reference": float(np.max(np.abs(diff))),
+                "correlation_vs_reference": correlation(out_a, ref_a),
+                "null_residual_rms_vs_reference": float(np.sqrt(np.mean(np.square(diff)))),
+                "freq_response_delta_db_vs_reference": spectral_delta_db(out_a, ref_a),
+                "transient_delta_vs_reference": transient_delta(out_a, ref_a),
+            }
+        )
@@ -63,9 +122,11 @@ def gain_curve_stats(inp: np.ndarray, out: np.ndarray) -> dict[str, float]:
-        metrics.update({
-            "mse_vs_reference": None,
-            "mae_vs_reference": None,
-            "max_abs_error_vs_reference": None,
-            "correlation_vs_reference": None,
-            "null_residual_rms_vs_reference": None,
-            "freq_response_delta_db_vs_reference": None,
-            "transient_delta_vs_reference": None,
-        })
+        metrics.update(
+            {
+                "mse_vs_reference": None,
+                "mae_vs_reference": None,
+                "max_abs_error_vs_reference": None,
+                "correlation_vs_reference": None,
+                "null_residual_rms_vs_reference": None,
+                "freq_response_delta_db_vs_reference": None,
+                "transient_delta_vs_reference": None,
+            }
+        )
@@ -74,0 +136,24 @@ def gain_curve_stats(inp: np.ndarray, out: np.ndarray) -> dict[str, float]:
+def score_run(metrics: dict[str, float | bool | None]) -> float:
+    score = 0.0
+    output_peak = float(metrics["output_peak"])
+    gain_std = float(metrics["gain_curve_std_db"])
+    channel_delta = float(metrics["output_channel_delta_max"])
+    freq_delta = float(metrics["freq_response_delta_db"])
+    transient = float(metrics["transient_delta"])
+
+    score += 8.0 if metrics["output_clip_l"] else 0.0
+    score += 8.0 if metrics["output_clip_r"] else 0.0
+    score += 4.0 * max(0.0, output_peak - 0.98)
+    score += 0.8 * gain_std
+    score += 50.0 * channel_delta
+    score += 0.05 * freq_delta
+    score += 2.0 * transient
+
+    case = str(metrics["case"])
+    if case in {"bursts", "envelope_steps", "hf_bursts", "transient_train", "rapid_level_swings"}:
+        score += 0.6 * gain_std + 1.5 * transient
+    if case in {"too_hot", "clipped_input", "bass_plus_hf"}:
+        score += 2.0 * max(0.0, output_peak - 0.95)
+    return score
+
+
@@ -150,0 +236,33 @@ def music_like(fs: int, seconds: float) -> np.ndarray:
+def hf_bursts(fs: int, seconds: float) -> np.ndarray:
+    n = int(fs * seconds)
+    out = np.zeros(n)
+    burst = tone(fs, 0.03, 8500.0, -8.0)
+    gap = int(fs * 0.07)
+    idx = 0
+    while idx + len(burst) < n:
+        out[idx : idx + len(burst)] = burst
+        idx += len(burst) + gap
+    return out
+
+
+def bass_plus_hf(fs: int, seconds: float) -> np.ndarray:
+    return 0.7 * bass_heavy(fs, seconds) + 0.6 * treble_heavy(fs, seconds)
+
+
+def transient_train(fs: int, seconds: float) -> np.ndarray:
+    n = int(fs * seconds)
+    out = np.zeros(n)
+    click_len = max(1, int(0.001 * fs))
+    step = int(fs * 0.05)
+    for idx in range(0, n - click_len, step):
+        out[idx : idx + click_len] = 0.9
+    return out
+
+
+def rapid_level_swings(fs: int, seconds: float) -> np.ndarray:
+    t = np.arange(int(fs * seconds)) / fs
+    carrier = np.sin(2 * np.pi * 1200.0 * t)
+    env = 0.15 + 0.75 * (np.sin(2 * np.pi * 3.8 * t) > 0).astype(np.float64)
+    return carrier * env
+
+
@@ -174,0 +293,4 @@ def build_cases(fs: int) -> dict[str, np.ndarray]:
+        "hf_bursts": ensure_stereo(hf_bursts(fs, 3.0)),
+        "bass_plus_hf": ensure_stereo(bass_plus_hf(fs, 4.0)),
+        "transient_train": ensure_stereo(transient_train(fs, 3.0)),
+        "rapid_level_swings": ensure_stereo(rapid_level_swings(fs, 4.0)),
@@ -178,15 +300,8 @@ def build_cases(fs: int) -> dict[str, np.ndarray]:
-def main() -> None:
-    ap = argparse.ArgumentParser()
-    ap.add_argument("--out-dir", type=Path, default=Path("artifacts/harness"))
-    ap.add_argument("--profile", type=Path, default=PROFILE_PATH)
-    ap.add_argument("--reference-dir", type=Path)
-    ap.add_argument("--write-wavs", action="store_true")
-    args = ap.parse_args()
-
-    profile = json.loads(args.profile.read_text())
-    fs = int(profile["sample_rate_hz"])
-    cases = build_cases(fs)
-    params = Params.from_profile(args.profile)
-    args.out_dir.mkdir(parents=True, exist_ok=True)
-
-    results = []
+def evaluate_candidate(
+    fs: int,
+    cases: dict[str, np.ndarray],
+    params: Params,
+    reference_dir: Path | None = None,
+) -> tuple[list[dict[str, float | bool | None]], float]:
+    results: list[dict[str, float | bool | None]] = []
+    total_score = 0.0
@@ -197,4 +312,6 @@ def main() -> None:
-        ref_path = args.reference_dir / f"{name}.wav" if args.reference_dir else None
-        if ref_path and ref_path.exists():
-            ref_fs, ref_audio = read_audio(ref_path)
-            if ref_fs == fs:
+        if reference_dir:
+            ref_path = reference_dir / f"{name}.wav"
+            if ref_path.exists():
+                ref_fs, ref_audio = read_audio(ref_path)
+                if ref_fs == fs:
+                    ref = ref_audio
@@ -212,0 +330 @@ def main() -> None:
+        total_score += score_run(metrics)
@@ -213,0 +332,68 @@ def main() -> None:
+    return results, total_score
+
+
+def sweep_space() -> dict[str, list[float]]:
+    return {
+        "strength": [0.72, 0.76, 0.80],
+        "reference_db": [-20.0, -18.0, -16.0],
+        "attack_ms": [2.5, 3.5, 5.0],
+        "release_ms": [120.0, 140.0, 180.0],
+        "sidechain_hp_hz": [70.0, 90.0, 120.0],
+        "sidechain_shelf_hz": [2400.0, 2800.0, 3400.0],
+        "sidechain_shelf_db": [14.0, 16.0, 18.0],
+        "deemph_hz": [1600.0, 1850.0, 2200.0],
+        "deemph_db": [-7.0, -6.0, -5.0],
+        "soft_clip_drive": [1.04, 1.08, 1.12],
+        "headroom_db": [0.8, 1.0, 1.4],
+        "input_trim_db": [-3.5, -3.0, -2.5],
+        "output_trim_db": [-1.5, -1.0, -0.5],
+    }
+
+
+def candidate_overrides_for_mode(mode: str) -> list[dict[str, float]]:
... (truncated)
```

## tests/test_ocx_type2.py
- Added lines: 114
- Removed lines: 5
```diff
diff --git a/tests/test_ocx_type2.py b/tests/test_ocx_type2.py
index 6955a5e..c64409a 100644
--- a/tests/test_ocx_type2.py
+++ b/tests/test_ocx_type2.py
@@ -0,0 +1,5 @@
+import os
+import re
+import subprocess
+import sys
+from pathlib import Path
@@ -1,0 +7,17 @@
+import numpy as np
+
+ROOT = Path(__file__).resolve().parents[1]
+if str(ROOT) not in sys.path:
+    sys.path.insert(0, str(ROOT))
+
+from ocx_type2_harness import build_cases, compare
+from ocx_type2_wav_sim import PROFILE_PATH, Decoder, Params, ensure_stereo
+
+
+def _firmware_constants() -> dict[str, float]:
+    ino = (ROOT / "ocx_type2_teensy41_decoder.ino").read_text()
+    pairs = re.findall(r"k([A-Za-z0-9_]+)\s*=\s*([-0-9.]+)f?;", ino)
+    out: dict[str, float] = {}
+    for key, value in pairs:
+        out[key] = float(value)
+    return out
@@ -10 +32 @@ def test_profile_defaults_load():
-def test_decoder_finite_on_all_harness_cases():
+def test_profile_and_firmware_sync_for_core_defaults():
@@ -12 +34,23 @@ def test_decoder_finite_on_all_harness_cases():
-    fs = 44100
+    firmware = _firmware_constants()
+
+    assert firmware["LineInLevel"] == 0.0
+    assert firmware["LineOutLevel"] == 29.0
+    assert firmware["InputTrimDb"] == params.input_trim_db
+    assert firmware["OutputTrimDb"] == params.output_trim_db
+    assert firmware["Strength"] == params.strength
+    assert firmware["ReferenceDb"] == params.reference_db
+    assert firmware["AttackMs"] == params.attack_ms
+    assert firmware["ReleaseMs"] == params.release_ms
+    assert firmware["SidechainHpHz"] == params.sidechain_hp_hz
+    assert firmware["SidechainShelfHz"] == params.sidechain_shelf_hz
+    assert firmware["SidechainShelfDb"] == params.sidechain_shelf_db
+    assert firmware["DeemphHz"] == params.deemph_hz
+    assert firmware["DeemphDb"] == params.deemph_db
+    assert firmware["SoftClipDrive"] == params.soft_clip_drive
+    assert firmware["DcBlockHz"] == params.dc_block_hz
+    assert firmware["HeadroomDb"] == params.headroom_db
+
+
+def test_decoder_finite_on_all_harness_cases_low_rate():
+    params = Params.from_profile(PROFILE_PATH)
+    fs = 4000
@@ -15,0 +60,7 @@ def test_decoder_finite_on_all_harness_cases():
+
+
+def test_output_bounded_on_all_harness_cases_low_rate():
+    params = Params.from_profile(PROFILE_PATH)
+    fs = 4000
+    for name, audio in build_cases(fs).items():
+        out = Decoder(fs, params).process(audio)
@@ -19 +70 @@ def test_decoder_finite_on_all_harness_cases():
-def test_stereo_identity_case_remains_balanced():
+def test_production_rate_path_44100_finite_and_bounded():
@@ -21,2 +72,48 @@ def test_stereo_identity_case_remains_balanced():
-    audio = build_cases(44100)["stereo_identical"]
-    out = Decoder(44100, params).process(audio)
+    fs = 44100
+    audio = np.sin(2 * np.pi * 1000 * np.arange(fs // 2) / fs)
+    out = Decoder(fs, params).process(audio)
+    assert np.isfinite(out).all()
+    assert np.max(np.abs(out)) <= 1.0 + 1e-9
+
+
+def test_stereo_identical_case_remains_balanced():
+    params = Params.from_profile(PROFILE_PATH)
+    audio = build_cases(4000)["stereo_identical"]
+    out = Decoder(4000, params).process(audio)
+    assert np.allclose(out[:, 0], out[:, 1], atol=1e-9)
+
+
+def test_ensure_stereo_accepts_supported_shapes():
+    mono = np.array([0.1, -0.2, 0.3], dtype=np.float64)
+    mono_2d = mono[:, None]
+    stereo = np.column_stack([mono, -mono])
+
+    out_mono = ensure_stereo(mono)
+    out_mono_2d = ensure_stereo(mono_2d)
+    out_stereo = ensure_stereo(stereo)
+
+    assert out_mono.shape == (3, 2)
+    assert out_mono_2d.shape == (3, 2)
+    assert out_stereo.shape == (3, 2)
+
+
+def test_decoder_process_accepts_mono_shapes():
+    params = Params.from_profile(PROFILE_PATH)
+    decoder = Decoder(4000, params)
+    mono = np.sin(2 * np.pi * 440 * np.arange(1000) / 4000)
+    out_a = decoder.process(mono)
+    decoder.reset()
+    out_b = decoder.process(mono[:, None])
+
+    assert out_a.shape == (1000, 2)
+    assert out_b.shape == (1000, 2)
+
+
+def test_harness_compare_handles_reference_length_mismatch():
+    inp = ensure_stereo(np.sin(2 * np.pi * 220 * np.arange(1000) / 4000))
+    out = inp * 0.8
+    ref = ensure_stereo(np.sin(2 * np.pi * 220 * np.arange(700) / 4000))
+    metrics = compare(inp, out, ref)
+    assert metrics["mse_vs_reference"] is not None
+    assert np.isfinite(metrics["mse_vs_reference"])
+
@@ -23,0 +121,12 @@ def test_stereo_identity_case_remains_balanced():
+def test_help_works_without_matplotlib_dependency_for_non_plot_path():
+    env = os.environ.copy()
+    env["PYTHONPATH"] = str(ROOT)
+    result = subprocess.run(
+        [sys.executable, str(ROOT / "ocx_type2_wav_sim.py"), "--help"],
+        check=False,
+        capture_output=True,
+        text=True,
+        env=env,
+    )
+    assert result.returncode == 0
+    assert "--plot" in result.stdout
```
