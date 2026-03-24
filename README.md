# OpenCompanderX / OCX Type 2

Dieses Repository enthält einen robusten **Type-II Compander-Decoder** für eine feste Zielhardware:

- Teensy 4.1
- Teensy Audio Adaptor Rev D/D2 (SGTL5000)
- Stereo analog Line-In zu Stereo analog Line-Out/Headphone

## Status (ehrlich und reproduzierbar)

Aktuell im Repo verifizierbar:

- Firmware kompiliert mit PlatformIO für `teensy41`.
- Offline-Simulator läuft für WAV-Dateien.
- Harness trennt jetzt explizit:
  - **referenzlose Stabilitäts-/Plausibilitätsbewertung** (Clipping, Kanalabweichung, Ballistik-/Pump-Indikatoren, spektrale Verfärbung, Transientenverlust, Soft-Clip-Abhängigkeit)
  - **optionale Referenzbewertung** (nur wenn echte Referenzdateien vorhanden sind).
- Tuning verwendet eine **zweistufige Suche**: schnelle Grobselektion (z. B. 4 kHz) plus finales Re-Ranking bei **44.1 kHz**.
- Simulator kann zusätzlich einen **RMS-näheren Detectorpfad** (`detector_mode=rms`) gegen den bisherigen energy-nahen Pfad vergleichen.

Nicht offline beweisbar (nur echte Hardware):

- Analoges Headroom-/Noise-Verhalten der SGTL5000-Stufe.
- Endgültige Abstimmung für konkrete Zuspieler (z. B. TEAC W-1200, FiiO CP13, We Are Rewind).
- Subjektive Klangbeurteilung realer Type-II-encodierter Inhalte.

## Wichtige Dateien

- `ocx_type2_teensy41_decoder.ino` – Firmware
- `ocx_type2_profile.json` – gemeinsames Profil für Firmware + Simulator
- `ocx_type2_wav_sim.py` – Offline-Simulator
- `ocx_type2_harness.py` – Regression/Harness/Tuning
- `tests/test_ocx_type2.py` – Tests
- `FINAL_VALIDATION_ocx_type2_teensy.md` – methodische Validierung und Hardwareablauf

## Profilprinzip

Es gibt **ein konservatives Universalprofil** für die feste Zielhardware.
Kein Mehrprofil-System, keine Auto-Erkennung.

Aktueller Stand `ocx_type2_universal_v2`:

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

## Methodik: digitale Referenz vs. analoges Ziel

### 1) Digitaler Referenzpfad (Black-Box-Vergleich)

Beispiel: USB-Ausspielung/PC-Referenzdecoder/Simulator.
Hier werden Decoderlogik, Ballistik und Type-II-Tendenzen bewertet.
**Nicht** Teil dieses Pfads: SGTL5000 `line_in_level`, analoger Frontend-Headroom.

### 2) Analoger Zielpfad (reale Teensy-Hardware)

Beispiel: analog out (TEAC/FiiO/WAR) -> Teensy Line-In -> OCX -> analog out.
Hier zählen: `line_in_level`, Input-Trim, Clipping-Reserven, Noise/Hum, Laufzeitstabilität.

Diese Pfade dürfen methodisch nicht vermischt werden.

## Referenzmaterial (kurz)

- **Kassette A (uncompanded)**: gut für Pegel-/Linearity-/Headroom-Kontrolle, nicht ausreichend für Type-II-Decoderabgleich.
- **Kassette B (Type-II-encodiert)**: nötig für echte Decoderabstimmung.

Für Black-Box-Referenzvergleich gilt:

- Referenzdateien werden längenangepasst verglichen.
- MSE/MAE/Korrelation/Frequenz-/Transientenvergleich werden separat als Referenzscore erfasst.
- Ohne Referenz wird **keine** Referenznähe behauptet.
- Optionale Hilfen wie Play Trim, Azimuth-Korrektur, Gap-Loss-Kompensation oder EQ-Konvertierung (IEC 120 µs <-> 70 µs) gelten als Referenzpfad-Hilfen, nicht als implizite OCX-Kernlogik.

## Lokale Checks

```bash
python -m py_compile ocx_type2_wav_sim.py
python -m py_compile ocx_type2_harness.py
python -m py_compile tests/test_ocx_type2.py
pytest -q
pio run -e teensy41
```

## Harness- und Tuning-Beispiele

```bash
# Standard-Offline-Bewertung
python ocx_type2_harness.py --out-dir artifacts/harness

# Optional: Referenzmaterial (name.wav pro Harness-Case)
python ocx_type2_harness.py --out-dir artifacts/harness_ref --reference-dir refs/type2

# Decoder-Overrides
python ocx_type2_harness.py --override strength=0.80 --override release_ms=160

# Zweistufiges Tuning: coarse (4 kHz) + final (44.1 kHz)
python ocx_type2_harness.py --tune --tune-fs 4000 --tune-final-fs 44100 --tune-top-k 6

# Detector-Methodikvergleich (energy vs. RMS-näher)
python ocx_type2_harness.py --detector-study --out-dir artifacts/harness_detector
```

## Hardware-Telemetrie und Auswertung

Firmware-Kommandos:

- `p` = voller Status
- `m` = kompakter Telemetrie-Status
- `X` = Clip-/Runtime-Counter und Usage-Max zurücksetzen
- `0` = Factory-Preset neu laden

Telemetrie-Bedeutung:

- `AudioProcessorUsage()` / `AudioProcessorUsageMax()` -> aktuelle und maximale DSP-CPU-Last
- `AudioMemoryUsage()` / `AudioMemoryUsageMax()` -> aktuell/genutzte Audio-Blöcke
- `allocFailCount` -> Audio-Block-Allokation fehlgeschlagen (muss 0 bleiben)
- `inputClipCount` -> Frontend/Decoder-Eingang übersteuert
- `outputClipCount` -> Ausgangspfad übersteuert

Empfohlener realer Hardware-Ablauf:

1. Booten und per `0` Factory-Preset laden.
2. Mit `X` Clip-/Runtime-Zähler und Maxima zurücksetzen.
3. Definierte Testquelle abspielen (mehrere Minuten).
4. Mit `m` zyklisch kompakten Status lesen, mit `p` Vollstatus prüfen.
5. Bewertung: CPU-/Memory-Reserve OK, `allocFailCount == 0`, Clip-Zähler plausibel zu Eingangspegel/Headroom.

## Testkassetten-Methodik

- **Kassette A (nicht compandiert, Grundreferenz):** 400 Hz, 1 kHz, 10 kHz, 3.15 kHz für Pegel/Kanalgleichheit/HF-/Azimuth/Speed.
- **Kassette B (Type-II encodiert, Decoder-Tuning):** Mehrpegel-1-kHz, Bursts, Envelope-Steps, Pink/White Noise, Sweep, Bass+HF, Musik.
- Immer genau ein Type-II-Encoding-Pfad verwenden (keine Doppel-Encodierung).

## Claims

Dieses Projekt macht **keine** Bit-Exact-/Originalgleich-/Referenzgleich-Behauptung ohne harten Messbeleg.
