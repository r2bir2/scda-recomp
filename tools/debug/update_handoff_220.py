# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG (fuer den Patch-Persistenz-Schritt); der neue
`sub_004A12B0`-Blocker ist unbewertet.

**MODEL RECOMMENDATION:** Vor jeder weiteren Runtime-Arbeit: den Fix aus
Update 220 dauerhaft machen (er lebt aktuell NUR in der gitignorten,
generierten `recomp_0001.c` und wuerde von einem vollstaendigen
Pipeline-Regenerate stillschweigend geloescht). Erst danach den neuen
Blocker (`sub_004A12B0`/`sub_004A1600`/`sub_004A1740`) untersuchen.

**Seit Update 218 -- Updates 219/220 (diese Sitzung, FIX GEFUNDEN,
IMPLEMENTIERT UND VERIFIZIERT):** Die Kette `stale_value -> Cursor-Formel ->
remaining_space -> memcpy-Unterlauf` wurde per gezielter One-Shot-
Instrumentierung (Ringpuffer + Wiederverwendung der bereits bewaehrten
Ausloese-Bedingung) luckenlos, numerisch bestaetigt:
`stale=7 -> cursor=0x3FF9 -> +8 (Header-Parse) -> cursor=0x4001 ->
remaining_space=0x1FFFF -> Unterlauf`. **KORRIGIERT die bisherige
`stale==-1`-Vermutung** -- `stale=7` ist ein legitimer, kleiner Wert. Der
wahre Defekt: die Formel nutzt eine festkodierte `0x4000`-Konstante statt
der bereits im selben Codeblock geladenen ECHTEN Transferzahl
(`MEM32(ebp-4)`).

**Fix implementiert** (kein Clamp -- Nutzung bereits vorhandener korrekter
Daten): die eine Instruktion je Cursor-Formel-Stelle entfernt, die die
echte Transferzahl unnoetig verwirft. Bei `loc_000408F0` wurde `edx=0x4000`
bewusst UNVERAENDERT gelassen (wird spaeter von der Kompaktierungs-
Zielformel gebraucht); nur `eax=edx` (die Kopie fuer die Cursor-Berechnung)
entfernt. Bei `loc_00040992` wurde `eax=0x4000` komplett entfernt. Auf alle
vier Textvorkommen (zwei Stellen x zwei duplizierte Funktionen) angewandt.

**Verifiziert:** frischer Build + Lauf zeigt der Prozess erreicht den alten
Korruptions-/Einfrierpunkt NICHT MEHR. Stattdessen fortschreitend zu einer
komplett neuen, in dieser gesamten Sitzung nie zuvor gesehenen Stelle
(`sub_004A12B0`/`sub_004A1600`/`sub_004A1740`), einer echten,
niedrig-CPU-Poll-/Wartschleife (~16% CPU, nicht ~100%), stabil ueber
mehrere Minuten -- das ist der naechste Blocker.

**KRITISCHER OFFENER PUNKT:** der Fix hat noch kein dauerhaftes Zuhause.
`recomp_0001.c` ist generiert und gitignored; ein "vollstaendiger
Recomp-Pipeline"-Regenerate (naechster Schritt der urspruenglichen
Standing-Instruction-Sequenz) wuerde ihn stillschweigend loeschen, da kein
Pro-Funktion-Patch-/Override-Mechanismus im Lifter gefunden wurde. **Vor
jedem Pipeline-Regenerate muss dieser Fix dauerhaft gemacht werden**
(z.B. als adressbasierte Lifter-Patch-Liste, ein Post-Generierungs-Patch-
Schritt, oder Verschiebung dieser Funktion in `recomp_manual.c`).

**Repository:** `upgrade/selective-upstream`. Der Fix selbst ist NICHT
committet (lebt nur in der gitignorten generierten Datei) -- nur die
Dokumentation (Updates 219/220) wird committet.

**Aktueller Blocker (zwei, in Prioritaetsreihenfolge):**
1. Fix-Persistenz-Mechanismus fehlt (siehe oben) -- MUSS vor einem
   Pipeline-Regenerate geloest werden.
2. Neuer, unbewerteter Blocker bei `sub_004A12B0`/`sub_004A1600`/
   `sub_004A1740` (niedrig-CPU-Poll-Schleife) -- noch nicht untersucht.

**Bewiesen (kumulativ, inkl. neu in Updates 219/220):** Alle Eintraege aus
Updates 109-218 unveraendert gueltig, MIT KORREKTUR: Update 217/218s
`stale==-1`-Vermutung war falsch -- der tatsaechliche Wert ist `stale=7`,
legitim; der Defekt liegt in der Formel selbst (festkodierte 0x4000-Annahme
statt echter Transferzahl), nicht im Eingabewert. **NEU: Fix implementiert
und verifiziert, Prozess erreicht neues, nie zuvor gesehenes Terrain (220).**

**Offen (Stand nach Update 220), priorisiert fuer STRONG:**
1. Dauerhaften Patch-Mechanismus fuer den Fix finden/bauen (Top-Prioritaet).
2. Danach: vollstaendige Recomp-Pipeline + canonical Build (jetzt SICHER,
   da der Fix dauerhaft ist), Runtime-Verifikation wiederholen.
3. Den neuen `sub_004A12B0`-Blocker untersuchen (Funktion lesen, Aufrufer
   `sub_004A1600`/`sub_004A1740` verstehen, klaeren was die Poll-Schleife
   erwartet und warum es nie eintritt).
4. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Instrumentierung:** Aktuell KEINE Diagnose-Instrumentierung aktiv (voller
Revert der `SCDA-DIAG-219`/`-220`-Marker erfolgt, verifiziert per Grep). Der
Fix selbst (`SCDA-FIX-215`, 4 Stellen) ist aktiv und bleibt es, bis er einen
dauerhaften Mechanismus hat.

**Relevante neue Artefakte:** `tools/debug/append_update219.py` (falls
erstellt) / `append_update220.py`, `append_update220_status.py`,
`update_handoff_220.py`, `tools/debug/diag219_orchestrate.sh` (neues,
dauerhaftes, wiederverwendbares Orchestrierungs-Werkzeug: frischer Prozess,
gebundenes Warten, `cdb`-Snapshot-Fallback, automatischer Neustart bei
bekanntem Fremd-Einfrieren).

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen. Upstream-
Stand unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
