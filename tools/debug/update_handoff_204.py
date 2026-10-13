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

**REASON:** Update 204 hat zwei weitere vollstaendige Mechanismus-Klassen
definitiv ausgeschlossen (Timer/APC: 0 Aufrufe in einem kompletten Lauf;
Virtual-Memory-Release-Pfad: 0 Aufrufe von NtFreeVirtualMemory). Der
Suchraum ist jetzt extrem eng, aber der konkrete Schreiber weiterhin nicht
gefunden. Die verbleibende Suche braucht entweder einen direkten Live-Fang
der Transition oder das Wiederaufgreifen der alten, raeumlich verbundenen
26.08.2026-Bugmeldung -- beides erfordert weiterhin hohe Analyse-Tiefe.

**Seit Update 197 (diese Sitzung, gleiche Modellstufe fortgesetzt) --
Kurzfassung der Updates 198-204:**
- **198-202 (paralleler Agent, siehe Commit `18e9f7b`):** 4 Thread-
  Erzeugungen bestaetigt (Main, 2x Bink `sub_004BBE90`, 1x Lipsync
  `sub_00036889`/`sub_00039566`). Beide Bink-Worker enden vor dem Fatal;
  Lipsync lebt danach weiter. Alle architektonischen Guest-Zustaende sind
  TLS-isoliert (kein Register-Sharing-Race moeglich). Ein aktiver Bink-
  Zero-Block wurde exakt vermessen und widerlegt (falsches Ziel/falsche
  Groesse). Ein zweiter Bink-Kandidat (`sub_004B8950`, dynamisch berechneter
  Ringpuffer-Clear) bleibt strukturell moeglich, aber zeitlich ausgeschlossen
  (Bink-Worker beenden sich vor dem Fatal). 3 von 4 Lipsync-internen
  indirekten Aufrufen sind aufgeloest und harmlos (Critical-Section-Lock/
  Unlock, Zeitberechnung); der 4. (`sub_00039566+0x183`) blieb offen.
- **203 (diese Sitzung):** Nebenfund fehlender Funktions-Seed `0x3E7B6`
  (trivialer Getter, 436.000+ mal faelschlich uebersprungen) geprueft und
  als generisches, objektunabhaengiges C++-Plumbing eingestuft --
  zurueckgestellt (Rebuild-Kosten >1h historisch, ohne garantiertes Signal).
- **204 (diese Sitzung, neu):**
  - **Timer/APC-Mechanismus (KeSetTimer) vollstaendig ausgeschlossen:**
    0 Aufrufe in einem kompletten Lauf bis zum Fatal-Callback.
  - **Lipsync-4.-Aufruf stark negativ:** 8 Minuten Echtzeit ohne Treffer,
    obwohl das Hauptobjekt laengst genullt war -- staerker als Update 202s
    "inconclusive", aber nicht absolut.
  - **Virtual-Memory-Release-Pfad vollstaendig ausgeschlossen:** Die
    Allocator-eigene OS-Speicher-Schrumpf-Kette (`sub_00076849`/
    `sub_00076CC6`/`sub_00371BF1`, aufgeloest als `NtFreeVirtualMemory`
    mit `MEM_RELEASE`) wird in einem realen Lauf NIE aufgerufen (0 von 0).
    Fand dabei bereits vorhandene, umfangreiche Dokumentation dieser
    exakten Funktionen aus einer FRUEHEREN, unabhaengigen Untersuchung
    (2026-09-01/02, `src/kernel/kernel_bridge.c`).
  - **Kontext (kein neuer Bug):** Unser Objekt liegt in einem per
    `xbox_HeapAlloc` reservierten 1-MB-Block (`0x010CC000..0x011CC000`),
    demselben Mechanismus, der auch Worker-Thread-Stacks verwaltet
    (`xbox_AllocThreadStack`/`xbox_FreeThreadStack`). Dieser spezielle
    Block wird im erfassten Lauf nie freigegeben.

**Repository:** `upgrade/selective-upstream`, HEAD weiterhin `79e6c19`
als letzter Code-Commit (nur Doku-Commits seither). Keine geaenderten
getrackten Dateien ausser `PROJECT_STATUS.md`/`docs/SCDA_WORKLOG.md`/
neuen `tools/debug/append_update20{3,4}.py` + `update_handoff_20{3,4}.py`;
weiterhin viele absichtlich erhaltene untracked Diagnoseartefakte.

**Aktueller Blocker (unveraendert):** Ringpuffer-Unterobjekt (`this`,
Vtable `0x4F6940`, Adresse `0x1135928`) ist am Fatal-Callback vollstaendig
genullt (`~0x8070` Bytes ab dem Allocator-Boundary-Tag). Der konkrete
Schreiber ist weiterhin nicht gefunden.

