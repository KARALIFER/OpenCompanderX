# Phase-3 Open Gates Report (Merged Baseline Continuation)

## 1) Gemergter Baseline-Stand
- Baseline unverändert bestätigt: STRICT_COMPATIBLE/RESTORATION/CONTROLLED_RECORD, stereo-linked True-RMS detector, Type-II sidechain HP/LP/HF shelf, Auto-Trim, Dropout-Hold, Saturation-Soft-Fail, decoderOperatingMode persistence, PLAYBACK_GUARD_DYNAMIC.

## 2) Welche Dateien erneut geprüft wurden
- OpenCompanderX.ino
- ocx_type2_profile.json
- ocx_type2_wav_sim.py
- ocx_type2_harness.py
- tests/test_ocx_type2.py
- artifacts/harness_decode*/metrics.json, summary.json
- artifacts/harness_roundtrip/summary.json

## 3) Welche offenen Restprobleme identifiziert wurden
- Hardware-Gate weiterhin offen (keine neue Teensy4.1+SGTL5000 Messung in diesem Lauf).
- Clip-Gate offen: 35 output clips wurden noch nicht mit echtem Log reproduzierbar auf Ereigniszeitpunkt zurückgeführt.
- Qualitäts-Gate teilweise offen: RESTORATION zeigt nur kleine numerische Deltas bei Pflichtfällen.
- Guard-Gate offen: 30%-Reduktion weiterhin nicht belastbar bewiesen.
- Tracking/Burst-Abnahme als dedizierter Hardware/Hybrid-Report weiterhin offen.

## 4) Was auf realer Hardware geprüft wurde
- In diesem Durchlauf keine neue reale Hardwaremessung; nur Offline-/Harness-Validierung durchgeführt.

## 5) Welche Output-Clip-Ursache gefunden wurde
- Befund: Kein neuer Hardware-Log mit [TLM]/[DIAG] bereitgestellt, daher die 35 Clips nicht final kausal zuordenbar.
- Neu vorbereitet: telemetrie-parser `ocx_telem_analyzer.py` zur Ereignisrekonstruktion (clipOutTotal-Sprünge, guardState, near/clamp-Kontext).

## 6) Welche Änderungen noch zusätzlich im Firmwarepfad gemacht wurden
- Kein Eingriff in DSP-/Firmware-Audiopfad; Baseline-Firmware bewusst stabil gehalten.

## 7) Welche Änderungen STRICT_COMPATIBLE betreffen
- Keine Parameter-/Verhaltensänderung in STRICT_COMPATIBLE vorgenommen.

## 8) Welche Änderungen RESTORATION betreffen
- Keine RESTORATION-Parameteränderung; nur erneute Auswertung der Pflichtfälle.

## 9) Was an Guard/Clip/Headroom neu bewertet wurde
- Guard/Clip/Headroom wurden offline erneut bewertet; decode/roundtrip summaries weiterhin ohne verschärfte Clamp-Indikatoren in den vorhandenen Artefakten.
- Für reale Kausalität wurde ein strukturierter Log-Analyzer ergänzt (noch ohne echtes neues Logmaterial ausgewertet).

## 10) Welche Builds wirklich gelaufen sind
- `pio run -e teensy41`: erfolgreich.
- `arduino-cli compile --fqbn teensy:avr:teensy41 OpenCompanderX.ino`: erfolgreich.

## 11) Welche Tests wirklich grün sind
- `pytest -q tests/test_ocx_type2.py`: 42 passed.
- Harness decode/roundtrip neu gelaufen.
- decode-universal/restoration Artefakte als Baseline vorhanden; Neu-Lauf universal hing in der Umgebung ohne Ausgabe (abgebrochen).

## 12) Welche Hardwareläufe dokumentiert wurden
- Keine neuen Hardwareläufe in diesem Durchlauf dokumentiert (DIAG/TLM/SIGNAL SNAPSHOT fehlen).

## 13) Welche Metriken sich konkret verbessert haben
- Pflichtfall-Vergleich universal vs restoration (delta restoration-universal):
  - bursts: gain_diff_p95 +0.000005, transient +0.000000, overshoot -0.000002, undershoot -0.000000
  - transient_train: gain_diff_p95 +0.000004, transient -0.000001, overshoot -0.000001, undershoot -0.000001
  - bass_plus_hf: gain_diff_p95 +0.000000, transient +0.000000, overshoot +0.000000, undershoot -0.000000
  - music_like: gain_diff_p95 -0.000001, transient +0.000000, overshoot -0.000006, undershoot -0.000001
  - fast_level_switches: gain_diff_p95 -0.000169, transient -0.000025, overshoot -0.000156, undershoot -0.000142

## 14) Ob die 30%-Guard/Clamp-Schwelle jetzt belastbar erfüllt ist
- Nein. 30%-Guard/Clamp-Schwelle weiterhin NICHT belastbar erfüllt (mangels reproduzierter Hardware-Zeitreihen + statistischem Vorher/Nachher-Nachweis).

## 15) Welche Gates jetzt grün sind
- Build-Gate: GRÜN (offline toolchain).
- Python-Test-Gate: GRÜN (42 passed).
- Baseline-Stabilitätsgate: GRÜN (keine Regression-Änderung in Firmwarepfad).

## 16) Welche Gates noch rot sind
- Hardware-Gate: ROT.
- Clip-Gate (35 Clips Kausalität): ROT.
- Qualitäts-Gate (über kleine numerische Deltas hinaus): ROT.
- Guard-Gate (30%-Nachweis): ROT.
- Tracking/Burst-Hardwareabnahme: ROT.

## 17) Abschlussstatus
- **NICHT FERTIG** – Merge-Stand blieb stabil und wurde mit Zusatz-Analysewerkzeug ergänzt, aber Hardware-/Clip-/Qualitätsnachweis ist weiterhin unvollständig.
