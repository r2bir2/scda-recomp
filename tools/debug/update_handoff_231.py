# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG war ausreichend fuer die ganze Update-230/231-Runde.
Keine Eskalation auf MAXIMUM noetig.

**Seit Update 229 -- Update 231 (diese Sitzung):** Der Post-Fix-Steady-
State (Update 229 faelschlich als "Bink-Worker-Polling" bezeichnet) ist
ein ECHTER, aber ANDERER Bug als die 0x4AB668-Korruption, per xemu-
Ground-Truth bestaetigt (Update 230: 92 Live-Treffer am selben Guest-EIP
zeigen, echte Hardware liefert dort IMMER einen echten Heap-Pointer oder
sauber `0`, NIEMALS den bei uns beobachteten Wert `0x00390037`). Update
231 hat den falschen Wert eine Ebene weiter zurueckverfolgt: `ebx =
0x00390037` wird unveraendert durchgereicht durch `sub_0004934E` und
`sub_00048F1D` (keine von beiden schreibt je auf `ebx`) bis zu
`sub_00048B15`, dessen einzige eigene `ebx`-Zuweisung (`ebx = 0`, Zeile
15014) VOR einem Tail-Call in `sub_0004934E` liegt -- ein Widerspruch: `ebx`
sollte laut statischer Lesart `0` sein, ist aber live `0x00390037`.
Zwei Erklaerungen offen, noch nicht unterschieden: (1) ein Register-
Erhaltungs-Bug in einem von `sub_00048B15`s ~25 direkten Callees, oder
(2) ein anderer, statisch nicht beruecksichtigter Codepfad innerhalb
`sub_00048B15` selbst. Liste der 25 Callees in Update 231.

**Contig-Alias-Fix aus Update 229 bleibt bestaetigt und abgeschlossen** --
nichts in Update 230/231 betrifft ihn (andere Adressklasse, anderer
Mechanismus).

**Repository:** `upgrade/selective-upstream`. Zwei weitere Runden
temporaerer, vollstaendig zurueckgesetzter Instrumentierung in der
gitignoreten `recomp_0002.c` (git status/diff sauber) seit Update 230.
Keine getrackten Quelldateien geaendert.

**Bewiesen (kumulativ, inkl. neu in Update 231):** Alle Eintraege aus
Updates 109-230 gueltig. NEU: der falsche Wert ist ein reiner Pass-
Through durch mindestens zwei Funktionsebenen (`sub_0004934E`,
`sub_00048F1D`), sein tatsaechlicher Ursprung liegt in oder oberhalb von
`sub_00048B15`.

**Offen (Stand nach Update 231), priorisiert:**
1. Bisektion innerhalb von `sub_00048B15`: `ebx`s Wert an mehreren Stellen
   zwischen Zeile 15014 und dem Tail-Call bei Zeile 15529 live pruefen,
   um einzugrenzen, ob einer der ~25 gelisteten Callees `ebx` nicht
   korrekt erhaelt, oder ob ein anderer Codepfad genommen wird.
2. Nach Fund: Original-x86 an der Fundstelle gegen generierten Code
   vergleichen, Fix implementieren, Pipeline durchlaufen lassen,
   bestaetigen dass der Null-Vtable-Call verschwindet UND echter
   Spielfortschritt stattfindet.
3. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Instrumentierung:** Keine aktiv (vollstaendig zurueckgesetzt und
rueckgebaut bestaetigt).

**Relevante neue Artefakte:** `tools/debug/append_update231.py`,
`tools/debug/append_update231_status.py`, `tools/debug/update_handoff_231.py`
(Doku-Skripte).

**Laufende Prozesse / IN PROGRESS:** xemu laeuft ggf. noch (vom Nutzer
manuell gestartet/gespielt); kein scda_recomp.exe-Testprozess aktiv.
Upstream-Stand unveraendert (`origin/main` bei `766ecef`, Tags
`v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
