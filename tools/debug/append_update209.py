status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 209 (2026-09-21): sub_00038A43 vollstaendig analysiert und byte-genau gegen die Original-XBE verifiziert -- Trigger-Mechanismus verstanden, aber mehrere nicht-triviale Ursachen bleiben offen

**Ziel dieser Runde:** `sub_00038A43` (Kandidat aus Update 208) vollstaendig
verstehen: welcher Branch erzeugt Status `4` (loest `sub_0003FCA5`, den
Disc-Fehler-Dialog, aus), welche Bedingung fuehrt dorthin, und woher der
dafuer verwendete Wert stammt.

**Vollstaendige Ablaufverfolgung von `sub_00038A43` (402 Bytes/134
Instruktionen, komplett gelesen):**
- `esi` = "this" des Decoders (Kategorie "game_video"); `eax = MEM32(esi+0xFC)`
  ist ein Zeiger auf ein kleineres "Reader"-Objekt (die vom User erfragte
  Parent/Reader-Struktur).
- Der ENTSCHEIDENDE Vergleich (Adresse `0x00038AFB`/`0x00038B00`):
  `sub_00052A9C(...)` wird aufgerufen; liefert sie `!=0`, geht es normal
  weiter (`loc_00038B0E`). Liefert sie `0`, wird `sub_00052A2F()`
  (= GetLastError-Aequivalent ueber einen Thread-lokalen Fehler-Slot,
  indiziert per FS-Segment/TEB) aufgerufen. Ist das Ergebnis WEDER `0x26`
  (`ERROR_HANDLE_EOF`) NOCH `0x3E5` (`ERROR_IO_PENDING`), wird Status `4`
  zurueckgegeben -- DAS ist der Ausloeser fuer den Disc-Fehler-Dialog.

**`sub_00052A9C` vollstaendig gelesen (die eigentliche Status-Quelle):**
Sie interpretiert das Reader-Objekt wie ein `IO_STATUS_BLOCK` (Feld `+0`
= Status, Feld `+4` = transferierte Bytes):
- Ist `Reader.Status == 0x103` (`STATUS_PENDING`): wartet ggf. per
  `sub_00052D2E` (Sync-Primitive-Wrapper, bereits bekannt) auf Abschluss;
  bei Groesse-0-Anfrage oder Wait-Fehlschlag wird explizit
  `SetLastError(0x3E4)` (`ERROR_IO_INCOMPLETE`) gesetzt und `0` (Fehler)
  zurueckgegeben.
- Ist `Reader.Status != 0x103` (z.B. bereits abgeschlossen ODER genullt):
  kopiert `Reader.Information` (Bytes) in den Ausgabeparameter, liest
  `Reader.Status` erneut, und: **wenn NICHT-NEGATIV (Vorzeichenbit 0,
  EINSCHLIESSLICH des Werts 0!): Rueckgabe `1` (ERFOLG)** -- KEIN
  GetLastError-Check folgt in diesem Fall. Nur wenn NEGATIV (Vorzeichenbit
  gesetzt, ein echter NTSTATUS-Fehlercode mit hoher Severity): wird dieser
  Wert per `SetLastError` weitergereicht und `0` (Fehler) zurueckgegeben.

**Byte-genaue Verifikation gegen die Original-XBE (Capstone):** Sowohl die
Vergleichsstelle in `sub_00038A43` (`0x38ae1`-`0x38b09`) als auch die
komplette Logik von `sub_00052A9C` (`0x52a9c`-`0x52afe`) wurden disassembliert
und STIMMEN EXAKT mit dem generierten C-Code ueberein -- **kein
Recompiler-Fehler in dieser Kette.**

**Zentrale, praezise Erkenntnis:** Ein einfaches VOLLSTAENDIGES Nullen des
Reader-Objekts (`Status=0`, `Information=0`) wuerde NICHT den beobachteten
Fehlerpfad ausloesen -- `Status=0` ist NICHT-NEGATIV und wuerde als
"Erfolg mit 0 Bytes" interpretiert (`sub_00052A9C` liefert `1`, GetLastError
wird nie geprueft, `sub_00038A43` faehrt normal fort statt Status `4`
zurueckzugeben). Die einfache Erklaerung "das gesamte Objekt wurde genullt,
deshalb schlaegt der Read fehl" ist damit fuer DIESEN spezifischen
Mechanismus WIDERLEGT -- der tatsaechliche Trigger muss einer von diesen
sein:
1. Ein ECHTER, NEGATIVER NTSTATUS-Fehlercode im Reader-Status-Feld
   (nicht einfach Null) zum Zeitpunkt der Pruefung, der ausserdem nicht
   zufaellig exakt `0x26`/`0x3E5` ergibt.
