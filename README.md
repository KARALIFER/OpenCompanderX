# OpenCompanderX / OCX Type 2

Dieses Repository enthält einen robusten **Type-II-Compander-Decoder** für eine fest definierte Zielhardware:

- Teensy 4.1
- Teensy Audio Adaptor Rev D/D2 (SGTL5000)
- Stereo analog Line-In zu Stereo analog Line-Out/Headphone

## Aktueller, reproduzierbarer Stand

Der derzeit im Repository nachvollziehbare Stand umfasst:

- Die Firmware kompiliert mit PlatformIO für `teensy41`.
- Der Firmware-Decoderpfad arbeitet explizit **stereo-gekoppelt**: ein gemeinsamer Detector auf Basis von `max(L/R)` der Sidechain-Power sowie gemeinsamer Gain. Dadurch wird Stereo-Image-Wander bei asymmetrischem Material reduziert.
- Die Output-Finalisierung ist zentralisiert (`finalizeOutput`): Output-Trim, Headroom, Softclip und Clip-Zählung laufen konsistent über Decode- und Bypass-Pfad.
- Ein Offline-Simulator für WAV-Dateien ist vorhanden.
- Der Harness trennt explizit zwischen:
  - **referenzloser Stabilitäts- und Plausibilitätsbewertung** (Clipping, Kanalabweichung, Ballistik-/Pump-Indikatoren, spektrale Verfärbung, Transientenverlust, Soft-Clip-Abhängigkeit)
  - **optionalem Referenzvergleich** (nur bei vorhandenen echten Referenzdateien)
- Die referenzlose Score-Logik bewertet zusätzlich die **Pegel-Tracking-Plausibilität** über Gain-vs-Input-Slope/R² und gewichtet zu flaches oder unplausibles Tracking negativ.
- Das Tuning erfolgt über eine **zweistufige Suche**: schnelle Grobselektion (z. B. 4 kHz) und finales Re-Ranking bei **44.1 kHz**.
- Der Simulator kann zusätzlich einen **RMS-näheren Detectorpfad** (`detector_mode=rms`) mit dem bisherigen energy-nahen Pfad vergleichen.
- Der Harness enthält einen expliziten **cassette-primary**-Prüfpfad mit Pflicht-Frequenz-/Pegel-Matrix, musiknahen Verbundfällen und breitbandigen Fällen.

## Hardwarebezogene Punkte

Folgende Aspekte können nur auf realer Hardware abschließend beurteilt werden:

- Analoges Headroom- und Noise-Verhalten der SGTL5000-Stufe
- Endgültige Abstimmung für konkrete Zuspieler (z. B. TEAC W-1200, FiiO CP13, We Are Rewind)
- Subjektive Klangbeurteilung realer Type-II-encodierter Inhalte

## Wichtige Dateien

- `ocx_type2_teensy41_decoder.ino` – Firmware
- `ocx_type2_profile.json` – gemeinsames Profil für Firmware und Simulator
- `ocx_type2_wav_sim.py` – Offline-Simulator
- `ocx_type2_harness.py` – Regression, Harness und Tuning
- `tests/test_ocx_type2.py` – Tests
- `FINAL_VALIDATION_ocx_type2_teensy.md` – methodische Validierung und Hardwareablauf

## Profilprinzip

Es gibt **ein konservatives Universalprofil** für die fest definierte Zielhardware.  
Ein Mehrprofil-System oder eine Auto-Erkennung sind nicht vorgesehen.

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

Hinweis zur Samplerate: Profil und Harness arbeiten mit **44.1 kHz nominal**.  
Die Teensy-Firmware läuft zur Laufzeit mit `AUDIO_SAMPLE_RATE_EXACT` (leicht abweichender exakter I2S-Takt), ohne dass daraus automatisch andere Profilparameter abgeleitet werden.

## Kalibrierpegel und Decoder-Referenz

Diese beiden Werte sind bewusst voneinander getrennt:

