# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG (seit Update 213, in Updates 214-218 bestaetigt)

**MODEL RECOMMENDATION:** Normale Arbeitsstufe. Naechster Schritt ist eine
einzelne, klar spezifizierte Speicher-Erfassung (Wiederholung von
Update 217/218s DIAG-218-Versuch), blockiert nur durch Lauf-zu-Lauf-
Nichtdeterminiertheit, kein offenes Reverse-Engineering.

**Seit Update 217 -- Update 218 (diese Sitzung, HYPOTHESE WIDERLEGT,
MECHANISMUS WEITER VERFEINERT, WICHTIGE UMGEBUNGS-NICHTDETERMINIERTHEIT
ENTDECKT):** Update 217s eigene "vereinheitlichte Hypothese" (aeussere
Schleife ruft einmal zu viel auf) wurde per Live-Erfassung DIREKT WIDERLEGT:
`outer_remaining=00000000` im Moment der Korruption -- das ist die normale,
letzte Schleifeniteration, keine zusaetzliche. Der mitgemessene
`cursor=00004001` passt exakt zur bereits in Update 215 gefundenen
fehlerhaften Formel mit Eingabewert -1, was nahelegt: **der Cursor-Formel-
Defekt allein reicht aus, um die Korruption zu erklaeren** -- keine
zusaetzliche Terminator- oder Iterationszahl-Erklaerung noetig.

Ein Folge-Test (DIAG-218, sollte die -1-Arithmetik direkt verifizieren)
konnte diese Runde NICHT abgeschlossen werden. Grund war KEIN Bug in der
neuen Instrumentierung, sondern eine neu entdeckte, wichtige Tatsache: **das
bereits aus Update 210 bekannte `sub_0003FCA5`-`jmp $`-Einfrieren kann in
verschiedenen Laeufen ueber VERSCHIEDENE Streams zuerst erreicht werden**
(asynchrones Disk-I/O -> nicht-deterministische Abschluss-Reihenfolge).
Zwei von vier Laeufen dieser Runde froren auf einem ANDEREN Stream ein,
bevor sie `common.lin` erreichten -- verifiziert per Live-`cdb -pv`-Snapshot
(nicht gekillt), nicht als Bug, sondern als bereits bekanntes, charakter-
isiertes Verhalten erkannt.

**Durchgefuehrte Housekeeping-Aktion:** beide temporaeren One-Shot-
Erfassungen dieser Runde (`SCDA-DIAG-217`, `SCDA-DIAG-218`) vollstaendig aus
`recomp_0001.c` entfernt (inkl. `#include <stdio.h>`), per Grep verifiziert,
sauber neu gebaut.

**Repository:** `upgrade/selective-upstream`. Kein Fix committet.

**Aktueller Blocker:** die DIAG-218-Bestaetigung (stale-Wert exakt -1?)
steht noch aus, blockiert nur durch Nichtdeterminiertheit welcher Stream
zuerst das bekannte Einfrieren erreicht -- kein inhaltlicher Blocker mehr.

**Bewiesen (kumulativ, inkl. neu in Update 218):** Alle Eintraege aus
Updates 109-217 unveraendert gueltig, MIT EINER KORREKTUR: Update 217s
"vereinheitlichte Hypothese" (aeussere Schleife iteriert einmal zu oft) ist
WIDERLEGT (218) -- stattdessen ist der Cursor-Formel-Defekt aus Update 215
wahrscheinlich allein ausreichend, angewendet auf die GANZ NORMALE letzte
Schleifeniteration. **NEU: `sub_0003FCA5`-Einfrieren kann lauf-zu-lauf
ueber unterschiedliche Streams zuerst erreicht werden (218) -- wichtige
Debugging-Umgebungs-Tatsache fuer alle kuenftigen Laeufe in diesem
Bereich.**

**Offen (Stand nach Update 218), priorisiert fuer STRONG:**
1. DIAG-218-artige Erfassung wiederholen (voller Sequenz-Log der Cursor-
   Formel-Updates fuer das konkrete Objekt), mit Live-`cdb -pv`-Snapshots
   statt festem Timeout, um frueh zu erkennen ob ein Lauf auf einem anderen
   Stream eingefroren ist (dann sofort neu starten statt warten).
2. Sobald bestaetigt, dass der Eingabewert der Cursor-Formel auf recomp
   tatsaechlich -1 (oder aehnlich) ist: zurueckverfolgen, WARUM dieser Wert
   auf recomp so entsteht, aber (mutmasslich) nicht auf echter Hardware.
3. Danach erst Fixstrategie waehlen (Cursor-Formel-Fix, nicht Clamp am
   memcpy selbst, gemaess Standing Instruction).
4. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Naechster Test (Prioritaet, fuer STRONG):** siehe Update 218, letzter
Abschnitt. Kurz: DIAG-218 wiederholen, mit `cdb -pv`-Zwischenchecks statt
langem Warten, um Nichtdeterminiertheit zu umgehen.

**Instrumentierung:** Aktuell KEINE aktiv (voller Revert erfolgt).

**Relevante neue Artefakte:** `tools/debug/append_update218.py`,
`tools/debug/append_update218_status.py`, `tools/debug/update_handoff_218.py`
(reine Doku-Skripte, keine Instrumentierung).

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen (alle
gestarteten scda_recomp.exe-Instanzen dieser Runde wurden nach Snapshot-
Verifikation sauber beendet). Upstream-Stand unveraendert (`origin/main`
bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
