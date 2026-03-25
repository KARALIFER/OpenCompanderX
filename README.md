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
- Referenzlose Score-Logik bewertet zusätzlich die **Pegel-Tracking-Plausibilität** über Gain-vs-Input-Slope/R² und bestraft zu flaches oder unplausibles Tracking.
- Tuning verwendet eine **zweistufige Suche**: schnelle Grobselektion (z. B. 4 kHz) plus finales Re-Ranking bei **44.1 kHz**.
- Simulator kann zusätzlich einen **RMS-näheren Detectorpfad** (`detector_mode=rms`) gegen den bisherigen energy-nahen Pfad vergleichen.
- Harness enthält jetzt einen expliziten **cassette-primary** Prüfpfad mit Pflicht-Frequenz-/Pegel-Matrix, musiknahen Verbundfällen und breitbandigen Fällen.

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
- `tone.frequency_hz = 400.0`
- `tone.level_dbfs = -9.8`

## Kalibrierpegel vs. Decoder-Referenz (bewusst getrennt)

- `tone.level_dbfs` ist der **Kalibrier-Testtonpegel** für den realen Deck-/Band-Abgleich.
- `decoder.reference_db` ist ein **interner Modellparameter** der Decoder-Regelung.

Aktueller Hardware-Bezugspunkt für das hier dokumentierte Nutzer-Setup
(Mixtape Nerd + TEAC W-1200 + RTM-Band):

- 0 VU / Tape-Referenz liegt praktisch bei ca. **-9.8 dBFS** (rundbar auf -10 dBFS).
- Deshalb steht der Default-Testton bei **400 Hz @ -9.8 dBFS**.
- Das ist ein **Setup-spezifischer Messwert**, kein universeller historischer dbx-Standard.

`decoder.reference_db` bleibt vorerst bei `-18.0`, bis Simulator-/Harness-/Hardwarevergleich
eine belastbare, getrennte Umstellung dieses Modellparameters rechtfertigt.

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

### Cassette-primary Referenzlayout (empfohlen)

Klare Trennung:

- `refs/type2_cassette_real/` = echte, legal nutzbare Referenzen mit belegter Herkunft/Lizenz
- `refs/type2_cassette_synth/` = synthetische/approximative Referenzen (reproduzierbar generiert)

Pro Fall:

- `<name>_encoded.wav` (Pflicht)
- `<name>_source.wav` (Pflicht)
- `<name>_reference_decode.wav` (optional)
- `<name>.json` (Pflicht-Metadaten: Herkunft, Lizenz, Trust-Level, source_type)

Nur Fälle mit mindestens `*_encoded.wav` + `*_source.wav` werden als `cassette_reference` in die Bewertung aufgenommen.
Diese Struktur verbessert die Praxisnähe, beweist aber allein noch keine historische Standardgleichheit.

Aktuell enthält das Repo standardmäßig **keine echten lizenzierten dbx-Type-II-Referenzaufnahmen**. Deshalb ist die mitgelieferte Primärbasis zunächst synthetisch/approximativ und entsprechend gekennzeichnet.

Bekannte bereitgestellte Kandidaten können separat importiert werden:

- `musik_enc.wav` (wenn vorhanden): wird für den Offline-Pfad auf 44.1 kHz resampelt und als `encoded_candidate_only` geführt.
- `musicfox_shopping_street.mp3` (wenn decodierbar): wird ebenfalls auf 44.1 kHz gebracht, aber nur als Zusatz-/Stressfall geführt.

Beide Fälle gelten ohne dokumentierten Encoder-/Lizenzpfad **nicht** als harte Goldreferenz.

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

# Synthetisches Referenzpaket erzeugen (reproduzierbar)
python ocx_type2_harness.py --generate-synth-refs --reference-dir refs --out-dir artifacts/harness_refs

# Reale Referenzen indexieren (falls legal vorhanden)
python ocx_type2_harness.py --index-real-refs --reference-dir refs --out-dir artifacts/harness_refs

# Bekannte lokale Musik-Kandidaten importieren (falls Dateien lokal vorhanden sind)
python ocx_type2_harness.py --prepare-known-music-candidates --reference-dir refs --out-dir artifacts/harness_refs

# Optional: reale Referenzen aus Manifest laden (nur rechtlich saubere URLs)
python ocx_type2_harness.py --fetch-real-refs-manifest refs/type2_cassette_real/manifest.example.json --reference-dir refs --out-dir artifacts/harness_refs

# Bewertung nur cassette-priority Fälle, getrennt nach Source-Typ (real/synthetic/all)
python ocx_type2_harness.py --out-dir artifacts/harness_ref --reference-dir refs --cassette-priority-only --reference-source all

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
3. Kalibrierung: 400-Hz-Testton bei `tone.level_dbfs=-9.8` einspeisen und auf den
   eigenen 0-VU-/Ref.-Punkt des Deck-Workflows abgleichen.
4. Definierte Testquelle abspielen (mehrere Minuten).
5. Mit `m` zyklisch kompakten Status lesen, mit `p` Vollstatus prüfen.
6. Bewertung: CPU-/Memory-Reserve OK, `allocFailCount == 0`, Clip-Zähler plausibel zu Eingangspegel/Headroom.

Die kompakte `m`-Zeile enthält dafür explizit `cpuRes=OK/TIGHT` und `memRes=OK/TIGHT` neben den Clip-/Alloc-Zählern.

## Testkassetten-Methodik

- **Kassette A (nicht compandiert, Grundreferenz):** 400 Hz (hier: -9.8 dBFS als
  Setup-spezifischer 0-VU-Bezug), 1 kHz, 10 kHz, 3.15 kHz für Pegel/Kanalgleichheit/HF-/Azimuth/Speed.
- **Kassette B (Type-II encodiert, Decoder-Tuning):** Mehrpegel-1-kHz, Bursts, Envelope-Steps, Pink/White Noise, Sweep, Bass+HF, Musik.
- **Cassette-primary Harness (offline):**
  - Einzelton-/Mehrpegel-Matrix: **400 Hz, 1 kHz, 3.15 kHz, 10 kHz** über mehrere Pegel.
  - Mehrfrequenz-/musiknahe Verbundfälle: Two-Tone, Bass+HF, Burst-/Transient-Train, Fast-Level-Switches, `music_like`.
  - Breitbandfälle: Pink/White-Noise, Log-Sweep.
- Immer genau ein Type-II-Encoding-Pfad verwenden (keine Doppel-Encodierung).

## Claims

Dieses Projekt macht **keine** Bit-Exact-/Originalgleich-/Referenzgleich-Behauptung ohne harten Messbeleg.
Auch der erweiterte cassette-primary Harness erhöht primär die praktische Validierungstiefe; er ersetzt keine vollständig dokumentierte dbx-Type-II-Normkonformitätsmessung.

## Ehrlicher Kompatibilitätsstatus (dbx Type II Cassette)

- Ziel bleibt maximale praktische Decoder-Kompatibilität für reale Type-II-Kassetten im genannten Hardware-Setup.
- Der aktuelle Stand ist ein methodisch abgestimmter, stabiler Decoderpfad mit Type-II-orientierter Ballistik/Sidechain-Formung,
  aber **nicht** als historisch standardgenauer dbx-Type-II-Decoder belegt.
- Disc-spezifische Annahmen (z. B. implizite LF-Roll-off-Übernahmen) werden nicht stillschweigend als Cassette-Default übernommen.
- Offene Restabweichungen werden über Harness- und Hardware-Messungen geführt, nicht durch Gleichheits-Claims kaschiert.