- `tone.level_dbfs` ist der **Kalibrier-Testtonpegel** für den realen Deck-/Band-Abgleich.
- `decoder.reference_db` ist ein **interner Modellparameter** der Decoder-Regelung.

In der Firmware wird der Kalibrierton **post-decoder** in den Ausgangsmix eingespeist.  
Er dient damit der Ausgangs- und Workflow-Kalibrierung, nicht als Decoder-Eingangstestton.

Auch `bypass` ist bewusst **kein transparenter Hard-Relay-Bypass**: Headroom und Softclip bleiben aktiv, damit der analoge Ausgangsschutz auch im Bypass-Betrieb erhalten bleibt.

### Aktueller Hardware-Bezugspunkt für das dokumentierte Nutzer-Setup
(Mixtape Nerd + TEAC W-1200 + RTM-Band)

- 0 VU bzw. Tape-Referenz liegt praktisch bei ca. **-9.8 dBFS**  
  (rundbar auf -10 dBFS)
- Deshalb ist der Default-Testton auf **400 Hz @ -9.8 dBFS** gesetzt

Dabei handelt es sich um einen **setup-spezifischen Messwert**, nicht um einen universellen historischen dbx-Standard.

`decoder.reference_db` bleibt vorerst bei `-18.0`, bis Simulator-, Harness- und Hardwarevergleich eine belastbare, davon unabhängige Anpassung dieses Modellparameters rechtfertigen.

## Methodik: digitale Referenz und analoges Ziel

### 1) Digitaler Referenzpfad (Black-Box-Vergleich)

Beispiel: USB-Ausspielung, PC-Referenzdecoder oder Simulator.  
In diesem Pfad werden Decoderlogik, Ballistik und Type-II-Tendenzen bewertet.

**Nicht Teil dieses Pfads** sind:

- SGTL5000 `line_in_level`
- analoger Frontend-Headroom

### 2) Analoger Zielpfad (reale Teensy-Hardware)

Beispiel: analog out (TEAC/FiiO/WAR) → Teensy Line-In → OCX → analog out

Hier stehen im Fokus:

- `line_in_level`
- Input-Trim
- Clipping-Reserven
- Noise/Hum
- Laufzeitstabilität

Beide Pfade werden methodisch klar getrennt betrachtet und nicht miteinander vermischt.

## Referenzmaterial

### Grundsätzliches

- **Kassette A (uncompanded)** eignet sich gut für Pegel-, Linearitäts- und Headroom-Kontrolle, ist aber allein nicht ausreichend für den Type-II-Decoderabgleich.
- **Kassette B (Type-II-encodiert)** ist für eine echte Decoderabstimmung erforderlich.

Für den Black-Box-Referenzvergleich gilt:

- Referenzdateien werden längenangepasst verglichen.
- MSE, MAE, Korrelation sowie Frequenz- und Transientenvergleich werden separat als Referenzscore erfasst.
- Ohne Referenz wird **keine Aussage über Referenznähe** getroffen.
- Optionale Hilfen wie Play Trim, Azimuth-Korrektur, Gap-Loss-Kompensation oder EQ-Konvertierung (IEC 120 µs ↔ 70 µs) gelten als **Hilfen im Referenzpfad**, nicht als implizite OCX-Kernlogik.

### Empfohlenes Cassette-primary-Referenzlayout

Klare Trennung zwischen:

- `refs/type2_cassette_real/` = echte, legal nutzbare Referenzen mit belegter Herkunft/Lizenz
- `refs/type2_cassette_synth/` = synthetische bzw. approximative, reproduzierbar erzeugte Referenzen

Pro Fall:

- `<name>_encoded.wav` (Pflicht)
- `<name>_source.wav` (Pflicht)
- `<name>_reference_decode.wav` (optional)
- `<name>.json` (Pflicht-Metadaten: Herkunft, Lizenz, Trust-Level, `source_type`)

Nur Fälle mit mindestens `*_encoded.wav` und `*_source.wav` werden als `cassette_reference` in die Bewertung aufgenommen.  
Diese Struktur verbessert die Praxisnähe, stellt für sich genommen jedoch noch keinen Nachweis historischer Standardgleichheit dar.

