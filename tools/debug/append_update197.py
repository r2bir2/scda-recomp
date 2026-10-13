status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 197 (2026-09-20): Live Alle-Threads-Dump zeigt nur 2 Threads (kein hängender Worker); komplette sichtbare Aufrufkette main()->Fatal-Callback auf Zero-Fill geprüft -- überall sauber

**Test:** Ein einzelner, nicht-invasiver `cdb -pv -p <PID> -c "~*kb; q"` gegen den bereits abgestürzten Prozess (PID 46460, weiterhin am Leben aus Update 195/196) -- keine Breakpoints, keine Watchpoints, minimal-invasiv wie in Update 193 etabliert.

**Ergebnis 1 -- nur zwei Threads insgesamt:** Thread 0 (Hauptthread) steckt in der bekannten `sub_0003FCA5`-`jmp $`-Schleife; Thread 1 ist `nv2a_ack_thread`, unveraendert im `SleepEx`-Wartezyklus (identisch zu Update 180s Befund). **Kein einziger Worker-/Sound-Thread existiert zu diesem Zeitpunkt.** Das schwaecht (ohne es vollstaendig auszuschliessen -- ein kurzlebiger Thread koennte laengst beendet sein) die in Update 188 erwaehnte Idee "mehrere Worker-/Sound-Threads laufen parallel" fuer DIESEN Lauf: falls hier ueberhaupt je mehr als zwei Threads existierten, sind sie zum Absturzzeitpunkt bereits wieder verschwunden.

**Ergebnis 2 -- komplette Aufrufkette von `main()` bis zum Fatal-Callback sichtbar und deckt sich exakt mit der historischen Update-179-Kette:** `main` -> `xbe_entry_point` -> ... -> `sub_00048B15` -> `sub_00048F1D` -> `sub_0004934E` -> `sub_00035DEE` -> `sub_0004BE42` -> `sub_001CA0A5` -> `sub_00065015` -> `sub_00064FF3` -> `sub_00064DEF` -> `sub_00064B00` -> `sub_0007F412` -> `sub_0007DE25` -> `sub_00036582` -> `sub_0007DC43` -> `sub_00045738` -> `sub_0004438C` -> `sub_00040821` -> [Fatal-Callback].

**Jede einzelne Funktion in dieser Kette wurde jetzt (diese Runde) gezielt auf `memset`/`rep stosd`/`stosb`/`stosw`-Uebersetzungsmuster durchsucht** (die alten Updates 177/178 hatten dieselben Funktionen nur auf "Stop-Zaehler"-Logik geprueft, NICHT explizit auf Zero-Fills):
- `sub_00048F1D`, `sub_0004934E` (3797 Zeilen!), `sub_00035DEE`, `sub_0004BE42`, `sub_001CA0A5`, `sub_00065015`, `sub_00064FF3`, `sub_00064DEF`, `sub_0007DE25`, `sub_00036582`, `sub_0007DC43`, `sub_00045738`, `sub_0004438C`: **KEIN Treffer.**
- `sub_00064B00`: EIN `stosd`+`stosw`-Treffer, aber Ziel ist `ebp + -518` -- ein LOKALER STACK-Puffer (Groesse `0x7F`, passend zu Update 178s "sprintf-aehnlich, 0x7F/0xFF-Puffergroessen"), kein Heap-/Objektbezug.
- `sub_0007F412`: EIN `stosd`-Treffer, aber Fuellwert ist `0xFFFFFFFF` (NICHT Null!) auf einem ANDEREN, unabhaengigen Objekt (`esi+0xF0`, das bereits in Update 178 als "voellig unabhaengiges globales Array" identifizierte Ziel) -- passt weder in der Groesse noch im Fuellwert noch in der Zieladresse zu unserem Bug.
- `sub_00040821`, `sub_000516B7`, `xbox_NtReadFile`, `sub_00052A85`/`sub_00052A57`: bereits in Update 195/196 einzeln geprueft, sauber.

**Ergebnis: Die GESAMTE, tatsaechlich zur Laufzeit durchlaufene Aufrufkette von `main()` bis zum Fatal-Callback enthaelt an KEINER Stelle einen Zero-Fill, der auf unser Objekt zutreffen koennte.** Das ist die bisher vollstaendigste Ausschluss-Pruefung dieser Sitzung: praktisch der gesamte synchron sichtbare Code-Pfad ist jetzt einzeln gelesen/durchsucht.

**Konsequenz:** Da (a) kein zweiter aktiver Thread zum Absturzzeitpunkt existiert, (b) die komplette sichtbare Aufrufkette sauber ist, UND (c) Allocator/Vtable-Familie/Destruktoren bereits ausgeschlossen sind (Updates 190/194/195), bleiben nur noch zwei plausible Erklaerungsklassen: entweder ein **kurzlebiger, laengst wieder beendeter Thread**, der irgendwann waehrend des Laufs existierte und dabei (fehlerhaft) in unser Objekt schrieb, bevor er terminierte -- oder ein **nicht-Thread-basierter asynchroner Mechanismus** (z.B. eine Exception/APC/OS-Callback), der ausserhalb des normalen synchronen Aufrufpfads laeuft.

