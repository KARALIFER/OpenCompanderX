# AUTO_CAL measurement tape (1 kHz)

## 1) Zweck
`AUTO_CAL` ist ein expliziter Kalibriermodus für eine **separate Messkassette**. Er ist nicht für laufende Musik-Nachregelung gedacht.

## 2) Nicht pro Musikkassette nötig
Die Messkassette wird nur verwendet, wenn neu kalibriert werden soll (z. B. anderes Deck/Servicezustand). Für normale Musikbänder bleibt die gelockte Kalibrierung aktiv.

## 3) Empfohlene Struktur (verbindlich)
- 5 s silence
- 30 s 1 kHz
- 3 s silence
- 30 s 1 kHz
- 3 s silence
- 30 s 1 kHz
- 5 s silence

## 4) Aufnahmeparameter
- kein Fade
- kein EQ
- keine Pegeländerung zwischen Blöcken
- dieselbe dbx-Type-II-Aufnahmekette wie bei echten Kassetten

## 5) Start des AUTO_CAL-Modus
- Serielle Konsole öffnen
- `l` senden (`start AUTO_CAL`)
- Preset-Status mit `J` prüfen

## 6) Erwarteter Ablauf
State-Machine:
- `AUTO_WAIT_FOR_TONE`
- `AUTO_MEASURE`
- `AUTO_COMPUTE`
- `AUTO_LOCKED` oder `AUTO_FAILED`

Erst bei stabiler 1-kHz-Erkennung werden valide Blöcke gesammelt.
Die Erkennung ist absichtlich tolerant gegen reale Deck-Abweichungen (Wow/Flutter/leichte Speed-Offsets):

- schmale Mehrbin-Prüfung um ~1 kHz (nicht nur exakt 1000.0 Hz),
- Stereo-Auswertung (L+R) statt nur linker Kanal,
- Freshness-Fenster mit gelatchten Analyzerwerten (statt "alle Analyzer exakt im selben Poll fresh"),
- kurzer Warmup/Debounce beim Start von `AUTO_WAIT_FOR_TONE`,
- LR-Mismatch wird bei sehr niedrigem Pegel nicht als harter Fehler gewertet,
- harte Reject-Gründe sichtbar, z. B. `reject_tone_too_weak`, `reject_unstable`, `reject_lr_mismatch`.

Wichtig: `AUTO_CAL` lockt nicht mehr nach wenigen Sekunden.  
Es werden längere Messfenster und mehrere valide Tonblöcke erwartet (3×30-s-Struktur mit kurzen Silences dazwischen toleriert).

## 6b) Roh-Telemetrie
- `J`: kompakter Status
- `K`: Rohtelemetrie (`toneL/toneR`, `peakL/peakR`, RMS-Proxies, Gate-Flags, Fresh-Data-Flags, Latched-Werte, Fresh-Ages, Blockzähler, Zeit im State, Reject-Reason)
- `L`: gelockte AUTO_CAL-Werte

## 7) Speicherung
- Nach erfolgreichem Lock (`AUTO_LOCKED`) optional mit `P` in EEPROM speichern.
- Keine periodischen EEPROM-Schreibzyklen im Dauerbetrieb.

## 8) Neu kalibrieren
- Messkassette erneut starten
- `l` erneut senden
- Neue Werte mit `L` prüfen
- Optional mit `P` persistieren
