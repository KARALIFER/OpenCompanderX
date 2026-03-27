# Forensischer Audit: OpenCompanderX (Stand 2026-03-26)

## A) Repo-Inventar (Quelle: `git ls-files`)

Vollständige getrackte Dateien:

1. .gitignore — Repo-Ausschlüsse, nicht build-relevant.
2. FINAL_VALIDATION_ocx_type2_teensy.md — Validierungs-/Hardware-Workflow-Doku, indirekt build-relevant (Prozessvorgaben).
3. LICENSE — Lizenztext, nicht build-relevant.
4. README.md — Hauptdokumentation inkl. Build-Kommandos, build-relevant als Prozessquelle.
5. artifacts/encoder_feasibility_summary.md — Ergebnisdokument, nicht build-relevant.
6. artifacts/harness_decode/metrics.csv — generiertes Messartefakt, nicht build-relevant.
7. artifacts/harness_decode/metrics.json — generiertes Messartefakt, nicht build-relevant.
8. artifacts/harness_decode/split_summary.json — generiertes Messartefakt, nicht build-relevant.
9. artifacts/harness_decode/summary.json — generiertes Messartefakt, nicht build-relevant.
10. artifacts/harness_detector/detector_study.json — generiertes Messartefakt, nicht build-relevant.
11. artifacts/harness_encode/metrics.csv — generiertes Messartefakt, nicht build-relevant.
12. artifacts/harness_encode/metrics.json — generiertes Messartefakt, nicht build-relevant.
13. artifacts/harness_encode/split_summary.json — generiertes Messartefakt, nicht build-relevant.
14. artifacts/harness_encode/summary.json — generiertes Messartefakt, nicht build-relevant.
15. artifacts/harness_roundtrip/metrics.csv — generiertes Messartefakt, nicht build-relevant.
16. artifacts/harness_roundtrip/metrics.json — generiertes Messartefakt, nicht build-relevant.
17. artifacts/harness_roundtrip/split_summary.json — generiertes Messartefakt, nicht build-relevant.
18. artifacts/harness_roundtrip/summary.json — generiertes Messartefakt, nicht build-relevant.
19. artifacts/harness_tune/metrics.csv — generiertes Messartefakt, nicht build-relevant.
20. artifacts/harness_tune/metrics.json — generiertes Messartefakt, nicht build-relevant.
21. artifacts/harness_tune/split_summary.json — generiertes Messartefakt, nicht build-relevant.
22. artifacts/harness_tune/summary.json — generiertes Messartefakt, nicht build-relevant.
23. artifacts/harness_tune/tuning_best.json — generiertes Messartefakt, nicht build-relevant.
24. ocx_type2_harness.py — Python-Harness, run-relevant.
25. ocx_type2_profile.json — gemeinsame Parameterquelle, build/run-relevant.
26. OpenCompanderX.ino — Firmwarekern, build-relevant.
27. ocx_type2_wav_sim.py — Python-Simulator, run-relevant.
28. platformio.ini — PlatformIO-Builddefinition, build-relevant.
29. refs/README.md — Referenzmaterial-Doku, run-relevant für Harness-Referenzpfad.
30. refs/type2_cassette_real/manifest.example.json — Beispielmanifest, optional run-relevant.
31. refs/type2_cassette_synth/.gitkeep — Platzhalter, nicht build-relevant.
32. src/main.cpp — PlatformIO-Entrypoint (inkludiert .ino), build-relevant.
33. tests/test_ocx_type2.py — Pytest-Suite, run-relevant.

Inventar-Hinweis: Es sind **tatsächlich** `platformio.ini`, `tests/` und JSON-Profile vorhanden. Diese wurden nicht erfunden.

## B) Repo vs. lokaler Stand (forensisch)

Vergleich durchgeführt per Verzeichnis-Diff zwischen:
- `/workspace/OpenCompanderX_upstream` (frisch geklont von GitHub)
- `/workspace/OpenCompanderX` (lokaler Arbeitsstand)

Ergebnis vor Patch: **kein Inhaltunterschied** (0 Diff-Zeilen).

Konsequenz:
- Kein separater „größerer lokaler Sketch“ nachweisbar.
- Der lokale Sketch `OpenCompanderX.ino` entsprach vor der Korrektur exakt dem Public-Repo-Stand.

## C) Arduino/Teensy Buildprüfung

Dokumentierte Build-Wege in README enthalten u. a. `pio run -e teensy41`.
Realer Build wurde auf exakt diesem Stand ausgeführt und war erfolgreich.

## D) Pflichtprüfung `PersistSettings does not name a type`

Befund:
- Im Sketch existieren Funktionen mit Signaturen auf `PersistSettings` (`settingsChecksum`, `persistSettings`, `loadSettingsOrFactory`, `factoryResetSettings`) und der Typ wird später im File definiert.
- Arduino erzeugt implizite Funktionsprototypen; diese können vor der Struct-Definition landen.

Forensischer Nachweis per Minimalreproduktion:
- Ohne Forward-Declaration produziert C++ exakt den Fehler:
  `error: ‘PersistSettings’ does not name a type`
- Mit `struct PersistSettings;` vor dem Prototyp kompiliert derselbe Aufbau.

Durchgeführte Korrektur im Sketch:
- Forward-Declaration eingefügt: `struct PersistSettings;` direkt nach den Includes.

Warum nötig:
- Stabilisiert die Übersetzungsreihenfolge gegen Arduino-Autoprototyping und verhindert typabhängige Prototypfehler.

## E) Python-Simulatorprüfung

Geprüft gegen vorhandene Dateien:
- `ocx_type2_wav_sim.py`
- `ocx_type2_harness.py`
- `tests/test_ocx_type2.py`
- `ocx_type2_profile.json`

Installiert (real ausgeführt):
- `python -m pip install --upgrade pip`
- `python -m pip install numpy scipy matplotlib`

Audio-Run-Status:
- Im getrackten Repo sind keine `.wav`-Dateien vorhanden.
- Aussage daher zwingend: **„Simulator-Code geprüft, aber kein echter Audio-Run mangels Input-WAV.“**

## F) Ergebnisstatus (wahrheitsgemäß)

- Repo gelesen: JA
- alle getrackten Dateien geprüft: JA
- echter Python-Run (CLI/Test): JA
- echter Python-Audio-Run mit Input-WAV: NEIN (kein Input-WAV im Repo)
- echter Teensy-Build: JA (PlatformIO-Build erfolgreich)
- nur statische Prüfung: NEIN

## Konkreter Minimal-Patch

```diff
--- a/OpenCompanderX.ino
+++ b/OpenCompanderX.ino
@@
 #include <SPI.h>
 #include <EEPROM.h>
 #include <math.h>
+
+// Forward declaration to keep Arduino auto-prototype generation safe for
+// functions that take PersistSettings before the struct definition appears.
+struct PersistSettings;
```
