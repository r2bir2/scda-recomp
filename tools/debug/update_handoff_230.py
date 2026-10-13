# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG war ausreichend fuer diese ganze Runde (Diagnose +
xemu-Ground-Truth-Vergleich). Keine Eskalation auf MAXIMUM noetig -- das
xemu-Ergebnis war eindeutig, keine widerspruechlichen Zustaende.

**Seit Update 229 -- Update 230 (diese Sitzung, NEUE DIVERGENZ BESTAETIGT):**
Der Update-229-Steady-State wurde untersucht und war NICHT das, was Update
229 vermutete ("Bink-Worker-Polling"). Tatsaechlich: der HAUPTTHREAD haengt
in einem rekursiven Menue-/Szenen-Baum-Walk (`sub_00062237` ->
`sub_00060B2E` -> `sub_000600AD` -> `sub_0005F6FF`), der bei einem
bestimmten "Knoten" (`ebx = 0x00390037`) einen Null-Vtable-Call macht
(sicher uebersprungen, kein Crash) -- aber dieser Knoten-Pointer selbst ist
falsch. Per direktem XBE-Parsing bestaetigt: `0x00390037+0x38` ist eine
**festverdrahtete Konstante im Executable-Image** (`0x81660000`), keine
Laufzeitberechnung, keine fehlenden Disc-Daten (beide UMDs vorhanden und
erfolgreich gelesen).

**Per Live-xemu-GDB-Vergleich (92 Treffer am selben Guest-EIP
`0x000622D1`, adressidentisch auf beiden Plattformen) EINDEUTIG bestaetigt:
echte Hardware liefert an dieser Stelle IMMER entweder einen echten
dynamischen Heap-Pointer oder sauber `0` -- NIEMALS `0x00390037` oder
irgendeinen Wert im statischen Image-Bereich.** Das ist also kein
Sentinel-Design und kein Testumgebungsartefakt, sondern eine echte
Divergenz dieser Rekompilierung. Kein Bezug zum Contig-Alias-Fix aus
Update 229 (andere Adressklasse, anderer Mechanismus) -- dieser Fix bleibt
bestaetigt und wurde nicht wieder geoeffnet.

**Repository:** `upgrade/selective-upstream`. Eine Runde temporaerer,
vollstaendig zurueckgesetzter Instrumentierung in der gitignoreten
`recomp_0002.c` (git status/diff sauber); eine reine Lese-Ground-Truth-
Sitzung gegen xemu (keine Aenderungen dort). Keine getrackten
Quelldateien geaendert diese Runde.

**Bewiesen (kumulativ, inkl. neu in Update 230):** Alle Eintraege aus
Updates 109-229 gueltig (inkl. des Contig-Alias-Fixes). NEU: der
Post-Fix-Steady-State ist ein ECHTER, aber ANDERER Bug als die
0x4AB668-Korruption -- ein falscher (statischer Image-) Pointer speist
eine Menue-Baum-Traversierung, wo echte Hardware immer einen dynamischen
Heap-Pointer liefert.

**Offen (Stand nach Update 230), priorisiert:**
1. Den Erzeuger von `ebx=0x00390037` zurueckverfolgen: welcher Caller von
   `sub_00062237` (oder welcher Schreibzugriff auf ein Knoten-eigenes
   `+0x24`-Kind-Feld) liefert diese Adresse statt eines echten
   Heap-Pointers? Original-x86 gegen generierten Code an der Fundstelle
   vergleichen.
2. Nach Fund: Fix implementieren, Pipeline durchlaufen lassen, bestaetigen
   dass der Null-Vtable-Call verschwindet UND dass echter Spielfortschritt
   (nicht nur Steady-State) im Hauptmenue/danach stattfindet.
3. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Instrumentierung:** Keine aktiv (vollstaendig zurueckgesetzt und
rueckgebaut bestaetigt).

**Relevante neue Artefakte:** `tools/debug/append_update230.py`,
`tools/debug/append_update230_status.py`, `tools/debug/update_handoff_230.py`
(Doku-Skripte); `tools/debug/xemu_390037_check.py` (xemu-Ground-Truth-
Skript, wiederverwendbar fuer aehnliche Vergleiche).

**Laufende Prozesse / IN PROGRESS:** xemu laeuft ggf. noch (vom Nutzer
manuell weitergespielt); kein scda_recomp.exe-Testprozess aktiv. Upstream-
Stand unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
