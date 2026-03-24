# OpenCompanderX – OCX Type 2 Validation Report

## 1) Ehrlichkeitsgrenze

Dieser Stand liefert **Build-/Offline-Messbarkeit**, aber keine Referenz- oder Bitexact-Behauptung.
Es lagen keine legalen proprietären Referenzausgänge im Repo vor.

## 2) Reparatur- und Stabilitätsstatus

Repariert/erweitert:

1. Profil/Firmware-Sync (inkl. `codec.line_in_level`).
2. Vollständiger Simulator mit numerischen Guards und CLI-Overrides.
3. Harness als Abstimmungs-Harness inkl. zusätzlicher Stressfälle und Score-Funktion.
4. Tests erweitert (inkl. 44.1-kHz-Pfad und Sync-Checks).
5. Firmware-Telemetrie für reale CPU/RAM/Allocate/Clip-Stabilitätsprüfung.

## 3) Firmware-Stabilität (offline + instrumentiert)

### Realtime-Sicherheit

- Kein Heap-/malloc-Muster im Audio-Sample-Loop.
- Keine `Serial`-Ausgabe in `update()`.
- Allocate-Fehler in `update()` werden gezählt/markiert (statt still zu passieren).

### Telemetrie (für echte Hardwareläufe)

Statusausgabe zeigt:

- Audio-CPU current/max (`AudioProcessorUsage`, `AudioProcessorUsageMax`)
- AudioMemory current/max (`AudioMemoryUsage`, `AudioMemoryUsageMax`)
- Allocate-Failure-Flag + Counter
- Input/Output-Clip-Flags + Counter

### RAM-Hinweis

`AudioMemory(64)` bleibt als konservativer Startwert; endgültige Reserve sollte auf echter Hardware über Max-Telemetrie verifiziert werden.

## 4) Offline-Abstimmungsmethodik

Ziel: stabiler Universal-Decoder, **nicht** Referenzklon.

Workflow:

A) Baseline messen
B) begrenzte Kandidaten-Sweeps
C) Score-Vergleich
D) plausibelsten Universal-Kandidaten auswählen

Score bestraft u. a.:

- häufiges Output-Clipping
- hohe Gain-Schwankung
- Kanalabweichung
- starke spektrale/Transienten-Abweichung
- Instabilität in Burst-/Envelope-/Rapid-Swing-Fällen

## 5) Durchgeführte Sweep-Ergebnisse (dieser Lauf)

Ausgeführt mit:

```bash
python ocx_type2_harness.py --out-dir artifacts/harness --analysis-fs 8000 --tune --tune-mode refine --max-candidates 6 --top-k 3
```

Ergebnis:

- Baseline-Score: `249.8292`
- Bester Kandidat (aus 6 Kandidaten): Score `230.5766`
- Dominanter Unterschied des besten Kandidaten: `reference_db = -16.0` (statt `-18.0`)

Zusätzlicher 44.1-kHz-Subsetvergleich (kritische Fälle) bestätigte Richtung:

- Baseline: `204.6092`
- `reference_db=-16.0`: `188.6646` (niedriger = besser im verwendeten Stabilitäts-Score)

## 6) Empfohlenes Universalprofil (offline-basiert)

Empfohlen für diesen Stand:

- `reference_db = -16.0`
- übrige Kernparameter konservativ belassen
- `line_in_level = 0`, `line_out_level = 29`

Begründung: im dokumentierten Offline-Scoring stabiler als Baseline, ohne aggressive Zusatzänderungen.

## 7) Offen / nur auf echter Hardware prüfbar

- finale Analog-Headroom-/Noise-Grenzen
- Quellgeräte-spezifisches Feintuning
- subjektive Hörqualität auf realem Material

## 8) Schlussbewertung

- Build stabil: **ja**
- Offline-Stabilität verbessert und messbar: **ja**
- Universalprofil plausibel (offline): **ja, mit dokumentierter Unsicherheit**
- Referenznähe: **unbelegt ohne legale Referenzmessung**
