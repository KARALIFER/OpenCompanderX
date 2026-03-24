# OpenCompanderX / OCX Type 2

Dieses Repository enthält einen robusten **Type-II compander decoder** für eine feste Zielhardware:

- Teensy 4.1
- Teensy Audio Adaptor Rev D/D2 (SGTL5000)
- Stereo analog Line-In zu Stereo analog Line-Out/Headphone

## Status (ehrlich und reproduzierbar)

Aktuell im Repo verifizierbar:

- Firmware kompiliert mit PlatformIO für `teensy41`.
- Offline-Simulator läuft für WAV-Dateien.
- Offline-Harness erzeugt Metriken auch ohne Referenzdateien.
- Pytest-Regressionen prüfen Kernverhalten (Finite/Bounds/Stereo/CLI).

Nicht offline beweisbar (nur echte Hardware):

- Analoges Headroom-/Noise-Verhalten der SGTL5000-Stufe.
- Endgültige Abstimmung für konkrete Zuspieler (z. B. TEAC W-1200, FiiO CP13, We Are Rewind).
- Subjektive Klangbeurteilung realer Type-II-encodierter Inhalte.

## Wichtige Dateien

- `ocx_type2_teensy41_decoder.ino` – Firmware
- `ocx_type2_profile.json` – gemeinsames Profil für Firmware + Simulator
- `ocx_type2_wav_sim.py` – Offline-Simulator
- `ocx_type2_harness.py` – Regression/Harness
- `tests/test_ocx_type2.py` – Tests

## Profilprinzip

Es gibt **ein konservatives Universalprofil** für die feste Zielhardware.
Kein Mehrprofil-System, keine Auto-Erkennung.
`codec.line_in_level` ist wieder explizit gesetzt und mit der Firmware synchronisiert.

## Lokale Checks

```bash
python -m py_compile ocx_type2_wav_sim.py
python -m py_compile ocx_type2_harness.py
python -m py_compile tests/test_ocx_type2.py
pytest -q
pio run -e teensy41
```

## Hinweise zur Signalführung

Der Decoder arbeitet pro Kanal mit:

1. Input-Trim
2. DC-/Rumpel-Filter
3. Sidechain-Filterung
4. Hüllkurvendetektor (Attack/Release)
5. Expansions-Gain
6. De-Emphasis
7. Headroom + Soft-Clip als Schutz

Hinweis: Der Firmware-Bypass ist ein **geschützter Bypass** (inkl. Headroom/Soft-Clip), kein hartes transparentes Relay.

## Claims

Dieses Projekt macht **keine** Bit-Exact- oder Referenzgleichheits-Behauptung ohne Messbeleg.