Standardmäßig enthält das Repository **keine echten lizenzierten dbx-Type-II-Referenzaufnahmen**.  
Die mitgelieferte Primärbasis ist daher zunächst synthetisch bzw. approximativ und entsprechend gekennzeichnet.

### Bekannte bereitgestellte Kandidaten

Folgende Kandidaten können separat importiert werden:

- `musik_enc.wav` (falls vorhanden): wird für den Offline-Pfad auf 44.1 kHz resampelt und als `encoded_candidate_only` geführt
- `musicfox_shopping_street.mp3` (falls decodierbar): wird ebenfalls auf 44.1 kHz gebracht, jedoch nur als Zusatz- bzw. Stressfall geführt

Der Import sucht diese Dateien zuerst im angegebenen Suchpfad und zusätzlich rekursiv darunter, falls sie nicht im Root liegen.

Beide Fälle gelten ohne dokumentierten Encoder- und Lizenzpfad **nicht** als harte Goldreferenz.

Referenzpaare `*_encoded.wav` und `*_source.wav` werden im Harness bei Bedarf ebenfalls auf die Zielrate des Profils (44.1 kHz) normalisiert, damit gemischte Quell-Sampleraten den Lauf nicht blockieren.  
Die Originaldateien bleiben unverändert.

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

### Firmware-Kommandos

- `p` = voller Status
- `m` = kompakter Telemetrie-Status  
  (inkl. `bypass=ON/OFF`, letztem `gDb/envDb`, Tonstatus und L/R-Modus)
- `n` = Signaldiagnose-Snapshot  
  (Input/Output Peak + RMS + Mean, Gain/Env, Decode-Aktivität, In/Out-Delta, L/R-Balance)
- `N` = Signaldiagnose-Counter gezielt zurücksetzen
- `v` = nur neue Clip-Ereignisse seit der letzten `v`-/`m`-/`p`-Abfrage ausgeben
- `X` = Clip-/Runtime-Counter, Signaldiagnose und Usage-Max zurücksetzen
- `k` = Testton-Kanalmodus zyklisch umschalten (`BOTH -> LEFT -> RIGHT`)
- `0` = Factory-Preset neu laden

### Bedeutung der Telemetrie

- `AudioProcessorUsage()` / `AudioProcessorUsageMax()`  
  → aktuelle und maximale DSP-CPU-Last
- `AudioMemoryUsage()` / `AudioMemoryUsageMax()`  
  → aktuell genutzte Audio-Blöcke bzw. Maximum
- `allocFailCount`  
  → Audio-Block-Allokation fehlgeschlagen (muss 0 bleiben)
- `inputClipCount`  
  → Frontend/Decoder-Eingang übersteuert
- `outputClipCount`  
  → Ausgangspfad übersteuert
- `inClipNew` / `outClipNew`  
  → neue Clipping-Ereignisse seit der letzten Statusabfrage
- `gain clamp hits` / `near-limit` im Snapshot  
  → wie oft `maxCutDb` bzw. `maxBoostDb` hart oder beinahe erreicht wurden

## Empfohlener Ablauf auf realer Hardware

1. Gerät booten und mit `0` das Factory-Preset laden.
2. Mit `X` Clip-/Runtime-Zähler und Maxima zurücksetzen.
3. Kalibrierung: 400-Hz-Testton bei `tone.level_dbfs = -9.8` einspeisen und auf den eigenen 0-VU-/Referenzpunkt des Deck-Workflows abgleichen.
4. Definierte Testquelle über mehrere Minuten abspielen.
5. Mit `m` zyklisch den kompakten Status lesen, mit `p` den Vollstatus prüfen.
6. Bewertung: CPU- und Memory-Reserve ausreichend, `allocFailCount == 0`, Clip-Zähler plausibel in Bezug auf Eingangspegel und Headroom.

Die kompakte `m`-Zeile zeigt dafür explizit `cpuRes=OK/TIGHT` und `memRes=OK/TIGHT` zusammen mit Clip-/Alloc-Zählern sowie gut sichtbar `bypass=ON/OFF`.

