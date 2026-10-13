status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** MAXIMUM (seit Update 209, in Update 210 bestaetigt)

**MODEL RECOMMENDATION:** In Codex: staerkste verfuegbare Analyse-Stufe.
In Claude: staerkstes verfuegbares Modell mit hoechster Reasoning-Tiefe.

MODEL STATUS: ESCALATION RECOMMENDED
Tier: MAXIMUM
Reason: Update 210 hat per One-Shot-Source-Instrumentierung (keine
CDB-Polling mehr) zweifelsfrei bewiesen, dass der aktuelle Lauf
`sub_0003FCA5` NICHT ueber die drei in Update 208 gefundenen Aufrufstellen
erreicht, sondern ueber den globalen Callback-Zeiger `MEM32(0x5E6E0C)`, der
von `sub_00040821` (der Ringpuffer-Nachfuellfunktion) aufgerufen wird. Diese
Kette ist grossteils bereits aus Updates 109/117/173-182/186/187 bekannt --
die Sitzung hat also im Wesentlichen bestaetigt, dass die alte, nie geloeste
Update-190-Frage ("was nullt das Ringpuffer-Unterobjekt, bevor
`sub_00040821` es benutzt") weiterhin die eigentliche Root Cause ist, nicht
der in 207-209 verfolgte `sub_00038A43`-Pfad.

**Seit Update 209 -- Update 210 (diese Sitzung):**
One-Shot-Instrumentierung (Ringpuffer in `recomp_manual.c`, Deklarationen in
`recomp_types.h`, vier Aufrufstellen-Marker in `recomp_0001.c`, siehe
`SCDA-DISC-ERR-INSTRUMENTATION`) gebaut, um OHNE CDB den echten Aufrufer und
Status zu bestimmen. Ergebnis: (1) ein voellig ungestoerter Lauf reproduziert
NICHT innerhalb 36 Minuten (60x das erwartete ~30-36s-Fenster); (2) ein
einzelner nicht-invasiver Attach zeigt den Prozess in einer `jmp $`-
Selbstschleife GENAU INNERHALB von `sub_0003FCA5` (per Map-Datei bestaetigt,
nicht per potenziell falschem cdb-PDB-Symbol), unmittelbar nach einem
`RECOMP_ABI_CHECK`-Treffer (`recomp_abi_violation_log`, Guest-VA `0x49CC50`
-- neuer, noch nicht eingeordneter Datenpunkt); (3) die Ringpuffer-Globals
zeigen `dumped=1` aber `ring_count=0` -- KEINE der drei Update-208-
Aufrufstellen feuerte; (4) Instrumentierung auf `RECOMP_ICALL`/
`RECOMP_ICALL_SAFE`/`RECOMP_ITAIL` erweitert (da `sub_0003FCA5` auch einen
`recomp_dispatch.c`-Eintrag hat) -- Treffer: `RECOMP_ICALL_SAFE`, Rueckkehr-
Guest-Adresse `0x00040992`, innerhalb `sub_00040821`; (5) Quellcode-Trace
(kein cdb) zeigt: `sub_00040821` ruft nach erfolgreichem `sub_000516B7`
unbedingt ueber `MEM32(0x5E6E0C)`, das EINZIGE Mal beschrieben in
`sub_00048B15` (`= 0x3FCA5`, unbedingt, ganz am Anfang) -- deckt sich exakt
mit den bereits in Updates 109/117/173-182/186/187 dokumentierten Funden.

**Repository:** `upgrade/selective-upstream`, HEAD weiterhin `79e6c19` als
letzter Code-Commit vor dieser Sitzung; diese Sitzung hat die temporaere
Instrumentierung (siehe unten) als Arbeitsstand, noch nicht committet.

**Aktueller Blocker (unveraendert seit Update 190, jetzt praeziser
lokalisiert):** Ringpuffer-Unterobjekt (`this`, Vtable `0x4F6940`, historisch
`0x1135928`) ist zum Zeitpunkt von `sub_00040821`s Dispatch ueber
`MEM32(0x5E6E0C)` bereits vollstaendig genullt/nie initialisiert (Updates
187/190). Der konkrete Schreiber/die konkrete Nicht-Initialisierung ist
weiterhin nicht gefunden -- Update 210 hat NICHT versucht, dies neu zu
loesen, sondern nur den nachgelagerten Aufrufmechanismus (wie der Fehler zum
Dialog wird) unabhaengig bestaetigt.

**Bewiesen (kumulativ, inkl. neu in Update 210):** Alle Eintraege aus
Updates 190-209 UNVERAENDERT gueltig; **NEU: `sub_0003FCA5` wird in diesem
Build/Lauf ueber den indirekten Callback `MEM32(0x5E6E0C)` erreicht, NICHT
ueber die drei Update-208-Aufrufstellen (210, per Instrumentierung
bewiesen); dieser Mechanismus ist identisch mit dem bereits in Updates
109/117/173-182/186/187 dokumentierten (210); `sub_0003FCA5` macht intern
einen ABI-Check-verletzenden Aufruf zu Guest-VA `0x49CC50`, bevor es in
seine bekannte `jmp $`-Endlosschleife faellt (210, neu, nicht eingeordnet).**

**Widerlegt/relativiert (kumulativ, inkl. neu in Update 210):** Alle
bisherigen Eintraege bleiben gueltig als Aussagen ueber die drei Update-208-
Aufrufstellen selbst; **NEU relativiert: die Update-208-Annahme, dass DIESE
drei Aufrufstellen die fuer den echten Absturz relevanten sind -- fuer den
in Update 210 beobachteten Lauf/Build stimmt das nicht, der tatsaechliche
Pfad ist der aeltere, globale Callback-Mechanismus (210).**

**Offen (Stand nach Update 210), priorisiert fuer MAXIMUM:**
1. **Die eigentliche, seit Update 190 offene Root-Cause-Frage:** welcher
   konkrete Aufruf/Zustand laesst das Ringpuffer-Unterobjekt genullt/nie
   initialisiert zurueck, bevor `sub_00040821` es (nach erfolgreichem Read)
   fuer den Fatal-Callback-Dispatch verwendet? Empfohlene Methode (statt der
   in Updates 188/189 wiederholt unzuverlaessigen Hardware-Watchpoints):
   dieselbe Ringpuffer-Instrumentierungstechnik aus Update 210 auf
   `sub_00040821`s Eintrittspunkt anwenden (ecx + Unterobjekt-Felder
   `+0x3C`/`+0x4058`/`+0x405C` bei jedem Aufruf aufzeichnen, nur die letzten
   paar behalten, einmalig am Absturzpunkt statisch auslesen).
2. **Der neue `RECOMP_ABI_CHECK`-Fund (Guest-VA `0x49CC50`, aufgerufen aus
   `sub_0003FCA5`):** ist das bereits aus der aelteren ABI-Clobber-
   Untersuchung (docs Zeile ~8745) bekannt, oder eine neue, eigene Spur?
   Niedrigere Prioritaet als Punkt 1.
3. `sub_0007F412`/`sub_00064B00` an tiefen Host-Offsets (Update 205),
   `sub_00052A9C`s Geschwisterfunktionen `sub_00036541` (noch ungeprueft) --
   niedrigste Prioritaet, da Update 210 den Fokus zurueck auf die Update-190-
   Frage gelenkt hat.
4. Lipsync-Worker-Restrisiko (204); 26.08.2026-Bugreport (`sub_00056B22`);
   niedrigste Prioritaet: (a)/(d) aus 195, (b) aus 194.

**Naechster Test (Prioritaet, fuer MAXIMUM):**
1. Ringpuffer-Instrumentierung (Update-210-Technik) auf `sub_00040821`s
   Eintrittspunkt erweitern: `ecx`("this") + Unterobjekt-Felder pro Aufruf
   in einen Ring schreiben, NICHT auf stdout/stderr ausgeben, einmalig
   statisch auslesen sobald der Fatal-Callback erreicht ist (per demselben
   "warte bis eingefroren, dann ein einziger cdb-Read"-Muster wie in 210).
2. Sobald der genaue Aufruf mit bereits genulltem Unterobjekt isoliert ist:
   Quellcode/Map-Datei-Abgleich (kein Vertrauen in cdb-PDB-Symbole) auf den
   unmittelbar vorausgehenden Code anwenden, um den ersten falschen Zustand
   zu finden.
3. Weiterhin: kein Dauerpolling, keine bedingten Breakpoints ueber laengere
   Zeit, keine Wiederholung der in 188/189 gescheiterten Hardware-Watchpoint-
   Methode ohne neue Idee, was diesmal anders waere.

**Instrumentierung (WICHTIG, temporaer, siehe Update 210):** vier
`SCDA-DISC-ERR-INSTRUMENTATION`-Marker in
`tools/recomp/output/scda_v060/gen/recomp_0001.c`, drei Makro-Checks in
`tools/recomp/output/scda_v060/gen/recomp_types.h`
(`RECOMP_ICALL`/`RECOMP_ICALL_SAFE`/`RECOMP_ITAIL`), und ein Ringpuffer-Block
in `projects/scda/src/recomp_manual.c` sind aktuell noch vorhanden -- **sie
sind diagnostisch und duerfen NICHT als dauerhafter Fix bestehen bleiben**
(explizite Standing Instruction). Bleiben vorerst bestehen, da Naechster
Test #1 dieselbe Infrastruktur weiterverwendet; werden zurueckgesetzt, sobald
die Update-190-Root-Cause gefunden ist oder diese Untersuchung anderweitig
abgeschlossen wird.

**Relevante neue Artefakte:** `tools/debug/append_update210.py`,
`tools/debug/append_update210_status.py`, `tools/debug/update_handoff_210.py`.

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen mehr (beide
in dieser Runde gestarteten Instanzen, PID 8412 und 52048 sowie die
Nachfolgeinstanz 52548, wurden nach Extraktion ihrer Diagnosedaten sauber
per `taskkill` beendet). Das frueher geschuetzte Paar (`cdb.exe` PID 25164 /
`scda_recomp.exe` PID 27552) wurde in dieser Sitzung mit expliziter
Nutzerzustimmung beendet (siehe Update-210-Text) und existiert nicht mehr.
Upstream-Stand unveraendert (`origin/main` bei `766ecef`, Tags
`v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
