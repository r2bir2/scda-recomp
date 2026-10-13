status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 193 (2026-09-20): Exakte Ausdehnung des genullten Bereichs bestimmt -- 0x8070 Bytes, passt exakt zu "Header + ein Ringpuffer-Segment (0x4000)"; Off-by-8-Zeigerfehler als konkrete neue Arbeitshypothese

**Methodik-Korrektur:** Bedingte CDB-Breakpoint-Aktionen mit `.printf`/`kb` in geschweiften Klammern haengen sich in dieser Sandbox wiederholt still auf (kein Fehler, keine Ausgabe, der Zielprozess bleibt einfach angehalten) -- reproduziert sowohl mit einer verschachtelten als auch mit einer bewusst NICHT verschachtelten `.if`-Struktur. Das bestaetigt das bereits vom vorherigen Agenten dokumentierte Verhalten (CURRENT HANDOFF, vorherige Fassung) und wird ab sofort in dieser Untersuchung nicht mehr versucht. Die einzige verlaessliche Methode bleibt: minimal-invasives, wiederholtes `cdb -pv`-Polling mehrerer Adressen in einem einzigen Aufruf (~1.3s pro Poll bei 13 Adressen).

**Praezise Kernerkenntnis:** Boundary-Tag (`0x1135920`/`0x1135924`, das Allocator-eigene Size|Flags-Feld) und Vtable (`0x1135928`) werden NACHWEISLICH im selben Poll-Fenster (< 1.4 Sekunden Abstand) genullt -- kein zeitlich getrenntes Nacheinander. Per Binaersuche (mehrere `cdb -pv`-Aufrufe gegen denselben, bereits abgestuerzten Prozess, keine erneute zeitliche Unsicherheit) wurde die GENAUE obere Grenze des zusammenhaengend genullten Bereichs bestimmt: **zwischen `+0x8068` (noch Null) und `+0x8070` (bereits wieder echte, hochentropische Daten) relativ zum Objektanfang `0x1135928`.**

**Gesamtgroesse des genullten Bereichs: ca. `0x8070` Bytes**, beginnend beim Allocator-Boundary-Tag (`0x1135920`, also 8 Bytes vor dem User-Pointer). Das entspricht **exakt** `0x4070` (Header inkl. 8-Byte-Allocator-Header) **+ `0x4000`** (ein einzelnes Ringpuffer-Segment -- exakt die aus `sub_00040821`/`sub_00040766` bekannte Doppelpuffer-Segmentgroesse, die dort als `edx`-Wert zwischen `0` und `0x4000` wechselt). Bestaetigt durch eine SEPARATE, unabhaengige zeitnahe Messung: bei genau dem Poll-Uebergang, an dem Header+Boundary-Tag genullt wurden, wechselte GLEICHZEITIG auch der Wert bei `this+0x8000` von echten Audiodaten auf Null, waehrend `this+0x10000`/`this+0x18000` (weiter im Ringpuffer) UNVERAENDERT mit echten Daten blieben -- der genullte Bereich endet also weit VOR dem Ende des `0x24000` Byte grossen Ringpuffers, nicht am Ende.

**Widerlegt:** Chunks/Werte VOR unserem Objekt (`-0x494` (ein legitimer freier `0x120`-Byte-dlmalloc-Chunk), `-0x1000`, `-0x2020`) aendern sich in einer SEPARATEN, FRUEHEREN Poll-Runde als unser eigenes Objekt -- sie sind gewoehnliches, unabhaengiges Heap-Rauschen und NICHT Teil desselben Ereignisses. Die in Update 190/192 diskutierte Idee eines sehr breiten oder mehrere unabhaengige Bloecke umfassenden Bulk-Resets ist damit weiter eingeschraenkt: der tatsaechlich betroffene Bereich ist praezise und eng (`0x8070` Bytes), nicht "der ganze Heap in der Umgebung".

**Neue, konkrete Arbeitshypothese:** Eine Funktion berechnet einen Zielbereich als `(Zeiger, Groesse)` mit `Groesse = Header-Nutzgroesse + ein Puffersegment (0x4000)` und nullt ihn (memset-artig) -- aber der uebergebene ZEIGER liegt **8 Bytes zu frueh** (beim Allocator-Chunk-Start statt beim User-Pointer), wodurch der Allocator-eigene Boundary-Tag versehentlich mit ueberschrieben wird. Das waere ein klassischer Off-by-8-Zeigerfehler (evtl. eine Verwechslung zwischen "Chunk-Groesse inkl. Header" und "User-Groesse", die an anderer Stelle bereits als `0x4070` vs. `0x4068` beobachtet wurde -- siehe Boundary-Tag-Analyse in Update 192).

