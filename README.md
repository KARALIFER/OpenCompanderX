# OpenCompanderX / OCX Type 2

Dieses Repository enthält einen robusten **Type-II compander decoder** für eine feste Zielhardware:

- Teensy 4.1
- Teensy Audio Adaptor Rev D/D2 (SGTL5000)
- Stereo analog Line-In zu Stereo analog Line-Out/Headphone

## Status (ehrlich und reproduzierbar)

Aktuell im Repo verifizierbar:

- Firmware kompiliert mit PlatformIO für `teensy41`.
- Offline-Simulator läuft für WAV-Dateien.
- Offline-Harness erzeugt robuste Metriken auch ohne Referenzdateien und berechnet einen Straf-Score für instabile Kandidaten.
- Pytest-Regressionen prüfen Kernverhalten (Finite/Bounds/Stereo/CLI/Profil-Firmware-Sync).

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

Empfohlenes Universalprofil (aktueller Stand): `ocx_type2_universal_v2` mit:
- `line_in_level=0` (konservativer Analog-Headroom für reale Consumer-/Portable-Quellen),
- moderater Expansion (`strength=0.76`),
- defensivem Schutzpfad (`headroom_db=1.0`, `soft_clip_drive=1.08`).

## Lokale Checks

```bash
python -m py_compile ocx_type2_wav_sim.py
python -m py_compile ocx_type2_harness.py
python -m py_compile tests/test_ocx_type2.py
pytest -q
pio run -e teensy41
# optional: Offline-Messlauf
python ocx_type2_harness.py --out-dir artifacts/harness
```

Optionales Tuning-Interface:

```bash
# einzelne Parameter überschreiben (Simulator)
python ocx_type2_wav_sim.py in.wav out.wav --override strength=0.80 --override deemph_db=-5.0

# Harness mit Decoder-Overrides
python ocx_type2_harness.py --override strength=0.80 --override release_ms=160
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
Zusätzlich stellt die Firmware Laufzeit-Telemetrie bereit (`AudioProcessorUsage`, `AudioMemoryUsage`, Clip-/Allocate-Zähler), ohne `update()` mit Serial-I/O zu belasten.
`AudioMemory(64)` bleibt als plausibler Startwert gesetzt; echte Reservebewertung benötigt Hardwarelasttests.

## Claims

Dieses Projekt macht **keine** Bit-Exact- oder Referenzgleichheits-Behauptung ohne Messbeleg.
