status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 203 (2026-09-20): Sync mit parallelem Agenten (Updates 198-202 uebernommen); Nebenfund fehlender Funktions-Seed 0x3E7B6 bewertet und zurueckgestellt; empfohlener Test erneut gestartet

**Sync-Hinweis:** Bei Uebernahme dieser Runde stand HEAD bereits bei Update 197
(eigener Commit), aber der Arbeitsbaum enthielt umfangreiche unkommittete
Aenderungen eines PARALLEL laufenden Agenten (Updates 198-202, siehe Commit
`18e9f7b`). Diese wurden geprueft, als kohaerent befunden und als
Checkpoint committet, bevor hier weitergearbeitet wurde -- keine eigene
Wiederholung der darin bereits behandelten Worker-Thread-/Bink-/Lipsync-
Untersuchung.

**Eigener Nebenfund (unabhaengig von Update 198-202 entdeckt, waehrend
derselbe Worker-Thread-Ansatz parallel verfolgt wurde):** Der Laufzeit-
Log zeigt einen ICALL-Fehlschlag-Zaehler (`recomp_icall_fail_log`,
`projects/scda/src/recomp_manual.c`), der Ziel `0x0003E7B6` ueber
**436.162+ Aufrufe** hinweg als "nicht aufloesbar" meldet. `0x3E7B6` ist
EIN der in Update 194 gelisteten (aber dort NIE einzeln verifizierten)
geteilten Vtable-Slot-Adressen unserer Klassenfamilie. Per Capstone-
Disassemblierung der echten XBE-Bytes bestaetigt: `0x3E7B6` ist ein
trivialer 3-Byte-Getter (`mov eax,[ecx+0x48]; ret`), der wegen des
fehlenden Function-Seeds nie tatsaechlich ausgefuehrt wird -- `RECOMP_ICALL_SAFE`
liefert bei jedem Aufruf `eax=0` statt des echten Feldwerts zurueck
(`tools/recomp/output/scda_v060/gen/recomp_types.h:990-1013`). Ein
Aufrufer wurde identifiziert (`sub_0007DC2D`, ein generischer Multiple-
Inheritance-Thunk: `ecx=[ecx+0x450]; icall vtable[0x34]`) -- dieses Muster
ist Standard-C++-ABI-Plumbing, das quer durch VIELE unabhaengige
Klassenfamilien im ganzen Spiel verwendet wird, nicht spezifisch fuer
unser Ringpuffer-Objekt. **Bewertung:** ein echter, gut belegter
(statisch UND zur Laufzeit) Korrektheits-Bug, aber als DIREKTE Ursache
fuer die `0x8070`-Byte-Nullung unwahrscheinlich, da er nur einen falschen
GELESENEN Wert (0 statt Feldinhalt) liefert, niemals selbst schreibt.
**Zurueckgestellt statt gefixt:** Ein vollstaendiger Regenerate+Rebuild
kostet laut Update-202-Handoff-Text historisch ueber eine Stunde ohne
garantiertes Signal; angesichts der niedrigen Objektspezifitaet dieses
Fundes ist das aktuell nicht die kosteneffizienteste naechste Aktion.
Als moeglicher separater Correctness-Fix fuer eine spaetere Runde notiert,
NICHT als aktiver Kandidat fuer den `0x8070`-Bug behandelt.

**Empfohlenen Test erneut gestartet:** Die zwei von Update 202
hinterlassenen `cdb_lipsync_first_icall_capture`-Prozesse (PIDs
`3660`/`15280` und `21136`/`29212`) liefen bereits verwaist (kein
erreichbares Log, da ohne `-logo` gestartet) -- unangetastet gelassen.
Eine dritte, sauber geloggte Kopie desselben, bereits vorbereiteten
Skripts wurde gestartet (`tools/debug/cdb_lipsync_first_icall_capture3_out.log`,
PID `21308`/`13260`). Zwischenstand: Objekt ist im Zielprozess bereits
genullt (Fatal laengst erreicht auf Thread 0), der Lipsync-Breakpoint hat
aber noch nicht gefeuert -- das ist konsistent mit Update 199s Befund,
dass der Lipsync-Worker den Fatal-Zeitpunkt des Hauptthreads ueberlebt und
unabhaengig weiterlaeuft; Ergebnis folgt in einer separaten Ergaenzung.

**Model-Tier bleibt MAXIMUM** (unveraendert, Begruendung siehe Update 202).
"""

worklog_text = """
## Update 203 (2026-09-20): Synced with a parallel agent session (adopted Updates 198-202); own side-finding of a missing function seed (0x3E7B6) assessed and deprioritized; re-launched the recommended test

**Sync note:** on picking up this round, HEAD was already at Update 197 (this
session's own commit), but the working tree contained substantial
uncommitted changes from a PARALLEL agent session (Updates 198-202, see
commit `18e9f7b`). These were reviewed, found coherent, and committed as a
checkpoint before continuing -- no independent repetition of the
worker-thread/Bink/Lipsync investigation already covered there.

**Own side-finding (discovered independently while pursuing the same
worker-thread angle in parallel):** the runtime log shows an icall-failure
counter (`recomp_icall_fail_log`, `projects/scda/src/recomp_manual.c`)
reporting target `0x0003E7B6` as "unresolved" across **436,162+ calls**.
`0x3E7B6` is one of the shared vtable-slot addresses listed in Update 194
(but never individually verified there). Capstone disassembly of the real
XBE bytes confirms: `0x3E7B6` is a trivial 3-byte getter
(`mov eax,[ecx+0x48]; ret`) that never actually executes because it lacks
a function seed -- `RECOMP_ICALL_SAFE` returns `eax=0` instead of the real
field value on every call (`tools/recomp/output/scda_v060/gen/recomp_types.h:990-1013`).
One caller was identified (`sub_0007DC2D`, a generic multiple-inheritance
thunk: `ecx=[ecx+0x450]; icall vtable[0x34]`) -- this is standard C++ ABI
plumbing used across many unrelated class families throughout the game,
not specific to our ring-buffer object. **Assessment:** a real,
well-evidenced (both statically and at runtime) correctness bug, but
unlikely to be the DIRECT cause of the `0x8070`-byte zeroing, since it only
ever produces a wrong READ value (0 instead of the field), never writes
anything itself. **Deprioritized rather than fixed:** a full
regenerate+rebuild historically costs over an hour with no guaranteed
signal (per Update 202's own handoff text); given this finding's low
object-specificity, that is not the most cost-effective next action right
now. Logged as a possible separate correctness fix for a later round, not
treated as an active candidate for the `0x8070` bug.

**Re-launched the recommended test:** the two `cdb_lipsync_first_icall_capture`
processes left running by Update 202 (PIDs `3660`/`15280` and
`21136`/`29212`) were already orphaned (no reachable log, launched without
`-logo`) -- left untouched. A third, properly logged copy of the same
already-prepared script was launched
(`tools/debug/cdb_lipsync_first_icall_capture3_out.log`, PID `21308`/`13260`).
Interim status: the object in the target process is already zeroed (thread 0
long past the fatal callback), but the Lipsync breakpoint has not fired yet --
consistent with Update 199's finding that the Lipsync worker outlives the
main thread's fatal moment and keeps running independently; result to
follow in a separate update.

**Model tier stays MAXIMUM** (unchanged, see Update 202 for reasoning).
"""

with open(status_path, "ab") as f:
    f.write(status_text.replace("\n", "\r\n").encode("utf-8"))

with open(worklog_path, "ab") as f:
    f.write(worklog_text.replace("\n", "\r\n").encode("utf-8"))

print("done")
