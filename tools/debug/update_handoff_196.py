status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** MAXIMUM (unveraendert seit Update 192)

**MODEL RECOMMENDATION:** In Codex: staerkste verfuegbare Analyse-Stufe. In
Claude: staerkstes verfuegbares Modell mit hoechster Reasoning-Tiefe.

**REASON:** Update 196 hat zwei bisher getrennt gefuehrte Untersuchungs-
straenge (die alte "falsches Handle"-Spur, Updates 185-187, und die
diesjaehrige "0x8070-Byte-Nullung"-Spur, Updates 192-195) ursaechlich als
EIN UND DASSELBE Phaenomen identifiziert, UND die komplette Disk-Read-
Aufrufkette (`sub_00040821`->`sub_000516B7`->`xbox_NtReadFile`->
`sub_00052A85`/`sub_00052A57`) vollstaendig gelesen und ausgeschlossen.
Der Suchraum schrumpft kontinuierlich; MAXIMUM bleibt angemessen.

**Seit Update 195 (diese Sitzung, gleiche Modellstufe fortgesetzt):**
Update 196 hat `docs/SCDA_WORKLOG.md` Updates 173-191 (aus einer FRUEHEREN
Untersuchungsrunde, vor dieser Sitzung, bisher nur komprimiert bekannt) im
Detail nachgelesen und daraus eine praezisere zeitliche Eingrenzung
gewonnen:
- **Update 187 (alte Runde):** `ebx`(=`this`) live AM EINTRITTSPUNKT von
  `sub_0003FCA5` gelesen -- ALLE Felder inkl. Vtable bereits Null. Dieser
  Callback wird DIREKT aus `sub_00040821`s seltenem "Rest < 8 Bytes"-Zweig
  ausgeloest, sofort nachdem `sub_000516B7` einen Fehlschlag meldet. Das
  Objekt ist also spaetestens zu Beginn dieses letzten Calls bereits (oder
  wird dabei) genullt.
- **`sub_000516B7` (NTSTATUS-Uebersetzung, 237 Bytes) vollstaendig gelesen:**
  KEIN Schreibzugriff auf unser Objekt irgendwo -- nur Schreibzugriffe auf
  des Aufrufers Out-Parameter und einen eigenen Stack-lokalen
  `IO_STATUS_BLOCK`. AUSGESCHLOSSEN.
- **Fehlerpfad `sub_00052A85`->`sub_00052A57` gelesen:** reines
  SetLastError-Aequivalent (schreibt in einen Thread-lokalen Fehlercode-
  Slot), kein Objektbezug. AUSGESCHLOSSEN.
- **Wichtige Versoehnung:** Update 186 (alte Runde) deutete `handle=0` beim
  fehlgeschlagenen `NtReadFile` als "Code liest Handle faelschlich aus
  Data-Plane-Speicher". Nach vollstaendigem Lesen von `sub_00040821`
  steht fest: der Code liest korrekt aus `MEM32(ebx+0x3C)` -- kein
  Offset-Fehler. Einzige Erklaerung, die beide Beobachtungen vereint:
  `ebx` (das GANZE Objekt) ist zu diesem Zeitpunkt bereits vollstaendig
  genullt. **Die "falsches Handle"-Beobachtung von 2026-09-18 UND die
  "0x8070-Byte-Nullung" von 2026-09-20 sind derselbe Bug.**

**Repository:** `upgrade/selective-upstream`, HEAD weiterhin `79e6c19`
(Frame-Pointer-Fix, behebt den Blocker nachweislich NICHT). Keine
geaenderten getrackten Dateien ausser `PROJECT_STATUS.md`/
`docs/SCDA_WORKLOG.md`/neuer `tools/debug/append_update19{5,6}.py`;
weiterhin viele absichtlich erhaltene untracked Diagnoseartefakte.

**Aktueller Blocker (unveraendert):** Nach dem durch xemu belegten
korrekten Short-Read erreicht der Recomp-Lauf den originalen
Fatal-Callback `sub_0003FCA5`; am Callback ist das Ringpuffer-Unterobjekt
(`this`, Vtable `0x4F6940`, stabile Adresse `0x1135928`) vollstaendig
genullt (`~0x8070` Bytes ab dem Allocator-Boundary-Tag). Der konkrete
Schreiber ist weiterhin nicht gefunden.

**Bewiesen (kumulativ, inkl. neu in Update 196):**
- Exakte Ausdehnung `~0x8070` Bytes, Ende bytegenau bei `this+0x8068`
  (Update 193).
- Der Schreiber muss mit einem Chunk-Zeiger (`this-8`) arbeiten, nicht dem
  User-Zeiger (Update 195).
- Free() nullt nie Nutzdaten -- ein reiner Free-Bug reicht nicht als
  Erklaerung (Update 195).
