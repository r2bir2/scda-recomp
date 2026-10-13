# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG fuer die Audit-Arbeit war ausreichend. Die
eigentliche FIX-Entscheidung (siehe unten) ist eine Architektur-Frage
und sollte vor der Implementierung mit dem Nutzer besprochen werden.

**Seit Update 227 -- Update 228 (diese Sitzung, GRUNDURSACHE
BESTAETIGT):** Auf explizite Nutzer-Anweisung wurde der Contiguous-/
Tiled-Memory-Allokator direkt auditiert (statt weiter Funktionsebene fuer
Funktionsebene zurueckzuverfolgen). Ergebnis: **kein Overflow-/
Bounds-Bug**, sondern eine echte architektonische Luecke:

- Basis-RAM (`0x00000000+`) und die Tiled Aperture (`0xF0000000+`) sind
  beide Views *derselben* File-Mapping-Section -- korrekt aliasend, wie
  auf echter Hardware.
- Das Contiguous-Fenster (`0x80000000+`) ist ein **separates,
  eigenstaendiges** `VirtualAlloc` -- laut eigenem Code-Kommentar
  ABSICHTLICH nicht mit dem Basis-RAM verknuepft ("nothing here depends
  on the aliasing").
- Diese Annahme ist falsch fuer D3D-/Treiber-Code, der eine
  Contig-allozierte Oberflaeche ueber ihren Tiled-Aperture-Alias
  anspricht (`0xF0000000 | (VA & 0x0FFFFFFF)`, echte Hardware-Formel,
  bereits im Code dokumentiert): die Tiled Aperture loest das dann zum
  physischen Offset im BASIS-RAM auf, nicht zum tatsaechlichen
  Contig-Speicher.
- Das Device-Pointer-Global (`0x004AB668`, Basis-RAM) liegt zufaellig
  innerhalb des physischen Offset-Bereichs, den eine Contig-Allokation
  (`0x464000` Bytes in ihr eigenes 64-MB-Fenster) beim Tiled-Alias
  trifft -- daher die Korruption.

**Explizit geprueft und ausgeschlossen:** 32-Bit-Wraparound (bereits per
`uint64_t`-Cast verhindert), Alignment-Fehler, Groessenarithmetik-Fehler,
Kollision mit NV2A-/MCPX-MMIO-Aperturen (beide unabhaengig von diesem
Bereich).

**Repository:** `upgrade/selective-upstream`. Reine Lese-/Analyse-Arbeit
diese Runde, keine Instrumentierung, keine Quelldateien geaendert.

**Bewiesen (kumulativ, inkl. neu in Update 228):** Alle Eintraege aus
Updates 109-227 gueltig. NEU: die Grundursache der 0x4AB668-Korruption
ist vollstaendig verstanden -- eine architektonische Aliasing-Luecke
zwischen dem Contiguous-Fenster und der Tiled Aperture, kein
Copy-Loop-Bug, kein Overflow, kein Original-x86-Uebersetzungsfehler.

**Offen (Stand nach Update 228), priorisiert:**
1. **Fix-Strategie mit dem Nutzer besprechen** (Architektur-Entscheidung,
   nicht trivial): (a) Tiled Aperture so erweitern, dass sie sowohl
   Basis-RAM als auch das Contiguous-Fenster bei ihren jeweiligen
   physischen Offsets korrekt erreicht (braucht ein Verfahren, um zu
   wissen, zu welcher Apertur ein gegebener physischer Offset "gehoert"),
   oder (b) dem Contig-Allokator eine eigene, nicht mit dem Basis-RAM
   ueberlappende Tiled-Adressraum-Scheibe geben, oder (c) eine andere
   Loesung.
2. Nach Fix-Entscheidung: implementieren, komplette Pipeline durchlaufen
   lassen, bestaetigen dass der `sub_004A12B0`-Blocker verschwunden ist.
3. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert
   (u.a. der separate "manche Aufrufe der Zeilenkopie schreiten nie
   fort"-Defekt aus Update 224).

**Instrumentierung:** Keine aktiv.

**Relevante neue Artefakte:** `tools/debug/append_update228.py`,
`tools/debug/append_update228_status.py`, `tools/debug/update_handoff_228.py`
(reine Doku-Skripte).

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen. Upstream-
Stand unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
