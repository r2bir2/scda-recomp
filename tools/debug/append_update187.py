status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 187 (2026-09-18): `ebx` live am Fatal-Callback-Einsprungpunkt mitgeschnitten -- Objekt ist komplett genullt (nie konstruiert), drei "fehlende Datei"-Hypothesen widerlegt, echte Ursache ist unabhaengig von jeder konkreten Datei

**Methodik-Fix:** Frischer `cdb`-Start mit `-g -G` und Standard-Symbolpfad (`srv*`) blieb wiederholt direkt nach dem Modul-Laden haengen (fuenf Versuche, nie ein Timeout-Problem sondern ein echter Hang der cdb-Konsole selbst, vermutlich Netzwerk-Symbolabgleich fuer System-DLLs). Umgangen durch: zuerst normal (ohne Debugger) starten, danach per `cdb -p <PID>` **invasiv anhaengen** und `.sympath` auf das lokale Build-Verzeichnis setzen (kein `srv*`) -- damit lief jeder Versuch sofort durch.

**Test:** Breakpoint auf den EINSPRUNG von `sub_0003FCA5` selbst (dem Fatal-Handler), nicht auf die aufrufende Stelle -- damit werden die Register genau in dem Moment gelesen, in dem der Callback aufgerufen wird, BEVOR der Handler selbst irgendetwas ueberschreibt. Bestaetigt wichtig: ein fruehrerer Versuch, dieselben Register per nachtraeglichem, nicht-invasivem Attach (`cdb -pv`) NACHDEM der Prozess schon in der `jmp $`-Schleife haengt, zu lesen, lieferte `g_ebx=0` -- aber das ist unbrauchbar, da der Handler selbst zwischen dem Callback-Einsprung und dem Erreichen der `jmp $`-Schleife bereits x86-Code ausfuehrt, der `ebx` fuer eigene Zwecke ueberschreibt.

**Ergebnis (sauber, am echten Einsprungpunkt, per typisiertem `??`-Evaluator um den dokumentierten `poi()`-Bug zu vermeiden):** `g_eax=0x3FCA5` (die Callback-Adresse selbst, bestaetigt den Icall-Mechanismus), `g_ebx=0x1135928` ("this" des Ringpuffer-Objekts). ALLE gelesenen Felder dieses Objekts sind exakt Null: Offset `+0x00`, `+0x3C` (file_obj), `+0x44`, `+0x4058`, `+0x405C` (Ringpuffer-Basiszeiger!), `+0x4060`, `+0x4064`. Ein Objekt mit einem Null-Basiszeiger fuer seinen eigenen Ringpuffer ist niemals ueber eine erfolgreiche "open"-Operation initialisiert worden -- es ist ein frisch angelegtes/genulltes Objekt, das nie mit einer echten Datei verknuepft wurde.

**Drei nacheinander getestete "fehlende Datei"-Hypothesen, alle widerlegt:**
1. `Default\\Core.int` / `TDATA\\5553005e\\Core.int` fehlten (`err=2`) -- Platzhalter-Dateien angelegt, Datei oeffnet danach erfolgreich, Boot kommt spuerbar weiter (laedt zusaetzliche Screens), aber derselbe Absturz tritt trotzdem auf.
2. `Default\\bfc7a3c3` / `TDATA\\5553005e\\bfc7a3c3` (derselbe First-Run-Datei-Namensmuster wie Core.int) fehlten -- Platzhalter angelegt, oeffnet danach erfolgreich, Absturz bleibt identisch.
3. `Default\\DataXb\\VideosPal\\Menu_loading.bik` fehlte komplett (kein Bink-Video mit diesem Namen existiert irgendwo im extrahierten Disc-Abbild) -- Platzhalter angelegt, Datei wird danach erfolgreich getrackt (`disc-backed handle tracked: D:\\Default\\DataXb\\VideosPal\\Menu_loading.bik`), **aber der Absturz tritt weiterhin exakt gleich auf, UND diesmal ganz ohne jeden fehlgeschlagenen `NtCreateFile`-Versuch dazwischen** (zwischen dem erfolgreichen Menu_loading.bik-Tracking und dem Short-Read/Absturz liegt kein einziger neuer Dateizugriff).

**Schlussfolgerung:** Der Absturz ist nicht an eine konkrete fehlende Datei gekoppelt. Das genullte Objekt am Fatal-Callback-Einsprungpunkt ist eine generische, nie initialisierte lokale Struktur, die unabhaengig vom Zustand jeder einzelnen Ressourcen-Datei in die Ringpuffer-Refill-Maschinerie (`sub_00040821`) hineingereicht wird -- vermutlich ein echter Uninitialized-Use-Bug in der Aufrufkette oberhalb von `sub_00040821` (`sub_0004438C` -> `sub_00045738` -> `sub_0007DC43` -> `sub_00036582` -> `sub_0007DE25` -> ...), nicht in `sub_00040821`/`sub_000516B7` selbst und nicht im scda_recomp-eigenen Datei-I/O.

