# OpenCompanderX / OCX Type 2

Dieses Repository enthält einen robusten **Type-II-compander-decoder** für eine feste Zielhardware:

- Teensy 4.1
- PJRC Teensy Audio Adaptor Rev D/D2 (SGTL5000)
- analog stereo line-in
- analog stereo line-out / optional headphone

## Scope-Grenzen (bewusst eng)

- Kein USB-Audio
- Kein Encoder/Recorder/Display
- Ein Universalprofil
- Keine Autoerkennung / keine Mehrprofile

## Was in diesem Repo valide ist

- Firmware-Build mit PlatformIO (`teensy41`) ist reproduzierbar.
- Offline-Simulator und Harness laufen reproduzierbar.
- Parameter-Sweeps (offline) sind dokumentiert.
- Pytest deckt Produktionspfad inkl. 44.1 kHz mit ab.

## Was **nicht** offline bewiesen werden kann

- finale Klangbeurteilung auf realem Type-II-Programmmaterial
- analoge SGTL5000-Headroom-/Noise-Details unter echter Verkabelung
- Referenzgleichheit zu proprietären Decodern (keine legalen Referenzausgänge im Repo)

## Universalprofil (aktuell empfohlen)

Datei: `ocx_type2_profile.json`

Wichtige Decoder-Defaults:

- `input_trim_db: -3.0`
- `output_trim_db: -1.0`
- `strength: 0.76`
- `reference_db: -16.0`
- `attack_ms: 3.5`
- `release_ms: 140.0`
- `sidechain_hp_hz: 90.0`
- `sidechain_shelf_hz: 2800.0`
- `sidechain_shelf_db: 16.0`
- `deemph_hz: 1850.0`
- `deemph_db: -6.0`
- `soft_clip_drive: 1.08`
- `dc_block_hz: 12.0`
- `headroom_db: 1.0`

Codec-Teil:

- `line_in_level: 0` (konservativer universeller Default)
- `line_out_level: 29`

## CPU/RAM stability notes

- `AudioMemory(64)` bleibt gesetzt; das ist für diese Objekt-/Patchcord-Struktur ein konservativer Startpunkt.
- Firmware-Telemetrie enthält jetzt:
  - `AudioProcessorUsage()` / `AudioProcessorUsageMax()`
  - `AudioMemoryUsage()` / `AudioMemoryUsageMax()`
  - Allocate-Fehler-Flag + Counter in `update()`
  - Input/Output-Clip-Flags + Counter
- Telemetrie wird nur im Statuspfad ausgegeben (nicht im Audio-Callback), damit Echtzeitpfad sauber bleibt.

## Reproduzierbare Offline-Abstimmung (kein Referenz-Claim)

Baseline und Kandidaten werden über `ocx_type2_harness.py` mit gewichteter Stabilitäts-Score-Funktion verglichen.

Beispiel:

```bash
python ocx_type2_harness.py --out-dir artifacts/harness --analysis-fs 8000 --tune --tune-mode refine --max-candidates 6 --top-k 3
```

Hinweis: Für schnelle Sweeps kann `--analysis-fs` reduziert werden; finale Plausibilisierung sollte zusätzlich bei 44.1 kHz erfolgen.

## Lokale Checks

```bash
rg '^(<<<<<<<|=======|>>>>>>>)' .
git diff --check
python -m py_compile ocx_type2_wav_sim.py
python -m py_compile ocx_type2_harness.py
python -m py_compile tests/test_ocx_type2.py
pytest -q
pio run -e teensy41
```

## Firmware-Hinweis

Der Bypass ist ein **geschützter Bypass** (inkl. Headroom + Soft-Clip) und kein hartes transparentes Relay.
