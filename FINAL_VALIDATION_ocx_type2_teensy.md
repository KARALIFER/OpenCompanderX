# OpenCompanderX – OCX Type 2 Validierungsstand (Phase 2)

## Scope

Dieser Stand dokumentiert nur das, was im Repository praktisch geprüft werden konnte.
Die Methodik trennt strikt:

1. **Referenzlose Stabilitäts-/Plausibilitätsbewertung** (offline)
2. **Black-Box-Referenzvergleich** (offline, nur mit Referenzdateien)
3. **Analoge Laufzeitvalidierung auf echter Teensy-/SGTL5000-Hardware**

## Phase-2-Ergebnisüberblick

- Score-Logik bewertet ohne Referenz nicht mehr primär „Input möglichst ähnlich“.
- Tuning läuft zweistufig: **coarse low-rate** + **final 44.1-kHz Re-Ranking**.
- Referenzpfad ist im Harness sauber getrennt modelliert.
- Firmware-Telemetrie ist kompakt (`m`) und als Hardware-Testablauf interpretierbar dokumentiert.
- Simulator bietet zusätzlich einen RMS-näheren Detectorpfad für methodische A/B-Vergleiche gegen den bisherigen energy-nahen Pfad.
- Universalprofil bleibt konservativ (`ocx_type2_universal_v2`), ohne Über-Claims.

## Methodikdetails

### Cassette-primary Pfad (neu, offline)

Der Harness behandelt dbx-Type-II-Cassette jetzt explizit als Primärpfad:

1. **Einzelton-/Mehrpegel-Matrix**
   - Pflichtfrequenzen: 400 Hz, 1 kHz, 3.15 kHz, 10 kHz
   - Mehrere Pegel pro Frequenz für Gain-/Level-Tracking
2. **Mehrfrequenz-/musiknahe Fälle**
   - Two-/Multi-Tone, Bass+HF gleichzeitig
   - Burst-/Transient-Train, Fast-Level-Switches, `music_like`
3. **Breitbandfälle**
   - Pink/White Noise, Log Sweep
4. **Cassette-Referenzfälle (wenn vorhanden)**
   - `*_encoded.wav` + `*_source.wav`, optional `*_reference_decode.wav`

Dieser Pfad verbessert die Praxisnähe deutlich, ist aber weiterhin keine automatische „100 % dbx-original“-Aussage.


## Referenzbeschaffung / Generierung (neu)

- Harness unterstützt jetzt eine zweistufige Referenzpipeline:
  - `--index-real-refs` und optional `--fetch-real-refs-manifest` für legal belegte reale Daten in `refs/type2_cassette_real/`.
  - `--prepare-known-music-candidates` für bekannte bereitgestellte Kandidaten (`musik_enc.wav`, `musicfox_shopping_street.mp3`) als `encoded_candidate_only`-Fälle.
  - `--generate-synth-refs` für reproduzierbare synthetische Fälle in `refs/type2_cassette_synth/`.
- Reale und synthetische Fälle werden im Lauf getrennt geführt (`reference_source_type`) und getrennt zusammengefasst (`split_summary.json`).
- Reale Referenzen werden im Score höher gewichtet als synthetische; Trust-Level wird zusätzlich berücksichtigt.
- Wichtige Grenze bleibt explizit: der interne Approximations-Encoder ist ein Testwerkzeug und **kein** Beweis historischer dbx-Type-II-Normgleichheit.

Hinweis zu den zwei bekannten Kandidaten:

- `musik_enc.wav` wird bei Import auf 44.1 kHz normalisiert und als praxisnaher encoded Kandidat geführt, solange Herkunft/Encoderpfad nicht belegt ist.
- `musicfox_shopping_street.mp3` bleibt Zusatz-/Stressmaterial (verlustbehaftet, nicht Primärreferenz).
- Der Kandidatenimport sucht Dateien auch rekursiv im gewählten Suchpfad, falls sie nicht direkt im Repo-Root liegen.
- Referenzfälle mit abweichender Samplerate werden für den Offline-Lauf auf die Profilrate (typisch 44.1 kHz) resampelt; Originaldateien werden dabei nicht überschrieben.

## A) Referenzlose Bewertung (Plausibilität/Stabilität)

Bewertet werden u. a.:

- Output-Clipping
- Kanalabweichung
- Gain-Kurven-Streuung und Gain-Sprünge (Ballistik-/Pump-Indikatoren)
- starke spektrale Verfärbung
- starke Transientenzerstörung
- übermäßige Soft-Clip-Abhängigkeit
- unplausible Reaktion über unterschiedliche Eingangspegel
- Gain-vs-Input-Tracking (Slope/R²) bei ausreichender Pegelspanne
- bandbegrenzte Spektraldeltas (Low/Mid/High) statt nur globalem Spektralwert
- Overshoot-/Undershoot-Indikatoren für Burst-/Transient-Fälle

Nicht als Primärziel: maximale Input-Ähnlichkeit.
Zusätzlich wird eine zu schwache Dekodierwirkung (Under-Decoding) explizit bestraft, damit „Input fast unverändert“ nicht trivial gewinnt.

## B) Referenzvergleich (optional)

Wenn Referenzdateien vorhanden sind:

- Längenabgleich auf gemeinsame Mindestlänge
- getrennte Referenzmetriken (`*_vs_reference`)
- separater Referenzscore zusätzlich zum Plausibilitätsscore
- optional zusätzlicher Vergleich gegen `source` (Originalmaterial), getrennt von `reference_decode`

