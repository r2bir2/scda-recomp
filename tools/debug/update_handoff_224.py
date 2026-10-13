# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG. Die Bisektion selbst war STRONG-angemessen
(systematisch, viele kleine, praezise Live-Checks). Die Aliasing-Frage
im naechsten Schritt (Speicher-Mapping-Audit) ist noch bug-fokussierte
Arbeit, keine neue Subsystem-Frage.

**Seit Update 223 -- Update 224 (diese Sitzung):** per expliziter
Bisektions-Anweisung wurde die 0x4AB668-Korruption von "irgendwo
zwischen Geraeteerstellung und sub_004A12B0" auf eine konkrete
Zeilenkopier-Schleife in `sub_004AED5D` (`recomp_0027.c:8016-8041`)
eingegrenzt, ueber die Kette `sub_0004934E -> sub_00037274 ->
sub_002E5F95 -> sub_004AE0A7 -> sub_004ADE9D -> sub_004B0F92 ->
sub_004AED5D`. Dabei: (1) ein echter, unabhaengiger Defekt gefunden --
die Schleife schreitet nie fort (esi/edi eingefroren ueber 480+
Iterationen, Pitch liest sich als Null); (2) bewiesen, dass diese
Schleifes eigener Zielbereich (`[0x398,0x1228]`) 0x4AB668 nicht
ueberlappt; (3) eine Thread-Race-Hypothese direkt per Alle-Threads-Dump
widerlegt (nur Thread 0 aktiv, alle anderen schlafen); (4) Plan B
(Hardware-Watchpoint), diesmal praezise auf ein ~15-Iterationen-Fenster
eingegrenzt, hat trotzdem NICHT ausgeloest -- ein viel schluessigeres
Negativergebnis als der breite Versuch in Update 223; (5) 0x7400087D
kommt nirgendwo sonst im 64-MB-Guest-RAM vor (nicht schluessig, aber wie
angefordert geprueft).

**Arbeitshypothese (nicht bestaetigt):** Host-Speicher-Aliasing -- der
Adressbereich um 0x4AB668 (oder der Schleifen-Zielbereich) koennte ueber
mehr als eine Host-VA erreichbar sein, sodass ein auf der kanonischen
Adresse scharf gemachter Watchpoint einen ueber eine andere Host-VA
laufenden Schreibzugriff nicht sieht. Das ist eine bereits 2026-09-06
("continued 53") als offen markierte, nie geschlossene Luecke in diesem
Projekt.

**Der konkrete Writer wurde noch NICHT gefunden.** Der Nutzer-Auftrag
("Eingabewert/Quelle bis zum ersten falschen Zustand zurueckverfolgen und
Original-x86 gegen generierten Code vergleichen") kann erst nach
Identifikation des Writers erfuellt werden.

**Repository:** `upgrade/selective-upstream`. Nur Dokumentation und
Live-Debugging diese Runde (Update 224), kein Code geaendert.

**Bewiesen (kumulativ, inkl. neu in Update 224):** Alle Eintraege aus
Updates 109-223 unveraendert gueltig, mit einer Praezisierung: der
korrumpierende Schreibzugriff liegt nachweislich NICHT in
sub_004AED5Ds eigenem Zeilenkopier-memcpy (Zielbereich passt nicht) und
NICHT in einem anderen Thread (alle anderen Threads schlafen zum
Korruptionszeitpunkt).

**Offen (Stand nach Update 224), priorisiert:**
1. Guest-Speicher-Mapping/-Allokations-Code auditieren
   (`src/kernel/xbox_memory_layout.c`, `kernel_bridge.c`s
   `NtAllocateVirtualMemory`-Pfad) auf mehrfache Host-VA-Aliasing fuer
   den Adressbereich um 0x4AB668.
2. Falls kein Aliasing gefunden wird: Page-Guard-basierte Speicherfalle
   statt Hardware-Debug-Register-Watchpoint versuchen.
3. Den unabhaengig gefundenen "Zeilenkopier-Schleife schreitet nie fort"-
   Defekt (sub_004AED5D, Pitch liest sich als Null) separat vermerken --
   moeglicherweise verwandt, moeglicherweise eigener Bug, nicht
   root-caused.
4. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Instrumentierung:** Keine aktiv. Mehrere cdb-Kommandoskripte im
Scratchpad fuer die naechste Runde wiederverwendbar (Bisektions-Kette
komplett dokumentiert in Update 224, kann bei Bedarf exakt reproduziert
werden).

**Relevante neue Artefakte:** `tools/debug/append_update224.py`,
`tools/debug/append_update224_status.py`, `tools/debug/update_handoff_224.py`
(reine Doku-Skripte).

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen. Upstream-
Stand unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
