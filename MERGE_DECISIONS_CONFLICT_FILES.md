# Merge-Entscheidungsliste (Konfliktdateien)

Ziel: PR-Konflikte in den 4 gemeldeten Dateien direkt auflösen.

## 1) README.md
**Entscheidung:** `take-branch` (Feature-Branch-Version)

**Warum:**
- enthält aktuelle Scope-Grenzen (ein Universalprofil, kein USB-Audio, kein Mehrprofil).
- enthält CPU/RAM-Stability-Notes und reproduzierbaren Check-Block.
- ist konsistent mit aktuellem Code- und Teststand.

**Nur falls im Main neuere Projekt-Metadaten stehen:**
- diese als kurze zusätzliche Zeilen **manuell** übernehmen (kein Zurückrollen der technischen Sections).

---

## 2) FINAL_VALIDATION_ocx_type2_teensy.md
**Entscheidung:** `take-branch` (Feature-Branch-Version)

**Warum:**
- dokumentiert explizit die Ehrlichkeitsgrenze (kein Bitexact-/Referenz-Claim).
- enthält aktuelle Offline-Sweep-/Score-Dokumentation.
- trennt klar Build-validiert vs offline-validiert vs hardware-offen.

**Optional manuell aus Main übernehmen:**
- formale Header/Datumszeilen, falls Main dort zusätzliche Governance-Texte hat.

---

## 3) ocx_type2_harness.py
**Entscheidung:** `take-branch` (Feature-Branch-Version)

**Warum:**
- Branch-Version ist vollständig lauffähig (py_compile/pytest grün).
- enthält die benötigte Abstimmungslogik (Score, Sweeps, zusätzliche Stressfälle).
- enthält robuste Längenangleichung im Compare-Pfad.

**Manuelle Nacharbeit nach take-branch:**
- kurz prüfen, ob Main neue CLI-Optionen hinzugefügt hat; falls ja, gezielt ergänzen ohne Regression.

---

## 4) tests/test_ocx_type2.py
**Entscheidung:** `take-branch` (Feature-Branch-Version)

**Warum:**
- Branch-Tests prüfen die relevanten Reparaturziele (Sync, 44.1-kHz-Pfad, Shapes, Compare-Längen).
- Main-Variante war im Vergleichsstand unvollständig/inkonsistent.

**Manuelle Nacharbeit nach take-branch:**
- falls Main neue Test-Konventionen hat (Fixtures/Marker), nur die Konvention übernehmen, Testinhalt behalten.

---

## Konkreter CLI-Ablauf zum Auflösen

> Im Konfliktzustand (`git status` zeigt unmerged paths):

```bash
# alle 4 Konfliktdateien auf Branch-Version setzen
git checkout --ours README.md FINAL_VALIDATION_ocx_type2_teensy.md ocx_type2_harness.py tests/test_ocx_type2.py

# aufgelöst markieren
git add README.md FINAL_VALIDATION_ocx_type2_teensy.md ocx_type2_harness.py tests/test_ocx_type2.py

# Pflichtchecks
rg '^(<<<<<<<|=======|>>>>>>>)' .
git diff --check
python -m py_compile ocx_type2_wav_sim.py
python -m py_compile ocx_type2_harness.py
python -m py_compile tests/test_ocx_type2.py
pytest -q
pio run -e teensy41

# Merge/Rebase abschließen
git commit -m "Resolve merge conflicts by taking branch versions for docs/harness/tests"
```

## Fallback-Regel
Wenn einer der Pflichtchecks nach `take-branch` rot wird:
- **kein globales rollback**, sondern genau den betroffenen Hunk manuell aus Main übernehmen.
- danach denselben Check erneut laufen lassen.

## Sofort-Variante (ein Befehl)

```bash
./resolve_merge_conflicts.sh
```