**Bewiesen (kumulativ, inkl. neu in Update 204):** Exakte Ausdehnung
`0x8070` Bytes (193); Schreiber muss Chunk-Zeiger nutzen (195); Free()
nullt nie Nutzdaten (195); alte "falsches Handle"-Beobachtung = dasselbe
Ereignis (196); nur 2 Threads beim Absturz (197); 4 Thread-Erzeugungen
insgesamt, Bink-Worker enden vorher, Lipsync lebt weiter (198-199);
**NEU: Timer/APC-Mechanismus wird nie genutzt (204); Virtual-Memory-
Release-Pfad wird nie aufgerufen (204).**

**Widerlegt (kumulativ, inkl. neu in Update 204):** Beide Destruktorpfade
(190); Vtable-Familie (194); Allocator-Slots 0/4/8/0xC + `sub_00048B15`
(195); Disk-Read-Kette (196); restliche main()-Kette (197); aktiver Bink-
Zero-Block mit falschem Ziel/Groesse (200); 3 von 4 Lipsync-Indirections
(202); **NEU: Timer-Queue-Mechanismus komplett (204); Virtual-Memory-
Release-Pfad komplett (204).**

**Offen (Stand nach Update 204):** Der Suchraum ist jetzt extrem eng.
Verbleibend mit nennenswerter Plausibilitaet:
1. **Ein bisher nicht identifizierter Mechanismus innerhalb des noch
   lebenden Lipsync-Workers**, jenseits des bereits gelesenen Codes (der
   4. Aufruf ist stark, aber nicht absolut negativ).
2. **Die alte, nie abschliessend verfolgte Verbindung zum 26.08.2026-
   Bugreport** (`sub_00056B22`, Freilisten-Korruption, Temp-Region
   `0x00F80000` -- raeumlich im SELBEN `xbox_HeapAlloc`-Bereich wie unser
   Objekt, siehe Update 192/204).
3. **Ein direkter Live-Fang der Transition selbst** -- Timing-Eingrenzung
   (Vorlage: Update 193) gefolgt von einem `~*kb`-Dump GENAU im
   Uebergangsfenster. Trotz mehrfacher Empfehlung noch nie tatsaechlich
   ausgefuehrt.
4. Niedrigste Prioritaet: (a)/(d) aus Update 195 (roher Zeiger/eigene
   Buchfuehrung), (b) aus Update 194 (andere Vtable-Familie).

**Naechster Test (fuer MAXIMUM-Modell, Stand Update 204):**
1. **Priorisiert:** Kandidat 3 -- frischen Prozess starten, mit der
   etablierten Polling-Technik das GOOD->ZERO-Uebergangsfenster auf
   ca. 1-2 Sekunden eingrenzen (Vorlage: `tools/debug/poll_extent_out.log`),
   dann GENAU in diesem Fenster EINEN `~*kb`-Dump (alle Thread-Stacks)
   statt nur eines Adress-Reads ausfuehren. Weiterhin reines Polling
   (kein Breakpoint, kein Watchpoint).
2. Falls das nichts Neues zeigt: Kandidat 2 -- die alte 26.08.2026-
   Bugmeldung (`sub_00056B22`) im Detail nachlesen und pruefen, ob sie
   denselben `xbox_HeapAlloc`-Block betrifft wie unser Objekt.
3. Bedingte CDB-Breakpoints UND Hardware-Watchpoints weiterhin NICHT
   versuchen.
4. Sobald der GOOD->BAD-Uebergang auf eine einzelne Funktion eingegrenzt
   ist: xemu-Vergleich fuer genau diese Funktion/diesen Call.

**Relevante neue Artefakte:** `tools/debug/cdb_timer_usage_check.txt` +
`_out.log` (Timer-Test, Update 204); `tools/debug/threadroutine_stderr.log`
(bereits vorhandenes, jetzt fuer NtAllocateVirtualMemory/NtFreeVirtualMemory/
[HEAP]-Analyse ausgewertet); `tools/debug/append_update20{3,4}.py`.

**Laufende Prozesse / IN PROGRESS:** Die historischen BAD-Prozesse PID
`46460`, `51104`, `13260`(inzwischen beendet) sind/waren vorhanden. Das
explizit geschuetzte Paar `cdb.exe` PID `25164` / `scda_recomp.exe` PID
`27552` bleibt unangetastet. Fuer den empfohlenen naechsten Test (Kandidat 3
oben) wird ein NEUER, frischer Prozess benoetigt. Upstream-Stand
unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
