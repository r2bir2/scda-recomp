status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 186 (2026-09-18): Fix implementiert (ehrlicher Byte-Count statt Auffuellen), gebaut, getestet -- Boot kommt weiter, aber ein ZWEITER, unabhaengiger Root Cause blockiert weiterhin denselben Fatal-Callback

**Fix:** `src/kernel/kernel_file.c`: `scda_disc_fill_overrun` (fuellte kurze Reads kuenstlich auf volle Groesse auf) ersetzt durch `scda_disc_zero_short_read_tail` (nullt nur noch defensiv den nicht geschriebenen Puffer-Rest, meldet aber den ECHTEN Transfer-Count via `IoStatusBlock->Information`). Sauber gebaut, isoliert getestet: Log zeigt exakt `zeroed 41103 ... reporting the true 89969-byte transfer` -- deckt sich exakt mit dem xemu-Befund aus Update 185.

**Erster Testlauf nach dem Fix -- Fatal-Screen tritt weiterhin auf, aber ueber einen ANDEREN Pfad:** Live-Stackdump (nicht-invasives `cdb -pv`) zeigt den Haupt-Thread exakt in der `sub_0003FCA5`-`jmp $`-Schleife, erreicht ueber `sub_00040821+0x10d9` -> `sub_0004438C` -> ... -> `sub_00048B15` -- denselben Aufrufpfad wie zuvor, aber diesmal NICHT weil unser Short-Read-Fix versagt: unmittelbar nach der (korrekten) Short-Read-Meldung folgt ein `NtReadFile(handle=0x0, len=2048)` mit `err=6` (ERROR_INVALID_HANDLE), was `sub_00040821`s eigene Fehlerbehandlung dazu bringt, den global geladenen Fatal-Callback (`MEM32(0x5E6E0C)`) aufzurufen (bestaetigt durch vollstaendiges Nachvollziehen der Disassemblierung von `sub_00040821`/`sub_000516B7`: nur ein Rueckgabewert von `sub_000516B7`, der weder Erfolg noch `STATUS_END_OF_FILE` ist, loest den Callback aus).

**Zwischenzeitlich entdeckter, echter und unabhaengiger zweiter Bug (jetzt behoben):** wiederholte `NtCreateFile FAILED`-Versuche fuer `Default\\Core.int` / `TDATA\\5553005e\\Core.int` (err=2, Datei fehlt) direkt vor dem obigen Fehler. Test mit leeren Platzhalter-Dateien an beiden Pfaden bestaetigt: Boot kommt danach spuerbar weiter (laedt zusaetzlich `esrb.tga`, `SplinterCellLogo.tga`, mehrere weitere Screens/Sounds), bevor exakt derselbe `sub_0004438C`/lipsync-Short-Read/Null-Handle-Ablauf erneut zuschlaegt. Die fehlenden `Core.int`-Dateien waren also ein echter, eigenstaendiger Boot-Blocker (vermutlich First-Run-Profildaten, die auf echter Hardware vom Dashboard automatisch angelegt wuerden), aber NICHT die Ursache des Fatal-Callbacks.

**Versuchte, aber widerlegte Hypothese:** Vermutung, dass das eigene defensive Nullen des unbeschriebenen Puffer-Rests (in `scda_disc_zero_short_read_tail`) selbst die Divergenz waere (da der Ringpuffer wiederverwendeter Speicher ist und "stale" Altdaten dort auf echter Hardware liegen wuerden statt Nullen). Live getestet durch Entfernen des `memset`: Das Ergebnis aendert sich nur kosmetisch -- statt `handle=0x0` liest der naechste Refill-Versuch jetzt `handle=0xE1F9F6F3` (offensichtlich Datenmuell) und scheitert genauso mit `err=6`. Die Hypothese ist damit widerlegt; das Nullen wurde zurueckgesetzt (sicherer: deterministisch, keine Altspeicher-Preisgabe), da es fuer den eigentlichen Blocker ohnehin irrelevant ist.

**Tatsaechlicher, noch offener Kern des zweiten Bugs:** Der `NtReadFile`-Aufruf unmittelbar nach dem Short-Read liest sein Handle-Argument offenbar NICHT aus dem persistenten Objektfeld (`file_obj`, bestaetigt `0x18` fuer dieses Objekt), sondern aus instabilem/falsch adressiertem Speicher (Datenbereich statt Metadatenfeld) -- unabhaengig davon, ob dieser Speicher Nullen oder Altdaten enthaelt, ist der gelesene Wert kein gueltiges Handle. Das ist ein per-Instanz-Adressierungsfehler, vermutlich in `sub_00040821`s "leftover < 8 bytes"-Zweig (Refill fuer den naechsten Record-Header direkt nach Erreichen des echten Stream-Endes), noch nicht auf Zeilenebene isoliert.