Ohne Referenzdateien wird keine Referenznähe behauptet.

Empfohlenes Layout:

- `refs/type2_cassette/<name>_encoded.wav`
- `refs/type2_cassette/<name>_source.wav`
- optional `refs/type2_cassette/<name>_reference_decode.wav`

## C) Zweistufige Abstimmung

1. **Grobselektion** bei kleiner Rate (Default `tune_fs=4000`) zur Laufzeitreduktion
2. **Finales Re-Ranking** bei `tune_final_fs=44100` für finale Entscheidung

Eine finale Profilentscheidung darf nicht nur aus 4-kHz-Sweeps abgeleitet werden.

## Referenzpfad vs. Zielpfad

### Digitaler Referenzpfad

- Black-Box-Vergleichspfad (z. B. USB/PC/Referenzdecoder)
- Fokus: Decodercharakteristik, Ballistik, Type-II-Tendenz
- Keine Bewertung von SGTL5000-Analoggrenzen

### Analoger Zielpfad

- Analoge Quelle -> Teensy Line-In -> OCX -> analog out
- Fokus: Headroom, Trim, Clipping, Rauschen, Runtime-Stabilität

Keine doppelte Type-II-Encode-Kette im Vergleichspfad aufbauen.

## Referenzmaterial (A/B)

- **Referenzkassette A (uncompanded):** Pegel-/Signalwegkontrolle
- **Referenzkassette B (Type-II-encodiert):** Decoderabstimmung/Black-Box-Abgleich

400-Hz-Ton ist nur Kalibrierhilfe, nicht alleiniger Decoderabgleich.
Für das konkrete Nutzer-Setup (Mixtape Nerd + TEAC W-1200 + RTM-Band) wird
`0 VU` praktisch bei ca. `-9.8 dBFS` getroffen; daher wird der Kalibrierton
als **400 Hz @ -9.8 dBFS** geführt (setup-spezifisch, kein Universalstandard).
Ergänzend nutzen: Mehrpegel-Sinus, Bursts, Envelope-Steps, Noise, Sweep, Musik.
Optionale Hilfen (Play Trim, Azimuth Correction, Gap-Loss Compensation, EQ Converter) sind Referenzpfad-Werkzeuge, nicht stillschweigende Kernfunktion des OCX-Decoders.

## Pegeltrennung im Profil

- `tone.level_dbfs` = Kalibrier-Testtonpegel für den realen Deck-/Band-Abgleich
  (hier: `-9.8 dBFS` im genannten Setup).
- `decoder.reference_db` = interner Decoder-Referenzparameter (derzeit `-18.0`),
  separat zu validieren und **nicht** automatisch aus dem Kalibrierpegel abzuleiten.

## Hardware-Testablauf (telemetriegeführt)

1. Gerät booten
2. Factory-Preset laden (`0`)
3. Telemetrie resetten (`X`)
4. Kalibrieren mit 400 Hz @ `-9.8 dBFS` gegen den eigenen 0-VU-/Ref.-Punkt
   des Ziel-Deck-Workflows.
5. Definierte Testquelle mehrere Minuten abspielen
6. Status (`p`) und/oder kompakte Telemetrie (`m`) auslesen
7. Interpretieren:
   - CPU-Reserve ausreichend? (`AudioProcessorUsageMax` nicht nahe Dauerüberlast)
   - AudioMemory-Reserve ausreichend? (`AudioMemoryUsageMax < AudioMemory(64)`)
   - `allocFailCount == 0`?
   - `inputClipCount`/`outputClipCount` unauffällig?

`m` liefert dafür eine kompakte Einzeile (`[TLM] ...`) ohne seriellen Spam im Audiopfad.
Die Zeile enthält explizit `cpuRes=OK/TIGHT` und `memRes=OK/TIGHT` für schnelle Laufzeit-Interpretation.

## Profilentscheidungslogik

Aktuelles Universalprofil wurde als robuster Kompromiss beibehalten:

- konservative Analog-Reserve (`line_in_level=0`, `headroom_db=1.0`)
- moderate Decoderstärke (`strength=0.76`)
- stabile Ballistik (`attack_ms=3.5`, `release_ms=140.0`)

Nicht gewählte Kandidaten (typische Gründe):

- höhere `strength` + weniger `headroom`: mehr Clipping-/Soft-Clip-Druck
- aggressiveres Release: stärkere Gain-Sprünge/Pump-Anzeichen
- stark reduzierte De-Emphasis: unplausible HF-Verfärbung

Finale Auswahl bleibt an 44.1-kHz-Ergebnissen gebunden; Low-Rate-Sweeps dienen nur als Vorscan.

## Grenzen / offene Unsicherheiten

- Keine Bitexact-/Originalgleich-/Referenzgleich-Claims ohne harte Messkette.
- Aktueller Stand ist als robuste, Type-II-orientierte Decoder-Annäherung bewertet, aber nicht als vollständig standardgenau historisch verifiziert.
- Cassette-vs-Disc-Trennung bleibt bewusst konservativ: keine unbelegte Übernahme disc-spezifischer LF-Kompensationen als Default.
- Endgültige Bewertung bleibt hardware- und materialabhängig.
- Black-Box-Demos ohne exakt dokumentierten Prozesspfad sind nur Hinweis, keine harte Kalibrierreferenz.
- RMS-nahe Detector-Variante ist aktuell primär als Simulator-/Methodikwerkzeug bewertet; Firmwareseitige Aktivierung bleibt offen bis echte CPU-/Telemetry-Reserve auf Hardware geprüft ist.
