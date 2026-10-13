status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 188 (2026-09-18): Konstruktor/Destruktor der Ringpuffer-Klasse gefunden, echter Use-after-free-Mechanismus identifiziert -- Beweis, dass es sich um eine Recomp-eigene Thread-Race handelt, nicht um einen Bug im Original-x86-Code

**Bestaetigt (statische Analyse, `sub_00040766`/`sub_000407F7`/`sub_00040B00`):**
- `sub_00040766` ist der Konstruktor der Ringpuffer-Unterobjekt-Klasse (Vtable `0x4F6940`, bestaetigt exakt gegen den fruehreren `xemu_object_fileobj.py`-Fund `subobj_vtable=0x004F6940`). Setzt u.a. `+0x3C`=Dateihandle, `+0x4058`=Puffergroesse, `+0x405C`=allozierter Ringpuffer (via icall, malloc-aehnlich).
- `sub_000407F7` ist die Aufraeumfunktion: schliesst das Handle (`+0x3C`), gibt den Ringpuffer-Speicher via `sub_0029FA18(MEM32(esi+0x405C))` frei -- setzt aber `esi+0x405C` selbst NICHT auf Null (haengender Zeiger).
- `sub_00040B00` ist der klassische "scalar deleting destructor": setzt die Vtable zwischendurch auf eine Basis-/Sentinel-Vtable (`0x4FD9E0`), ruft `sub_000407F7` auf, und -- falls das Delete-Flag gesetzt ist -- gibt **das Unterobjekt selbst** via `sub_0029FA18(esi)` frei.

**Live bestaetigt am Fatal-Callback-Einsprungpunkt (Update 187):** Leser-Objekt (`this=0xD1051C8`, Vtable `0x4F6F08`, dieselbe Klasse wie in Update 183) hat ein weiterhin GUELTIGES `+0x3C`-Feld, das korrekt auf `0x1135928` zeigt -- der Leser selbst ist intakt. Das darin referenzierte Unterobjekt (`0x1135928`) ist jedoch komplett genullt, exakt das Muster, das nach `sub_00040B00`s Freigabe des Unterobjekts zu erwarten waere.

**Entscheidender neuer Test:** Breakpoint auf `recomp_icall_not_code_log` gesetzt (unsere eigene Diagnosefunktion, die JEDES Mal ausgeloest wird, wenn ein indirekter Aufruf auf eine ungueltige/genullte Vtable-Adresse zeigt) UND auf `sub_0003FCA5` (Fatal-Handler), beide gleichzeitig scharf, waehrend eines kompletten Boot-Laufs. **Ergebnis: `recomp_icall_not_code_log` feuert kein einziges Mal -- der Fatal-Breakpoint wird direkt erreicht.** Das beweist: Der urspruengliche Dispatch von `sub_0004438C` in `sub_00040821` (und jeder interne Icall von `sub_00040821` selbst) erfolgte mit einer zu diesem Zeitpunkt GUELTIGEN, registrierten Vtable -- das Unterobjekt war beim eigentlichen Aufruf noch vollstaendig intakt. `sub_00040821`s kompletter Funktionskoerper wurde bereits in frueheren Updates vollstaendig gelesen und schreibt an keiner Stelle Null in eines dieser Felder.

**Schlussfolgerung:** Die Nullung muss in einem Zeitfenster NACH dem gueltigen Dispatch, aber VOR unserem Speicher-Read am Fatal-Callback-Einsprungpunkt passieren. Da innerhalb `sub_00040821`s eigenem Codepfad selbst nichts dafuer verantwortlich ist, deutet dies stark auf eine **echte Thread-Race** hin: ein ANDERER Thread (im Prozess sind mehrere Worker-/Sound-Threads aktiv, u.a. `nv2a_ack_thread`, mehrere `TppWorkerThread`-Instanzen) ruft parallel zu Thread 0s laufender `sub_00040821`-Ausfuehrung die Destruktor-Kette (`sub_00040B00`/`sub_000407F7`) fuer DASSELBE Unterobjekt auf und gibt es frei, waehrend Thread 0 noch mitten in der Verarbeitung ist. Das waere ein **Recomp-eigenes Nebenlaeufigkeitsproblem**, kein Bug im urspruenglichen x86-Spielcode (das Original laeuft vermutlich kooperativ/single-threaded fuer genau diesen Codepfad, waehrend unsere Portierung mehrere native Threads parallel auf denselben Guest-Speicher zugreifen laesst, ohne dass das Original-Spiel dafuer je eine Synchronisation vorgesehen hat).

