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

## 7) Speicherung
- Nach erfolgreichem Lock (`AUTO_LOCKED`) optional mit `P` in EEPROM speichern.
- Keine periodischen EEPROM-Schreibzyklen im Dauerbetrieb.

## 8) Neu kalibrieren
- Messkassette erneut starten
- `l` erneut senden
- Neue Werte mit `L` prüfen
- Optional mit `P` persistieren