**Naechster Test:** Nach einer Funktion suchen, die (a) einen `memset`/`rep stosd`-artigen Nullungs-Block mit einer Groesse in der Naehe von `0x4068`/`0x4070` PLUS `0x4000` (additiv oder als zwei getrennte Aufrufe mit `0x4070` und `0x4000`) erzeugt, UND (b) deren Zieladresse aus einem Zeiger berechnet wird, der potenziell 8 Bytes zu frueh ansetzt (z.B. `ptr - 8` statt `ptr`, oder eine Groessenberechnung, die den Allocator-Header versehentlich mit einrechnet). Gegeben die CDB-Breakpoint-Unzuverlaessigkeit: eher per gezielter, eng gefasster Codesuche (Textsuche nach `stosd`/`memset`-Uebersetzungsmustern MIT einer Groessenkonstante nahe `0x4070`/`0x4000`/`0x8070` in unmittelbarer Naehe) als per Live-Trace.

**Relevante neue Artefakte:** `tools/debug/poll_boundary_out.log` (Boundary-Tag+Vtable-Poll, Uebergang bei Poll #23->#24), `tools/debug/poll_extent_out.log` (13-Adressen-Poll, Uebergang bei Poll #13->#14, zeigt `this+0x8000` faellt gleichzeitig, `this+0x10000`/`+0x18000` bleiben unveraendert).
"""

worklog_text = """
## Update 193 (2026-09-20): Determined the exact extent of the zeroed region -- 0x8070 bytes, matching "header + one ring-buffer segment (0x4000)" precisely; off-by-8 pointer bug is now a concrete working hypothesis

**Methodology correction:** conditional CDB breakpoint actions with `.printf`/`kb` inside braces repeatedly hang silently in this sandbox (no error, no output, the target process simply stays stopped) -- reproduced with both a nested and a deliberately non-nested `.if` structure. This confirms the behavior already documented by the previous agent (CURRENT HANDOFF, earlier version) and will not be attempted further in this investigation. The only reliable method remains: minimally-invasive, repeated `cdb -pv` polling of several addresses in a single invocation (~1.3s per poll for 13 addresses).

**Precise core finding:** the boundary-tag header (`0x1135920`/`0x1135924`, the allocator's own size|flags field) and the vtable (`0x1135928`) are demonstrably zeroed within the same poll window (< 1.4 seconds apart) -- not in a separate sequence. Via binary search (multiple `cdb -pv` calls against the same, already-crashed process, so no further timing uncertainty), the exact upper boundary of the contiguously-zeroed region was determined: **between `+0x8068` (still zero) and `+0x8070` (already back to real, high-entropy data) relative to the object start `0x1135928`.**

**Total size of the zeroed region: approximately `0x8070` bytes**, starting at the allocator's boundary tag (`0x1135920`, i.e. 8 bytes before the user pointer). This matches **exactly** `0x4070` (header including the 8-byte allocator header) **+ `0x4000`** (a single ring-buffer segment -- exactly the double-buffer segment size already known from `sub_00040821`/`sub_00040766`, which switches an `edx` value between `0` and `0x4000` there). Confirmed by a SEPARATE, independent tight-timing measurement: at the exact poll transition where the header+boundary-tag were zeroed, the value at `this+0x8000` simultaneously flipped from real audio data to zero, while `this+0x10000`/`this+0x18000` (further into the ring buffer) stayed UNCHANGED with real data -- the zeroed region ends well before the end of the `0x24000`-byte ring buffer, not at its end.

**Disproven:** values before our object (`-0x494` (a legitimate free `0x120`-byte dlmalloc chunk), `-0x1000`, `-0x2020`) change in a SEPARATE, EARLIER poll round than our own object -- they are ordinary, independent heap noise, not part of the same event. This further narrows the "wide or multi-block bulk reset" idea discussed in Update 190/192: the actually-affected region is precise and narrow (`0x8070` bytes), not "the whole surrounding heap."

**New, concrete working hypothesis:** some function computes a target region as `(pointer, size)` with `size = header payload size + one buffer segment (0x4000)` and zeroes it (memset-style) -- but the PASSED-IN POINTER is **8 bytes too early** (at the allocator chunk start instead of the user pointer), causing the allocator's own boundary tag to be overwritten as collateral damage. This would be a classic off-by-8 pointer bug (possibly a mixup between "chunk size including header" and "user size," already observed elsewhere as `0x4070` vs `0x4068` -- see the boundary-tag analysis in Update 192).

**Next test:** search for a function that (a) produces a `memset`/`rep stosd`-style zero-fill block with a size close to `0x4068`/`0x4070` PLUS `0x4000` (additive, or as two separate calls with `0x4070` and `0x4000`), AND (b) whose target address is computed from a pointer that could plausibly start 8 bytes too early (e.g. `ptr - 8` instead of `ptr`, or a size calculation that accidentally includes the allocator header). Given the CDB breakpoint unreliability: prefer a targeted, narrow source search (text search for `stosd`/`memset` translation patterns WITH a size constant near `0x4070`/`0x4000`/`0x8070` nearby) over a live trace.

**Relevant new artifacts:** `tools/debug/poll_boundary_out.log` (boundary-tag+vtable poll, transition at poll #23->#24), `tools/debug/poll_extent_out.log` (13-address poll, transition at poll #13->#14, shows `this+0x8000` dropping simultaneously while `this+0x10000`/`+0x18000` stay unchanged).
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