2. Der `sub_00052D2E`-Wait-Aufruf schlaegt fehl oder die Groessen-Abfrage
   (`MEM32(esp+0x14)==0`) trifft zu, was zu `ERROR_IO_INCOMPLETE` (`0x3E4`)
   fuehrt -- unabhaengig vom Zustand des Reader-Objekts selbst.
3. Der tatsaechlich im konkreten Absturz genommene Pfad ist eine der
   ANDEREN beiden Aufrufstellen von `sub_0003FCA5` (`sub_000356F1` ueber
   `sub_000516B7`), nicht `sub_00038A43`/`sub_00038F96` -- das wurde in
   dieser Runde nicht live verifiziert.

**Kausalitaets-Einordnung (wie vom User erfragt):** Diese Runde liefert
keinen direkten Beweis fuer "Zeroing verursacht Read-Fehler" ODER
"Read-Fehler loest Zeroing aus" -- sie zeigt praezise, dass EIN einfacher
Bulk-Zero DIESES SPEZIFISCHEN Reader-Feldes NICHT ausreicht, um den
beobachteten Fehlerpfad zu erklaeren. Damit bleibt am plausibelsten:
beide (das 0x8070-Zeroing UND der Disc-Fehler) sind Folgen eines noch
frueheren, gemeinsamen Fehlerzustands -- ODER der Reader haelt tatsaechlich
einen echten negativen Fehlercode (keine einfache Nullung), was auf eine
ANDERE Art der Korruption als reines Zero-Fill hindeuten wuerde.

**Model-Tier:** Mehrere plausible Ursachen bleiben trotz vollstaendiger,
byte-verifizierter direkter Evidenz bestehen, und die Multi-Hop-Indirektion
(Decoder -> Reader-Zeiger -> IO_STATUS_BLOCK-artiger Status -> Thread-
lokale Fehler-Uebersetzung -> Vergleich gegen zwei Konstanten) ist
konzeptionell nicht-trivial.

MODEL STATUS: ESCALATION RECOMMENDED
Tier: MAXIMUM
Reason: Trotz vollstaendiger, byte-verifizierter Ablaufverfolgung bleiben
mehrere nicht-triviale, sich gegenseitig ausschliessende Erklaerungen
offen (echter negativer Statuscode vs. Timeout-Pfad vs. anderer
Aufrufort), und die einfachste Hypothese (Bulk-Zero verursacht den
Read-Fehler direkt) wurde durch die Ablaufverfolgung widerlegt.

**Naechster Test (fuer MAXIMUM):**
1. Klaeren, welche der drei `sub_0003FCA5`-Aufrufstellen im tatsaechlichen
   Absturz genommen wird (per Live-Stack an mehreren unabhaengigen
   Einzel-Attaches, gestaffelte Technik aus Update 206).
2. Falls `sub_00038A43`-Pfad bestaetigt: den tatsaechlichen Wert im
   Reader-Status-Feld (`MEM32(MEM32(decoder+0xFC))`) zum Absturzzeitpunkt
   bestimmen -- negativ (echter Fehlercode) vs. Timeout-Pfad.
3. Pruefen, ob das Reader-Objekt UEBERHAUPT innerhalb des bekannten
   `0x8070`-Byte-Bereichs liegt, oder eine separate, kleinere Allokation
   ist, die NUR zeitlich korreliert.
4. Bei Bestaetigung eines echten negativen Fehlercodes: dessen genauen
   Wert per Live-Read bestimmen und mit bekannten NTSTATUS-Werten
   abgleichen (koennte direkt auf den Korruptionsmechanismus zeigen, z.B.
   STATUS_ACCESS_VIOLATION vs. STATUS_INVALID_HANDLE vs. etwas
   Ringpuffer-Spezifisches).

**Relevante neue Artefakte:** `tools/debug/append_update209.py` (dieses
Update); keine neuen Live-Capture-Artefakte in dieser Runde (reine
statische + byte-verifizierte Analyse).
"""

worklog_text = """
## Update 209 (2026-09-21): sub_00038A43 fully analyzed and byte-verified against the original XBE -- trigger mechanism understood, but several non-trivial causes remain open

**Goal this round:** fully understand `sub_00038A43` (the candidate named
in Update 208): which branch produces status `4` (triggering
`sub_0003FCA5`, the disc-error dialog), what condition leads there, and
where the value used for that condition comes from.