- **NEU:** Die alte "falsches Handle beim NtReadFile"-Beobachtung
  (Update 186, 2026-09-18) UND die aktuelle "0x8070-Byte-Nullung" sind
  erwiesenermassen dasselbe Ereignis -- kein separater Zeiger-/Offset-
  Fehler beim Handle-Lesen, sondern Folge der Objekt-Nullung selbst.
- **NEU:** Die komplette Aufrufkette `sub_00040821` -> `sub_000516B7` ->
  `xbox_NtReadFile` -> `sub_00052A85` -> `sub_00052A57` ist lueckenlos
  gelesen und schreibt an keiner Stelle auf unser Objekt.

**Widerlegt (kumulativ, inkl. neu in Update 196):**
- Beide bekannten Destruktorpfade (`sub_000407F7`, `sub_00040B00`) --
  Update 190.
- Die Vtable-Familie mit gemeinsamer Basisklasse -- Update 194.
- Allocator-Vtable Slots 0/4/8/0xC (Allocate/Realloc/Free/Hook) und
  `sub_00048B15` direkt -- Update 195.
- **NEU:** Die komplette Disk-Read-/NTSTATUS-/Fehlerbehandlungskette
  (`sub_00040821`/`sub_000516B7`/`xbox_NtReadFile`/`sub_00052A85`/
  `sub_00052A57`) -- Update 196.
- **NEU:** Update 186s eigene "Data-Plane statt persistentem Feld"-
  Diagnose als SEPARATE Ursache -- sie ist nur ein Symptom derselben
  0x8070-Nullung, kein eigener Bug.

**Offen (Stand nach Update 196):** Der konkrete Schreiber ist weiterhin
nicht gefunden. Da praktisch der gesamte objekt-eigene Code (Konstruktor,
Refill, Close, Destruktor, geteilte Basisklassen-Methoden) UND die
gesamte Allocator-Engine UND die gesamte Disk-Read-Kette jetzt
ausgeschlossen sind, verbleiben:
1. **Cross-Thread-Effekt jenseits der beiden bekannten Destruktoren**
   (Update 188/190 pruefte NUR `sub_00040B00`/`sub_000407F7`, nicht
   "irgendeine andere Thread-Aktivitaet allgemein"). Update 189s
   Hardware-Watchpoint-Versuch dazu war inkonklusiv (Tooling-Problem);
   NICHT erneut mit Watchpoints versuchen (User-Vorgabe).
2. **(a)/(d) aus Update 195:** ein roher, separat gehaltener Zeiger auf
   unser Objekt, oder Code mit eigener `ptr-4`/`ptr-8`-Buchfuehrung, noch
   nicht lokalisiert.
3. **(b) aus Update 194, niedrige Prioritaet:** eine voellig andere
   Vtable-Familie.

**Naechster Test (fuer MAXIMUM-Modell, Stand Update 196):**
1. Frischen Prozess starten, mit der etablierten Polling-Technik
   (Update 193, `tools/debug/poll_extent_out.log` als Vorlage) das
   GOOD->ZERO-Uebergangsfenster auf ca. 1-2 Sekunden eingrenzen, dann
   GENAU in diesem Fenster EINEN nicht-invasiven `cdb -pv`-Aufruf mit
   `~*kb` (alle Thread-Stacks statt nur eines Adress-Reads) ausfuehren.
   Weiterhin reines Polling (kein Breakpoint, kein Watchpoint) -- prueft,
   ob ein ANDERER Thread als Thread 0 zu diesem Zeitpunkt in einer
   verdaechtigen Funktion steckt.
2. Bedingte CDB-Breakpoints UND Hardware-Watchpoints weiterhin NICHT
   versuchen.
3. Sobald der GOOD->BAD-Uebergang auf eine einzelne Funktion eingegrenzt
   ist: xemu-Vergleich fuer genau diese Funktion/diesen Call.

**Relevante neue Artefakte (Update 196):** `tools/debug/append_update196.py`,
`tools/debug/update_handoff_196.py` (dieses Update); keine neuen Live-
Trace-Artefakte, reine Code-Audit-Runde (Nachlesen alter Worklog-Eintraege
plus vollstaendiges Lesen dreier weiterer Funktionen).

**Laufende Prozesse / IN PROGRESS:** `scda_recomp.exe` PID `46460` laeuft
weiter, bereits im BAD-Zustand (per `cdb -pv`-Poll bestaetigt) -- nicht
beenden, aber fuer die naechste Live-Untersuchung (Punkt 1 oben) wird ein
NEUER, frischer Prozess benoetigt (dieser hier ist schon post-transition).
Keine haengenden `cdb.exe`-Prozesse mehr aktiv. Upstream-Stand unveraendert
(`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
