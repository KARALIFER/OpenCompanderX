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

## Tatsächlich ausgeführte Checks

- Konfliktmarker-Suche
- `git diff --check`
- Python-Compile-Checks für Simulator/Harness/Tests
- `pytest -q`
- `pio run -e teensy41`

## Ergebnis

- Firmware ist buildbar.
- Profil/Firmware/Simulator sind synchron.
- Simulator und Harness sind vollständig ausführbar.
- Tests prüfen zentrale Robustheitsanforderungen.

## Weiterhin nur mit echter Hardware verifizierbar

- Endgültiges analoges Pegelverhalten und Noise/Hum.
- Feintuning des Universalprofils gegen reale Zuspieler.
- Subjektive Qualitätsbewertung mit realem Material.

## Ehrlichkeitsregel

Keine Bit-Exact-Claims. Keine Referenzgleichheits-Claims ohne reale Referenzmessung.
