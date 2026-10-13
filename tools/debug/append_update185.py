status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 185 (2026-09-18): Objekt-Feldkette live gelesen, gezielter want/got-Trace auf xemu bestaetigt: reale Hardware erlebt exakt denselben Short-Read wie scda_recomp und stoppt danach sauber

**Test 1 (`tools/debug/xemu_object_fileobj.py`, neu):** Am bestaetigten Objekt (`this=0x00868CC0`, Vtable `0x4F6F08`) live die Feldkette gelesen: `subobj=0x00879ED0` (Vtable `0x4F6940`), `file_obj (subobj+0x3c) = 0x18`, `this+0x44 (consumed) = 0x275A0D6A`, `subobj+0x50 (avail) = 0x3FFB`.

**Test 2 (`tools/debug/xemu_object_refill_wantgot.py`, neu):** Breakpoint auf `sub_000516B7` (die tatsaechliche Disc-I/O-Funktion), gefiltert auf `file_obj=0x18`, mit temporaeren Breakpoints an beiden bekannten `sub_00040821`-Rueckkehrpunkten (`0x00040983`, `0x000408E1`), um den tatsaechlich transferierten Byte-Count aus dem Out-Parameter zu lesen. Ueber 290 Sekunden: 16 Treffer fuer dieses Objekt, davon zwei Short-Reads. Der entscheidende: **Treffer #16 bei t=27.828s: want=0x00020000 got=0x00015F71 -- byte-exakt identisch mit dem, was scda_recomp bei genau diesem Objekt/dieser Position erlebt.** Nach diesem Treffer: keine weiteren `sub_000516B7`-Aufrufe fuer `file_obj=0x18` in den restlichen ~262 Sekunden -- der Stream stoppt auf echter Hardware sauber von selbst, kein Absturz.

**Schlussfolgerung:** Die reale Hardware erlebt exakt denselben Short-Read wie scda_recomp und behandelt ihn als normales, sauberes Streamende. Der urspruengliche `scda_disc_fill_overrun`-Mechanismus (der den gemeldeten Byte-Count kuenstlich auf die volle angeforderte Groesse aufblies) verbarg dieses Signal vor dem Spiel und ist damit als fehlerhaft bestaetigt.
"""

worklog_text = """
## Update 185 (2026-09-18): Object field chain read live, targeted want/got trace confirms on xemu: real hardware experiences the exact same short read as scda_recomp and then stops cleanly

**Test 1 (`tools/debug/xemu_object_fileobj.py`, new):** read the field chain live on the confirmed object (`this=0x00868CC0`, vtable `0x4F6F08`): `subobj=0x00879ED0` (vtable `0x4F6940`), `file_obj (subobj+0x3c) = 0x18`, `this+0x44 (consumed) = 0x275A0D6A`, `subobj+0x50 (avail) = 0x3FFB`.

**Test 2 (`tools/debug/xemu_object_refill_wantgot.py`, new):** breakpoint on `sub_000516B7` (the actual disc-I/O function), filtered to `file_obj=0x18`, with temporary breakpoints at both known `sub_00040821` return sites (`0x00040983`, `0x000408E1`) to read the true bytes-transferred count from the out-parameter. Over 290 seconds: 16 hits for this object, two of them short reads. The decisive one: **hit #16 at t=27.828s: want=0x00020000 got=0x00015F71 -- byte-exact match for what scda_recomp experiences at this same object/position.** After that hit: zero further `sub_000516B7` calls for `file_obj=0x18` in the remaining ~262 seconds -- the stream stops cleanly on its own on real hardware, no crash.

**Conclusion:** real hardware experiences the exact same short read as scda_recomp and treats it as a normal, clean end of stream. This confirms the original `scda_disc_fill_overrun` mechanism (which artificially inflated the reported byte count to the full requested size) hid that signal from the title and was genuinely wrong.
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