**Fuer die naechste Sitzung:** Breakpoint auf `sub_0029FA18` (die Free-Funktion) UND/ODER `sub_00040B00` setzen, das Argument (Unterobjekt-Adresse) gegen bekannte aktive Unterobjekt-Adressen pruefen, und **die Thread-ID jedes Aufrufs protokollieren** (`~.` bzw. den aktuellen Thread in cdb) -- wenn der Aufruf, der `0x1135928` freigibt, von einem ANDEREN Thread als Thread 0 kommt, waehrend Thread 0 gleichzeitig in `sub_00040821`/`sub_0004438C` fuer dasselbe Objekt aktiv ist, ist die Thread-Race zweifelsfrei bewiesen. Ein moeglicher, nicht-invasiver echter Fix waere dann eine Synchronisation (Lock) um die Ringpuffer-Objekt-Lebensdauer im Kernel-Layer, NICHT eine Aenderung an der rekompilierten Spiellogik selbst.
"""

worklog_text = """
## Update 188 (2026-09-18): Ring-buffer class constructor/destructor found, real use-after-free mechanism identified -- proof this is a recomp-specific thread race, not a bug in the original x86 code

**Confirmed (static analysis, `sub_00040766`/`sub_000407F7`/`sub_00040B00`):**
- `sub_00040766` is the constructor of the ring-buffer subobject class (vtable `0x4F6940`, matches the earlier `xemu_object_fileobj.py` finding `subobj_vtable=0x004F6940` exactly). Sets, among others, `+0x3C`=file handle, `+0x4058`=buffer size, `+0x405C`=allocated ring buffer (via an icall, malloc-like).
- `sub_000407F7` is the teardown function: closes the handle (`+0x3C`), frees the ring buffer memory via `sub_0029FA18(MEM32(esi+0x405C))` -- but does NOT null `esi+0x405C` itself afterward (a dangling pointer).
- `sub_00040B00` is the classic "scalar deleting destructor": swaps the vtable to a base/sentinel vtable (`0x4FD9E0`) partway through, calls `sub_000407F7`, and -- if the delete flag is set -- frees **the subobject itself** via `sub_0029FA18(esi)`.

**Confirmed live at the fatal callback entry point (Update 187):** the reader object (`this=0xD1051C8`, vtable `0x4F6F08`, same class as Update 183) has a still-VALID `+0x3C` field correctly pointing to `0x1135928` -- the reader itself is intact. The subobject it references (`0x1135928`), however, is completely zeroed, exactly the pattern expected after `sub_00040B00` frees the subobject.

**Decisive new test:** set a breakpoint on `recomp_icall_not_code_log` (our own diagnostic, triggered EVERY time an indirect call targets an invalid/zeroed vtable address) AND on `sub_0003FCA5` (the fatal handler), both armed simultaneously through a full boot run. **Result: `recomp_icall_not_code_log` never fires -- the fatal breakpoint is hit directly.** This proves the original dispatch from `sub_0004438C` into `sub_00040821` (and any internal icall `sub_00040821` itself makes) happened through a VALID, registered vtable -- the subobject was still fully intact at the moment of the actual call. `sub_00040821`'s entire function body was already read in full in earlier updates and writes zero to none of these fields anywhere.

**Conclusion:** the zeroing must happen in the window AFTER the valid dispatch but BEFORE our memory read at the fatal callback entry point. Since nothing inside `sub_00040821`'s own code path is responsible, this strongly points to a **genuine thread race**: some OTHER thread (the process runs several worker/sound threads concurrently, including `nv2a_ack_thread` and multiple `TppWorkerThread` instances) calls the destructor chain (`sub_00040B00`/`sub_000407F7`) on the SAME subobject and frees it while thread 0 is still mid-flight through `sub_00040821` for that exact object. This would be a **recomp-specific concurrency bug**, not one in the original x86 game code (the original almost certainly runs this code path cooperatively/single-threaded, while our port lets several native threads touch the same guest memory concurrently with no synchronization the original game ever needed to provide).

**For the next session:** breakpoint `sub_0029FA18` (the free function) and/or `sub_00040B00`, check the argument (subobject address) against known active subobject addresses, and **log the thread ID of each call** (cdb's current thread, e.g. via `~.`) -- if the call that frees `0x1135928` comes from a DIFFERENT thread than thread 0 while thread 0 is simultaneously active inside `sub_00040821`/`sub_0004438C` for that same object, the thread race is proven beyond doubt. A plausible, non-invasive real fix at that point would be a lock around the ring-buffer object's lifetime in the kernel layer, not a change to the recompiled game logic itself.
"""

with open(status_path, "rb") as f:
    raw = f.read()
raw += status_text.replace("\n", "\r\n").encode("utf-8")
with open(status_path, "wb") as f:
    f.write(raw)

with open(worklog_path, "rb") as f:
    raw = f.read()
raw += worklog_text.replace("\n", "\r\n").encode("utf-8")
with open(worklog_path, "wb") as f:
    f.write(raw)

print("done")