**Full trace of `sub_00038A43` (402 bytes / 134 instructions, read in
full):**
- `esi` = the decoder's "this" (category "game_video"); `eax =
  MEM32(esi+0xFC)` is a pointer to a smaller "reader" object (the parent/
  reader structure the user asked about).
- The DECISIVE comparison (address `0x00038AFB`/`0x00038B00`):
  `sub_00052A9C(...)` is called; if it returns `!=0`, execution continues
  normally (`loc_00038B0E`). If it returns `0`, `sub_00052A2F()` (a
  GetLastError equivalent, reading a thread-local error slot indexed via
  the FS segment/TEB) is called. If that result is NEITHER `0x26`
  (`ERROR_HANDLE_EOF`) NOR `0x3E5` (`ERROR_IO_PENDING`), status `4` is
  returned -- THIS is what triggers the disc-error dialog.

**`sub_00052A9C` read in full (the actual status source):** it treats the
reader object like an `IO_STATUS_BLOCK` (field `+0` = Status, field `+4`
= bytes transferred):
- If `Reader.Status == 0x103` (`STATUS_PENDING`): it may wait for
  completion via `sub_00052D2E` (a sync-primitive wrapper, already known);
  on a zero-size request or a failed wait, it explicitly calls
  `SetLastError(0x3E4)` (`ERROR_IO_INCOMPLETE`) and returns `0` (failure).
- If `Reader.Status != 0x103` (e.g. already completed OR zeroed): it
  copies `Reader.Information` (byte count) into the output parameter,
  re-reads `Reader.Status`, and: **if NON-NEGATIVE (sign bit clear,
  INCLUDING the value 0!): returns `1` (SUCCESS)** -- no GetLastError check
  follows in this case. Only if NEGATIVE (sign bit set, a real high-
  severity NTSTATUS error code) is that value relayed via `SetLastError`
  and `0` (failure) returned.

**Byte-exact verification against the original XBE (Capstone):** both the
comparison site in `sub_00038A43` (`0x38ae1`-`0x38b09`) and the complete
logic of `sub_00052A9C` (`0x52a9c`-`0x52afe`) were disassembled and MATCH
the generated C code EXACTLY -- **no recompiler bug in this chain.**

**Central, precise finding:** a simple, COMPLETE zeroing of the reader
object (`Status=0`, `Information=0`) would NOT trigger the observed error
path -- `Status=0` is NON-NEGATIVE and would be interpreted as "success
with 0 bytes" (`sub_00052A9C` returns `1`, GetLastError is never checked,
`sub_00038A43` proceeds normally instead of returning status `4`). The
simple explanation "the whole object got zeroed, therefore the read fails"
is therefore DISPROVEN for this specific mechanism -- the actual trigger
must be one of:
1. A REAL, NEGATIVE NTSTATUS error code in the reader's status field (not
   simply zero) at the time of the check, which additionally doesn't
   happen to equal exactly `0x26`/`0x3E5`.
2. The `sub_00052D2E` wait call fails, or the size check
   (`MEM32(esp+0x14)==0`) is true, leading to `ERROR_IO_INCOMPLETE`
   (`0x3E4`) -- independent of the reader object's own state.
3. The path actually taken in the real crash is one of the OTHER two call
   sites of `sub_0003FCA5` (`sub_000356F1` via `sub_000516B7`), not
   `sub_00038A43`/`sub_00038F96` -- not live-verified this round.

**Causal framing (as requested):** this round does not directly prove
"zeroing causes the read failure" OR "the read failure triggers zeroing"
-- it precisely shows that a simple bulk zero of THIS SPECIFIC reader
field is NOT sufficient to explain the observed error path. The most
plausible remaining picture is: both the `0x8070` zeroing AND the disc
error are consequences of an even earlier, shared fault state -- OR the
reader actually holds a genuine negative error code (not a simple zero),
which would point to a different kind of corruption than a plain zero-fill.

**Model tier:** several plausible causes remain despite complete,
byte-verified direct-evidence tracing, and the multi-hop indirection
(decoder -> reader pointer -> IO_STATUS_BLOCK-like status -> thread-local
error translation -> comparison against two constants) is conceptually
non-trivial.

MODEL STATUS: ESCALATION RECOMMENDED
Tier: MAXIMUM
Reason: despite a complete, byte-verified trace, several non-trivial,
mutually exclusive explanations remain open (a real negative status code
vs. a timeout path vs. a different call site), and the simplest hypothesis
(bulk zero directly causes the read failure) was disproven by the trace.

**Next test (for MAXIMUM):**
1. Determine which of the three `sub_0003FCA5` call sites is actually
   taken in the real crash (via live stacks from several independent
   single attaches, the staggered technique from Update 206).
2. If the `sub_00038A43` path is confirmed: determine the actual value in
   the reader's status field (`MEM32(MEM32(decoder+0xFC))`) at crash time
   -- negative (real error code) vs. the timeout path.
3. Check whether the reader object even lies within the known `0x8070`-
   byte region, or is a separate, smaller allocation that is merely
   correlated in time.
4. If a real negative error code is confirmed: determine its exact value
   via a live read and cross-check it against known NTSTATUS values (it
   could point directly at the corruption mechanism, e.g.
   STATUS_ACCESS_VIOLATION vs. STATUS_INVALID_HANDLE vs. something
   ring-buffer-specific).

**Relevant new artifacts:** `tools/debug/append_update209.py` (this
update); no new live-capture artifacts this round (pure static +
byte-verified analysis).
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
