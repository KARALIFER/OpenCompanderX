# OpenCompanderX – OCX Type 2 Validierungsstand

## Scope

Dieser Stand dokumentiert nur das, was im Repository praktisch geprüft werden konnte.
Er trennt klar zwischen:

- Build-/Code-Validierung im Repo
- Offline-Simulation/Regression
- Hardware-only Themen

## Reparierte Kernprobleme

1. Profil/Firmware-Sync war defekt (`codec.line_in_level` fehlte).
2. Simulatordatei war trunkiert und nicht lauffähig.
3. Harness war trunkiert und Referenzvergleich unrobust.
4. Tests waren unvollständig/inkonsistent.
5. `.gitignore` deckte Harness-Artefakte nicht vollständig ab.
6. Dokumentation war weiter als der tatsächliche Codezustand.
7. Firmware-Telemetrie war für CPU/RAM/Allocate-Stabilität nicht explizit auswertbar.
8. Harness deckte mehrere praxisnahe Stressfälle und Bewertungslogik nicht systematisch ab.

## Tatsächlich ausgeführte Checks

- Konfliktmarker-Suche
- `git diff --check`
- Python-Compile-Checks für Simulator/Harness/Tests
- `pytest -q`
- `pio run -e teensy41`
- `python ocx_type2_harness.py --out-dir artifacts/harness`

## Ergebnis

- Firmware ist buildbar.
- Profil/Firmware/Simulator sind synchron.
- Simulator und Harness sind vollständig ausführbar; Simulator akzeptiert Decoder-Overrides.
- Harness enthält zusätzliche Stressfälle (u. a. HF-Burst-Züge, Bass+HF, schnelle Pegelwechsel, Transientenzug) und berechnet einen Straf-Score.
- Tests prüfen zentrale Robustheitsanforderungen inkl. 44.1-kHz-Fall, Mono/Stereo-Shape-Handling, Referenz-Längenangleichung und Profil/Firmware-Sync.
- Firmware bietet auslesbare Telemetrie: CPU now/max, AudioMemory now/max, Clip-Zähler, Allocate-Fail-Zähler.

## Aktuell empfohlenes Universalprofil

- Profilname: `ocx_type2_universal_v2`
- `codec.line_in_level = 0` bleibt konservativer Default (mehr Analog-Headroom gegen heiße Portable-/Consumer-Line-Outs).
- Profil ist als robuster Universal-Kompromiss zu verstehen, nicht als „optimal“.

## Parameterabstimmung (offline)

- Das Harness enthält eine Score-Funktion, die Kandidaten bei Clipping, Kanalabweichung, Spektralabweichung, Transientenfehlern und instabiler Gain-Reaktion bestraft.
- Damit ist reproduzierbare A/B-Bewertung möglich; der Score ersetzt keine reale Hör- und Hardwaremessung.
- Ein kompakter Tuning-Modus ist vorhanden, aber rechenintensiv; für reale Nutzung auf kleine Suchräume/Subset-Cases begrenzen.

## Weiterhin nur mit echter Hardware verifizierbar

- Endgültiges analoges Pegelverhalten und Noise/Hum.
- Feintuning des Universalprofils gegen reale Zuspieler.
- Subjektive Qualitätsbewertung mit realem Material.

## Ehrlichkeitsregel

Keine Bit-Exact-Claims. Keine Referenzgleichheits-Claims ohne reale Referenzmessung.