Zusätzlich enthält `m` jetzt `inClipNew/outClipNew`, sodass neue Übersteuerungen seit dem letzten Report sofort erkennbar sind.

Der Snapshot `n` ist als Live-Diagnosefenster gedacht:  
`N` drücken, Material abspielen, `n` lesen und anhand von In/Out-Delta, Gain-Min/Max und Decode-Aktivität bewerten.

Er enthält zusätzlich praxisnahe Cassette-Indikatoren mit sehr niedriger CPU-Last:

- RMS- und Peak-L/R-Balance in/out
- L/R-Differenzmittel und normierte L/R-Korrelation  
  (Hinweis auf Kanal- oder Phasenauffälligkeiten)
- einfacher Sidechain-HF/LF-Proxy (`high-vs-low`)  
  als grober Hinweis auf höhenarme bzw. höhenreiche Reize für den Detector
- Aktivitätsklassifikation (`LOW` / `MODERATE` / `HIGH`) plus `Cassette quick hints`  
  (als Diagnosehilfe, nicht als Normnachweis)
- Clamp-Auswertung (`cut/boost hits`, `near-limit`, kompakte Interpretation),  
  um z. B. Fälle wie `minGainDb = -24 dB` besser einzuordnen

## Arduino-`.ino`-Kompatibilität

`toneChannelModeLabel(...)` ist bewusst mit `uint8_t`-Signatur gehalten, damit der Arduino-IDE- bzw. `.ino`-Prototype-Preprocessor keinen Enum-Reihenfolgefehler auslöst.

## Testkassetten-Methodik

- **Kassette A (nicht compandiert, Grundreferenz):**  
  400 Hz (hier: -9.8 dBFS als setup-spezifischer 0-VU-Bezug), 1 kHz, 10 kHz, 3.15 kHz für Pegel, Kanalgleichheit, HF-/Azimuth- und Speed-Kontrolle
- **Kassette B (Type-II-encodiert, Decoder-Tuning):**  
  Mehrpegel-1-kHz, Bursts, Envelope-Steps, Pink/White Noise, Sweep, Bass+HF, Musik
- **Cassette-primary Harness (offline):**
  - Einzelton-/Mehrpegel-Matrix: **400 Hz, 1 kHz, 3.15 kHz, 10 kHz** über mehrere Pegel
  - Mehrfrequenz- und musiknahe Verbundfälle: Two-Tone, Bass+HF, Burst-/Transient-Train, Fast-Level-Switches, `music_like`
  - Breitbandfälle: Pink/White-Noise, Log-Sweep

Dabei gilt: Immer genau **einen** Type-II-Encoding-Pfad verwenden, keine Doppel-Encodierung.

## Einordnung der Aussagen

Dieses Projekt trifft **keine** Aussage über Bit-Exactness, Originalgleichheit oder Referenzgleichheit ohne belastbaren Messnachweis.

Auch der erweiterte cassette-primary Harness erhöht in erster Linie die praktische Validierungstiefe; er ersetzt keine vollständig dokumentierte dbx-Type-II-Normkonformitätsmessung.

## Kompatibilitätsstatus (dbx Type II Cassette)

- Ziel bleibt eine möglichst hohe praktische Decoder-Kompatibilität für reale Type-II-Kassetten im beschriebenen Hardware-Setup.
- Der aktuelle Stand ist ein methodisch abgestimmter, stabiler Decoderpfad mit Type-II-orientierter Ballistik und Sidechain-Formung.
- Eine historische Standardgenauigkeit eines dbx-Type-II-Decoders ist damit derzeit jedoch nicht belegt.
- Disc-spezifische Annahmen, beispielsweise implizite LF-Roll-off-Übernahmen, werden nicht stillschweigend als Cassette-Default übernommen.
- Offene Restabweichungen werden über Harness- und Hardware-Messungen dokumentiert und weitergeführt, nicht über Gleichheitsbehauptungen ersetzt.
