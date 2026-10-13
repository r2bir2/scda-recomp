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

**REASON:** Update 197 hat die GESAMTE synchron sichtbare Aufrufkette von
`main()` bis zum Fatal-Callback (18 Funktionen, inkl. einer 3797-Zeilen-
Funktion) einzeln auf Zero-Fill-Muster durchsucht -- nichts gefunden, das
zu unserem Objekt passt. Kombiniert mit Update 195/196 (Allocator-Engine,
Disk-Read-Kette) ist damit praktisch der GESAMTE synchron erreichbare
Code auf dem Pfad zum Crash ausgeschlossen. Der Suchraum ist jetzt auf
zwei Erklaerungsklassen verengt (siehe unten); MAXIMUM bleibt angemessen
fuer die verbleibende, methodisch anspruchsvollere Suche (Cross-Thread
bzw. asynchroner Mechanismus).

**Seit Update 196 (diese Sitzung, gleiche Modellstufe fortgesetzt):**
- **Live `~*kb`-Dump (nicht-invasiv, kein Breakpoint) auf dem bereits
  abgestuerzten Prozess:** nur ZWEI Threads insgesamt -- Hauptthread
  (in der `jmp $`-Schleife) und `nv2a_ack_thread` (idle, `SleepEx`-Wait,
  unveraendert seit Update 180). Kein Worker-/Sound-Thread aktiv.
- **Kompletter Hauptthread-Stack deckt sich exakt mit der historischen
  Update-179-Kette:** `main -> ... -> sub_00048B15 -> sub_00048F1D ->
  sub_0004934E -> sub_00035DEE -> sub_0004BE42 -> sub_001CA0A5 ->
  sub_00065015 -> sub_00064FF3 -> sub_00064DEF -> sub_00064B00 ->
  sub_0007F412 -> sub_0007DE25 -> sub_00036582 -> sub_0007DC43 ->
  sub_00045738 -> sub_0004438C -> sub_00040821 -> [Fatal-Callback]`.
- **Jede dieser Funktionen auf `memset`/`stosd`/`stosb`/`stosw` geprueft:**
  13 davon ohne jeden Treffer; zwei mit einem Treffer, aber beide klar
  unrelated (`sub_00064B00`: lokaler 0x7F-Byte-Stack-Puffer;
  `sub_0007F412`: Fuellwert `0xFFFFFFFF` auf einem anderen, bereits als
  unabhaengig bekannten globalen Objekt). **Kein Treffer passt zu unserem
  Objekt/unserer Groesse/unserem Nullwert.**
- Damit ist die GESAMTE sichtbare Aufrufkette (Update 195: Allocator +
  sub_00048B15; Update 196: Disk-Read-Kette; Update 197: der Rest der
  main-Kette) lueckenlos ausgeschlossen.

**Repository:** `upgrade/selective-upstream`, HEAD weiterhin `79e6c19`.
Keine geaenderten getrackten Dateien ausser `PROJECT_STATUS.md`/
`docs/SCDA_WORKLOG.md`/neuen `tools/debug/append_update19{5,6,7}.py` +
`update_handoff_19{6,7}.py`; weiterhin viele absichtlich erhaltene
untracked Diagnoseartefakte.

**Aktueller Blocker (unveraendert):** Ringpuffer-Unterobjekt (`this`,
Vtable `0x4F6940`, Adresse `0x1135928`) ist am Fatal-Callback vollstaendig
genullt (`~0x8070` Bytes ab dem Allocator-Boundary-Tag). Der konkrete
Schreiber ist weiterhin nicht gefunden.

**Bewiesen (kumulativ):** Exakte Ausdehnung `0x8070` Bytes (Update 193);
Schreiber muss Chunk-Zeiger (`this-8`) nutzen (Update 195); Free() nullt
nie Nutzdaten (Update 195); die alte "falsches Handle"-Beobachtung
(Update 186, 2026-09-18) ist dasselbe Ereignis wie diese 0x8070-Nullung
(Update 196); nur 2 Threads existieren beim Absturz, kein lingernder
Worker (Update 197).

**Widerlegt (kumulativ):** Beide Destruktorpfade (Update 190); Vtable-
Familie mit gemeinsamer Basisklasse (Update 194); Allocator-Vtable Slots
0/4/8/0xC und `sub_00048B15` direkt (Update 195); komplette Disk-Read-/
NTSTATUS-/Fehlerbehandlungskette (Update 196); **die gesamte restliche
main()-Aufrufkette (13 weitere Funktionen, davon eine 3797 Zeilen lang)
als Zero-Fill-Quelle (Update 197)**.

**Offen (Stand nach Update 197):** Da praktisch der gesamte synchron
sichtbare Code jetzt ausgeschlossen ist, bleiben nur noch:
1. **Ein kurzlebiger Thread, der bereits vor dem Absturz wieder beendet
   wurde** -- durch den post-mortem `~*kb`-Dump nicht mehr sichtbar, nicht
   ausgeschlossen. Update 188 erwaehnte "mehrere Worker-/Sound-Threads",
   ohne das explizit fuer DIESEN Lauf zu belegen.
2. **Ein nicht-Thread-basierter asynchroner Mechanismus** (Exception-
   Handler, APC, OS-Callback) ausserhalb des normalen synchronen
   Aufrufpfads.
3. **(a)/(d) aus Update 195, niedrigere Prioritaet nach dieser Runde:**
   ein roher, separat gehaltener Zeiger, oder eigene Intrusive-
   Buchfuehrung -- aber die naheliegendsten Traeger dafuer (die gesamte
   main-Kette) sind jetzt ausgeschlossen.
4. **(b) aus Update 194, niedrigste Prioritaet:** eine voellig andere
   Vtable-Familie.

**Naechster Test (fuer MAXIMUM-Modell, Stand Update 197):**
1. Einfacher, NICHT verschachtelter Breakpoint (bewiesen zuverlaessig in
   Updates 180-182, im Gegensatz zu den gescheiterten `.if`/`.else`-
   Konstrukten) auf `bridge_PsCreateSystemThreadEx` mit trivialer Aktion
   (`.printf`; `g`) -- zaehlt/protokolliert JEDE Thread-Erzeugung ueber
   einen vollstaendigen Lauf, um zu pruefen, ob jemals mehr als der eine
   bekannte `nv2a_ack_thread` erzeugt wird (Kandidat 1 oben).
2. Alternativ: frischer Prozess, Timing-Eingrenzung wie Update 193, dann
   EIN `~*kb`-Dump gezielt IM Uebergangsfenster (nicht danach) -- zeigt,
   ob zum Uebergangszeitpunkt selbst ein dritter Thread aktiv ist.
3. Bedingte CDB-Breakpoints UND Hardware-Watchpoints weiterhin NICHT
   versuchen.
4. Sobald der GOOD->BAD-Uebergang auf eine einzelne Funktion eingegrenzt
   ist: xemu-Vergleich fuer genau diese Funktion/diesen Call.

**Relevante neue Artefakte:** `tools/debug/allthreads_post196.txt` (der
volle `~*kb`-Dump, Update 197).

**Laufende Prozesse / IN PROGRESS:** `scda_recomp.exe` PID `46460` laeuft
weiter, BAD-Zustand, jetzt auch als Quelle des `~*kb`-Thread-Dumps
verwendet -- fuer Kandidat 1/2 oben wird ein NEUER, frischer Prozess
benoetigt. Keine haengenden `cdb.exe`-Prozesse mehr aktiv. Upstream-Stand
unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
