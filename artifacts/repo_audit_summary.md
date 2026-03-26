# Repo Audit Summary

## Inventar (alle getrackten Dateien gelesen)

| Datei | Zweck | Klassifikation |
|---|---|---|
| `.gitignore` | Repository-Datei (siehe Inhalt). | docs |
| `FINAL_VALIDATION_ocx_type2_teensy.md` | Detaillierter Validierungsleitfaden für offline + Hardwarepfad. | docs |
| `LICENSE` | Repository-Datei (siehe Inhalt). | docs |
| `README.md` | Projektüberblick, Methodik, Bedienung und lokale Checks. | docs |
| `artifacts/encoder_feasibility_summary.md` | Repository-Datei (siehe Inhalt). | artifact, docs |
| `artifacts/forensic_audit_2026-03-26.md` | Repository-Datei (siehe Inhalt). | artifact, docs |
| `artifacts/harness_decode/metrics.csv` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_decode/metrics.json` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_decode/split_summary.json` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_decode/summary.json` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_detector/detector_study.json` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_encode/metrics.csv` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_encode/metrics.json` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_encode/split_summary.json` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_encode/summary.json` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_roundtrip/metrics.csv` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_roundtrip/metrics.json` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_roundtrip/split_summary.json` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_roundtrip/summary.json` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_tune/metrics.csv` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_tune/metrics.json` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_tune/split_summary.json` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_tune/summary.json` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `artifacts/harness_tune/tuning_best.json` | Repository-Datei (siehe Inhalt). | artifact, audio-relevant |
| `ocx_type2_harness.py` | Harness, Metriken, Tuning, Detector-Study, Referenz-Tools. | audio-relevant |
| `ocx_type2_profile.json` | Gemeinsames Decoder/Encoder-Profil. | audio-relevant |
| `ocx_type2_teensy41_decoder.ino` | Haupt-Firmware (DSP, Telemetrie, Persistenz). | build-relevant |
| `ocx_type2_wav_sim.py` | Offline-Simulator decode/encode/roundtrip. | audio-relevant |
| `platformio.ini` | PlatformIO-Board/Framework-Konfiguration für Teensy 4.1. | build-relevant |
| `refs/README.md` | Layout und Regeln für Referenzmaterial. | docs |
| `refs/type2_cassette_real/manifest.example.json` | Beispiel-Manifest für legale Referenz-Downloads. | docs |
| `refs/type2_cassette_synth/.gitkeep` | Repository-Datei (siehe Inhalt). | docs |
| `src/main.cpp` | PlatformIO-Entry (inkludiert die .ino-Firmware). | build-relevant |
| `tests/test_ocx_type2.py` | Pytest-Regressionen (Profile-Sync, Metriken, Tuning, Detector). | test-relevant |

## Wichtige Prüfnotizen
- `arduino-cli compile` auf der losen `.ino`-Datei scheitert erwartbar wegen Sketch-Namensregel (Ordnername == .ino-Name).
- Kompilierung über temporären Sketch-Ordner war erfolgreich.
- Physische Teensy/SGTL5000-Analogvalidierung wurde im Container nicht ausgeführt (keine reale Hardware angebunden).