**Fuer die naechste Sitzung:** Breakpoint auf `sub_00040821`s Einsprung selbst setzen (nicht auf `sub_0003FCA5`), `ecx`("this") bei JEDEM Aufruf pruefen, und den ERSTEN Aufruf mit einem genullten `this` isolieren (per Vergleich mit dem vorherigen validen Aufruf) -- das identifiziert den genauen Aufrufer/Codepfad, der das nie initialisierte Objekt uebergibt, statt weiter am Dateisystem zu raten. Alle drei Test-Platzhalterdateien (`Core.int`, `bfc7a3c3`, `Menu_loading.bik`) sind reine lokale Testartefakte unter `build/scda_upgrade/bin/RelWithDebInfo/`, nicht Teil des Repos -- sie schaden nicht, loesen das eigentliche Problem aber auch nicht.
"""

worklog_text = """
## Update 187 (2026-09-18): Live-captured `ebx` at the fatal callback entry point -- object is completely zeroed (never constructed), three "missing file" hypotheses disproven, the real cause is independent of any specific file

**Methodology fix:** a fresh `cdb -g -G` launch with the default symbol path (`srv*`) repeatedly hung right after module load (five attempts, never a timeout issue but a genuine hang of the cdb console itself, likely network symbol resolution for system DLLs). Worked around by: launching normally (no debugger) first, then **invasively attaching** via `cdb -p <PID>` and setting `.sympath` to the local build directory (no `srv*`) -- every attempt then ran through immediately.

**Test:** breakpoint on the ENTRY of `sub_0003FCA5` itself (the fatal handler), not on its caller -- this reads registers at the exact moment the callback is invoked, BEFORE the handler's own body overwrites anything. Important confirmation: an earlier attempt to read the same registers via post-hoc, non-invasive attach (`cdb -pv`) AFTER the process was already sitting in the `jmp $` loop returned `g_ebx=0` -- but that is useless, since the handler itself runs x86 code between the callback entry and reaching the `jmp $` loop that overwrites `ebx` for its own purposes.

**Result (clean, at the true entry point, via the typed `??` evaluator to avoid the documented `poi()` bug):** `g_eax=0x3FCA5` (the callback address itself, confirming the icall mechanism), `g_ebx=0x1135928` (the ring-buffer object's "this"). EVERY field read from this object is exactly zero: offset `+0x00`, `+0x3C` (file_obj), `+0x44`, `+0x4058`, `+0x405C` (the ring buffer's own base pointer!), `+0x4060`, `+0x4064`. An object with a null base pointer for its own ring buffer has never been initialized through a successful "open" -- it is a freshly allocated/zeroed object that was never associated with a real file.

**Three "missing file" hypotheses tested in sequence, all disproven:**
1. `Default\\Core.int` / `TDATA\\5553005e\\Core.int` were missing (`err=2`) -- placeholder files created, the file opens successfully afterward, boot progresses noticeably further (loads additional screens), but the exact same crash still occurs.
2. `Default\\bfc7a3c3` / `TDATA\\5553005e\\bfc7a3c3` (same first-run filename pattern as Core.int) were missing -- placeholder created, opens successfully afterward, the crash is unchanged.
3. `Default\\DataXb\\VideosPal\\Menu_loading.bik` was entirely missing (no Bink video with this name exists anywhere in the extracted disc dump) -- placeholder created, the file is then tracked successfully (`disc-backed handle tracked: D:\\Default\\DataXb\\VideosPal\\Menu_loading.bik`), **but the crash still occurs identically, and this time with zero failed `NtCreateFile` attempts in between** (no new file access at all happens between the successful Menu_loading.bik tracking and the short-read/crash).

**Conclusion:** the crash is not tied to any specific missing file. The zeroed object at the fatal callback entry point is a generic, never-initialized local structure that gets handed into the ring-buffer refill machinery (`sub_00040821`) regardless of any individual resource file's state -- most likely a genuine uninitialized-use bug somewhere in the call chain above `sub_00040821` (`sub_0004438C` -> `sub_00045738` -> `sub_0007DC43` -> `sub_00036582` -> `sub_0007DE25` -> ...), not in `sub_00040821`/`sub_000516B7` themselves and not in scda_recomp's own file I/O.

**For the next session:** put the breakpoint on `sub_00040821`'s own entry (not `sub_0003FCA5`), check `ecx` ("this") on EVERY call, and isolate the FIRST call with a zeroed `this` (by comparing against the previous valid call) -- this identifies the exact caller/code path that hands in the never-initialized object, instead of continuing to guess at the filesystem. All three test placeholder files (`Core.int`, `bfc7a3c3`, `Menu_loading.bik`) are local test artifacts only, under `build/scda_upgrade/bin/RelWithDebInfo/`, not part of the repository -- they do no harm but also do not fix the actual problem.
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