**Fuer die naechste Sitzung:** Live per `ebx`(=`this` des Ringpuffer-Objekts) GENAU an der fehlschlagenden `NtReadFile`-Stelle mitschneiden (per Breakpoint auf `bridge_NtReadFile`, nicht per nachtraeglichem Attach -- letzteres liefert nur den durch den Fatal-Handler selbst ueberschriebenen `ebx`-Wert, wie live bestaetigt: `g_ebx=0` nach Erreichen der `jmp $`-Schleife ist unbrauchbar). Ziel: den exakten Objekt-Offset finden, aus dem das kaputte Handle gelesen wird, und pruefen, ob real Hardware an dieser Stelle ueberhaupt denselben Codepfad (zweiter interner Refill-Call in derselben `sub_00040821`-Invocation) durchlaeuft.

Alle Aenderungen dieser Runde: `scda_disc_zero_short_read_tail`-Umbenennung/-Fix bleibt aktiv (korrekt und durch xemu bestaetigt); Core.int-Platzhalter sind reine lokale Testdaten in `build/scda_upgrade/bin/RelWithDebInfo/`, nicht Teil des Repos.
"""

worklog_text = """
## Update 186 (2026-09-18): Fix implemented (honest byte count instead of topping up), built, tested -- boot progresses further, but a SECOND, independent root cause still blocks the same fatal callback

**Fix:** `src/kernel/kernel_file.c`: replaced `scda_disc_fill_overrun` (artificially topped up short reads to the full requested size) with `scda_disc_zero_short_read_tail` (still defensively zeroes the unwritten buffer tail, but now reports the TRUE transfer count via `IoStatusBlock->Information`). Built clean, tested in isolation: log shows exactly `zeroed 41103 ... reporting the true 89969-byte transfer`, matching the xemu finding from Update 185 exactly.

**First test run after the fix -- the fatal screen still fires, but via a DIFFERENT path:** a live stack dump (non-invasive `cdb -pv`) shows the main thread sitting exactly in the `sub_0003FCA5` `jmp $` loop, reached via `sub_00040821+0x10d9` -> `sub_0004438C` -> ... -> `sub_00048B15` -- the same call chain as before, but this time NOT because our short-read fix failed: immediately after the (correct) short-read log line, an `NtReadFile(handle=0x0, len=2048)` fails with `err=6` (ERROR_INVALID_HANDLE), which drives `sub_00040821`'s own error handling to invoke the globally-armed fatal callback (`MEM32(0x5E6E0C)`) -- confirmed by fully working through the `sub_00040821`/`sub_000516B7` disassembly: only a `sub_000516B7` return value that is neither success nor `STATUS_END_OF_FILE` triggers the callback.

**A real, independent second bug found (and now fixed) along the way:** repeated `NtCreateFile FAILED` attempts for `Default\\Core.int` / `TDATA\\5553005e\\Core.int` (err=2, file missing) immediately before the failure above. Testing with empty placeholder files at both paths confirms boot progresses noticeably further afterward (also loads `esrb.tga`, `SplinterCellLogo.tga`, several more screens/sounds) before the exact same `sub_0004438C`/lipsync-short-read/null-handle sequence fires again. The missing `Core.int` files were therefore a real, independent boot blocker (likely first-run profile data a real dashboard would create automatically) but NOT the cause of the fatal callback.

**A tried and disproven hypothesis:** suspected our own defensive zeroing of the unwritten buffer tail (in `scda_disc_zero_short_read_tail`) was itself the divergence, since the ring buffer is reused memory and real hardware would leave stale prior-cycle data there instead of zeros. Live-tested by removing the `memset`: the result changes only cosmetically -- instead of `handle=0x0`, the next refill attempt now reads `handle=0xE1F9F6F3` (evidently leftover data-plane garbage) and fails identically with `err=6`. The hypothesis is disproven; the zeroing was reverted (safer: deterministic, no leftover-memory exposure) since it is irrelevant to the actual blocker either way.

**The actual, still-open core of the second bug:** the `NtReadFile` call immediately after the short read apparently does NOT read its handle argument from the object's persistent field (`file_obj`, confirmed `0x18` for this object), but from unstable/incorrectly-addressed memory (data-plane content rather than a metadata field) -- regardless of whether that memory holds zeros or stale data, the value read is not a valid handle. This is a per-instance addressing bug, most likely in `sub_00040821`'s "leftover < 8 bytes" branch (refilling for the next record header right after genuinely reaching the end of the stream), not yet isolated to a specific line.

**For the next session:** capture `ebx` (= the ring-buffer object's `this`) live, exactly at the failing `NtReadFile` call (via a breakpoint on `bridge_NtReadFile`, not via post-hoc attach -- the latter only yields the value the fatal handler itself already overwrote, confirmed live: `g_ebx=0` after reaching the `jmp $` loop is useless). Goal: find the exact object offset the broken handle is read from, and check whether real hardware even takes the same code path (a second internal refill call within the same `sub_00040821` invocation) at this point at all.

All changes this round: the `scda_disc_zero_short_read_tail` rename/fix stays active (correct, confirmed against xemu); the Core.int placeholders are local test data only, under `build/scda_upgrade/bin/RelWithDebInfo/`, not part of the repository.
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