**Naechster Test:** (a) Ein einfacher, NICHT verschachtelter Breakpoint (im Gegensatz zu den fehlgeschlagenen `.if`/`.else`-Konstrukten) auf `bridge_PsCreateSystemThreadEx` mit einer trivialen Aktion (`.printf`; `g`, keine Verschachtelung) -- dieses Muster hat in frueheren Updates (180-182) nachweislich zuverlaessig funktioniert -- um zu zaehlen, ob waehrend eines vollstaendigen Laufs jemals mehr als der eine bekannte `nv2a_ack_thread` erzeugt wird. (b) Alternativ: ein frischer Prozess, Timing-Eingrenzung wie in Update 193, dann ein `~*kb`-Dump gezielt IM Uebergangsfenster statt danach.

**Model-Tier bleibt MAXIMUM.**
"""

worklog_text = """
## Update 197 (2026-09-20): Live all-threads dump shows only 2 threads (no lingering worker); the entire visible main()->fatal-callback call chain checked for zero-fill -- clean throughout

**Test:** a single, non-invasive `cdb -pv -p <PID> -c "~*kb; q"` against the already-crashed process (PID 46460, still alive from Updates 195/196) -- no breakpoints, no watchpoints, minimally invasive as established in Update 193.

**Result 1 -- only two threads total:** thread 0 (main) sits in the known `sub_0003FCA5` `jmp $` loop; thread 1 is `nv2a_ack_thread`, unchanged in its `SleepEx` wait cycle (identical to Update 180's finding). **Not a single worker/sound thread exists at this point.** This weakens (without fully ruling out -- a short-lived thread could already have exited) Update 188's mention of "several worker/sound threads running concurrently" for THIS run: if more than two threads ever existed here, they are already gone by the time of the crash.

**Result 2 -- the complete call chain from `main()` to the fatal callback is visible and matches the historical Update 179 chain exactly:** `main` -> `xbe_entry_point` -> ... -> `sub_00048B15` -> `sub_00048F1D` -> `sub_0004934E` -> `sub_00035DEE` -> `sub_0004BE42` -> `sub_001CA0A5` -> `sub_00065015` -> `sub_00064FF3` -> `sub_00064DEF` -> `sub_00064B00` -> `sub_0007F412` -> `sub_0007DE25` -> `sub_00036582` -> `sub_0007DC43` -> `sub_00045738` -> `sub_0004438C` -> `sub_00040821` -> [fatal callback].

**Every single function in this chain was searched (this round) for `memset`/`rep stosd`/`stosb`/`stosw` translation patterns** (the earlier Updates 177/178 checked the same functions only for "stop counter" logic, NOT explicitly for zero-fills):
- `sub_00048F1D`, `sub_0004934E` (3797 lines!), `sub_00035DEE`, `sub_0004BE42`, `sub_001CA0A5`, `sub_00065015`, `sub_00064FF3`, `sub_00064DEF`, `sub_0007DE25`, `sub_00036582`, `sub_0007DC43`, `sub_00045738`, `sub_0004438C`: **no hit.**
- `sub_00064B00`: one `stosd`+`stosw` hit, but the target is `ebp + -518` -- a LOCAL STACK buffer (size `0x7F`, matching Update 178's "sprintf-like, 0x7F/0xFF buffer sizes"), no heap/object relevance.
- `sub_0007F412`: one `stosd` hit, but the fill value is `0xFFFFFFFF` (NOT zero!) on a DIFFERENT, independent object (`esi+0xF0`, the same "completely unrelated global array" target already identified in Update 178) -- matches neither the size, the fill value, nor the target address of our bug.
- `sub_00040821`, `sub_000516B7`, `xbox_NtReadFile`, `sub_00052A85`/`sub_00052A57`: already individually checked in Updates 195/196, clean.

**Result: the ENTIRE, actually-executed-at-runtime call chain from `main()` to the fatal callback contains no zero-fill anywhere that could apply to our object.** This is the most complete exclusion check of this session so far: essentially the whole synchronously visible code path has now been individually read/searched.

**Consequence:** since (a) no second active thread exists at the moment of the crash, (b) the entire visible call chain is clean, and (c) the allocator/vtable family/destructors are already ruled out (Updates 190/194/195), only two plausible explanation classes remain: either a **short-lived thread, long since exited**, that existed at some point during the run and (erroneously) wrote to our object before terminating -- or a **non-thread-based asynchronous mechanism** (e.g. an exception/APC/OS callback) running outside the normal synchronous call path.

**Next test:** (a) a simple, NON-nested breakpoint (as opposed to the failed `.if`/`.else` constructs) on `bridge_PsCreateSystemThreadEx` with a trivial action (`.printf`; `g`, no nesting) -- this pattern has reliably worked in earlier updates (180-182) -- to count whether more than the one known `nv2a_ack_thread` is ever created during a full run. (b) Alternatively: a fresh process, timing narrowed as in Update 193, then a `~*kb` dump targeted to land INSIDE the transition window instead of after it.

**Model tier stays MAXIMUM.**
"""

with open(status_path, "ab") as f:
    f.write(status_text.replace("\n", "\r\n").encode("utf-8"))

with open(worklog_path, "ab") as f:
    f.write(worklog_text.replace("\n", "\r\n").encode("utf-8"))

print("done")
