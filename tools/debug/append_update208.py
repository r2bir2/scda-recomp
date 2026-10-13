status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 208 (2026-09-21): Alle drei Aufrufstellen des Disc-Fehler-Dialogs identifiziert und geprueft; Trigger-Bedingung gefunden -- fuehrt zu einer noch nicht vollstaendig gelesenen Decoder-Statemachine als neuem, konkretem naechsten Ziel

**Ausgangspunkt:** Update 207 zeigte, dass `sub_0003FCA5` (der Disc-Fehler-
Dialog) von genau DREI Stellen im Code aufgerufen wird. Wichtiger
Hinweis: Die in Live-Stacks gezeigten Aufrufer-Symbole (`sub_00040821+0x10d9`
etc.) sind auch hier per Host-Offset-Problem (Update 205) potenziell
irrefuehrend -- die tatsaechlichen Aufrufer wurden per Adressbereichs-
Abgleich der Original-Guest-Adressen ermittelt: `sub_000356F1`,
`sub_00038BD5`, `sub_00038F96`.

**Alle drei Aufrufstellen gelesen:**
1. `sub_000356F1`: dünner Wrapper -- ruft `sub_000516B7` (die bereits in
   Update 196 vollstaendig gelesene und als sauber bestaetigte NTSTATUS-
   Uebersetzungs-/Read-Dispatch-Funktion) auf; bei Fehlschlag (`eax==0`)
   direkt Disc-Fehler-Dialog. Kein Bulk-Zero.
2. `sub_00038BD5`/`sub_00038F96` (zwei Stellen in derselben Funktions-
   familie, Kategorie "game_video"): beide rufen `sub_00038A43` auf und
   pruefen `eax==4`; bei Treffer wird (in einer der beiden Stellen)
   zusaetzlich `sub_000368A5` aufgerufen -- eine winzige (41 Byte) Cleanup-
   Funktion, die EIN Handle-Feld (`this+0x18`) schliesst (`sub_004BABE0`/
   `sub_004BBAF0`) und danach explizit auf 0 setzt. Kein Bulk-Zero, nur
   ein einzelnes 4-Byte-Feld betroffen.

**Neuer, konkreter naechster Kandidat -- `sub_00038A43` (402 Bytes/134
Instruktionen, Kategorie "game_video", NOCH NICHT vollstaendig gelesen):**
Diese Funktion erzeugt den Statuscode `4`, der in beiden game_video-
Aufrufstellen die Fehleranzeige ausloest. Sie operiert auf einem Objekt
(`esi=ecx`) mit Feldern bis mindestens `+0xFC` -- KLEINER als unser
`0x8070`-Byte-Ringpuffer, vermutlich ein Decoder-/Stream-Kontext
("Reader"), der ueber `esi+0xFC` auf eine weitere Struktur zeigt (moeglicher
Kandidat fuer den vom User erfragten "Parent/Reader"). Enthaelt Aufrufe zu
`sub_000516B7` (Read-Dispatch, bereits bekannt) und mehreren noch nicht
untersuchten Funktionen (`sub_00052A9C`, `sub_00052A2F`, `sub_00036541`).
NICHT vollstaendig gelesen in dieser Runde (Zeitbudget) -- klar benannter
naechster Schritt.

**Bewertung:** Alle drei Downstream-Aufrufstellen des Fehlerdialogs sind
jetzt geprueft und bestaetigen erneut Update 196/207: der Dialog wird NUR
gezeigt, wenn eine Lese-/Decodier-Operation fehlschlaegt, niemals begleitet
von einem Bulk-Zero passender Groesse. Die Suche nach dem eigentlichen
Schreiber verschiebt sich damit praeziser auf die Decoder-Statemachine
`sub_00038A43` und ihre noch ungelesenen Unterfunktionen.

**Naechster Test:** `sub_00038A43` vollstaendig lesen, insbesondere die
Felder `esi+0x64/0x68/0x6C/0x78/0xFC` und die darueber erreichbare
Substruktur (`eax=MEM32(esi+0xFC)`, Felder `eax+0x10/0x14/0x18`) auf
Groessen-/Zeigerberechnungen nahe `0x8070`/`0x4070`/`0x4000` pruefen, sowie
`sub_00052A9C`/`sub_00052A2F`/`sub_00036541` kurz gegenlesen.

**Model-Tier:** STRONG bleibt angemessen.
"""

worklog_text = """
## Update 208 (2026-09-21): All three call sites of the disc-error dialog identified and checked; trigger condition found -- leads to a not-yet-fully-read decoder state machine as the new concrete next target

**Starting point:** Update 207 showed that `sub_0003FCA5` (the disc-error
dialog) is called from exactly THREE places in the code. Important caveat:
the caller symbols shown in live stacks (`sub_00040821+0x10d9` etc.) are
again potentially misleading due to the host-offset issue (Update 205) --
the actual callers were determined by matching the original guest address
ranges: `sub_000356F1`, `sub_00038BD5`, `sub_00038F96`.

**All three call sites read:**
1. `sub_000356F1`: a thin wrapper -- calls `sub_000516B7` (the NTSTATUS
   translation/read-dispatch function already read in full and confirmed
   clean in Update 196); on failure (`eax==0`) it goes straight to the
   disc-error dialog. No bulk zero.
2. `sub_00038BD5`/`sub_00038F96` (two sites in the same function family,
   category "game_video"): both call `sub_00038A43` and check `eax==4`;
   on a hit, one of the two sites additionally calls `sub_000368A5` -- a
   tiny (41-byte) cleanup function that closes one handle field
   (`this+0x18`) via `sub_004BABE0`/`sub_004BBAF0` and then explicitly
   sets it to 0. No bulk zero, only a single 4-byte field affected.

**New concrete next candidate -- `sub_00038A43` (402 bytes / 134
instructions, category "game_video", NOT yet fully read):** this function
produces the status code `4` that triggers the error display at both
game_video call sites. It operates on an object (`esi=ecx`) with fields up
to at least `+0xFC` -- SMALLER than our `0x8070`-byte ring buffer,
presumably a decoder/stream context ("reader") that points, via
`esi+0xFC`, to a further structure (a possible candidate for the "parent/
reader" the user asked about). It calls `sub_000516B7` (read dispatch,
already known) and several not-yet-examined functions (`sub_00052A9C`,
`sub_00052A2F`, `sub_00036541`). Not fully read this round (time budget) --
a clearly named next step.

**Assessment:** all three downstream call sites of the error dialog are now
checked and again confirm Update 196/207: the dialog is only shown when a
read/decode operation fails, never accompanied by a bulk zero of matching
size. The search for the actual writer therefore shifts more precisely to
the decoder state machine `sub_00038A43` and its still-unread
sub-functions.

**Next test:** read `sub_00038A43` in full, in particular checking fields
`esi+0x64/0x68/0x6C/0x78/0xFC` and the substructure reachable through them
(`eax=MEM32(esi+0xFC)`, fields `eax+0x10/0x14/0x18`) for size/pointer
computations near `0x8070`/`0x4070`/`0x4000`, and briefly cross-check
`sub_00052A9C`/`sub_00052A2F`/`sub_00036541`.

**Model tier:** STRONG remains appropriate.
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
