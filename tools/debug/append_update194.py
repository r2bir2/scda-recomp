status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 194 (2026-09-20): Vtable-Familie mit gemeinsamer Basisklasse gefunden -- viele geerbte Methoden geteilt zwischen unserer Ringpuffer-Klasse und mindestens einer Schwesterklasse

**Fund:** Direkt neben unserer Subobjekt-Vtable (`0x4F6940`) liegt bei `0x4F69C0` (genau `0x80` Bytes / 32 Funktionszeiger weiter) eine ZWEITE, eng verwandte Vtable. Slot 0 (Destruktor) und Slot 1 (Refill/"Pump") unterscheiden sich (`sub_00040B00`/`sub_00040821` bei unserer Klasse vs. `sub_00040CCC`/`sub_00040BB0` bei der Schwesterklasse), aber VIELE der weiteren Slots sind IDENTISCH zwischen beiden Vtables: `0x3ED16`, `0x3ED60`, `0x3E60E`, `0xEE9D1`, `0x39DA8`, `0x39DA3`, `0x370659`, `0x1AEFA1`, `0x3E7B6`, `0x4528E7` (und weitere) kommen an denselben relativen Slot-Positionen in BEIDEN Vtables vor.

**Interpretation:** Dies ist das klassische Muster einer C++-Klassenhierarchie mit gemeinsamer Basisklasse: mehrere abgeleitete Klassen ueberschreiben nur einige wenige, klassen-spezifische virtuelle Methoden (hier: Destruktor und die Refill/Pump-Methode), waehrend sie den grossen Rest der Vtable von einer gemeinsamen Basisklasse ERBEN. Das ist eine wichtige Praezisierung der in Update 192/193 aufgestellten Hypothese: Eine GEERBTE, generische Basisklassen-Methode (z.B. ein "Cleanup"/"Reset"-Pfad) koennte fuer die "typische" Schwesterklasse mit einem kleineren, ANDERS verwalteten Puffer korrekt sein, waehrend sie bei UNSERER Klasse (mit separat alloziertem, viel groesseren Ringpuffer) eine falsche Groessenannahme trifft und dabei kollateral den Allocator-Boundary-Tag ueberschreibt.

**Alle geteilten Slot-Funktionen einzeln geprueft -- diese Spur fuehrt NICHT zum Ziel:** `sub_0003ED16` (Bit-Manipulation/Freilisten-Bitmap), `sub_0003ED60` (duenner Dispatch-Thunk), `sub_0003E60E`/`sub_000EE9D1`/`sub_001AEFA1`/`sub_00370659`/`sub_004528E7` (allesamt triviale 1-5-Byte No-Op-Stubs, typisch fuer Default-Implementierungen einer generischen Basisklassen-Schnittstelle), und `sub_00039DA3` (die einzige nicht-triviale geteilte Funktion, 114 Bytes -- stellt sich als Iterator-/Wertvergleichsfunktion heraus, mehrere kleine Vergleichsroutinen im selben Codeblock zusammengefasst, kein Speicherzugriff auf Puffergroessen). **Keine dieser Basisklassen-Methoden enthaelt ein `rep stosd`/`memset`-Uebersetzungsmuster.** Die Hypothese "eine geerbte Basisklassen-Cleanup-Methode nullt mit falscher Groesse" ist damit fuer DIESE Vtable-Familie widerlegt.

**Ehrlicher Zwischenstand:** Ausmass und Charakteristik des Bugs sind jetzt praezise bekannt (Update 193: exakt `~0x8070` Bytes ab dem Allocator-Boundary-Tag, `0x4070+0x4000` entsprechend), aber die KONKRETE Funktion/der KONKRETE Call, der dies ausloest, ist noch nicht gefunden. Die naheliegenden Kandidaten (Klassen-eigene Destruktoren, geteilte Basisklassen-Methoden) sind widerlegt. Offene Kandidaten fuer die naechste Runde: (a) eine Funktion, die NICHT ueber DIESE Vtable dispatcht, sondern das Objekt/den Ringpuffer ueber einen rohen, gespeicherten Zeiger direkt anspricht (z.B. ein Level-/Ressourcen-Manager, der Zeiger in einer eigenen Liste haelt); (b) eine Funktion in einer VOELLIG ANDEREN, noch nicht untersuchten Vtable-Familie, die zufaellig denselben Speicherbereich referenziert; (c) tatsaechlich ein generischer, nicht virtueller `memset`/`ZeroMemory`-Aufruf mit einer zur Laufzeit falsch berechneten Groesse, aufgerufen von einer Stelle, die ueberhaupt nichts mit C++-Vtables zu tun hat (z.B. direkt aus `sub_00048B15`s Level-Load-Ablauf).

**Model-Tier bleibt MAXIMUM** -- die naheliegenden Kandidaten sind erschoepft; die verbleibende Suche (Kandidat b/c oben) erfordert entweder eine sehr viel breitere Codesuche oder einen erneuten, sorgfaeltig konstruierten Live-Trace-Versuch, der die in dieser Sitzung wiederholt aufgetretene CDB-Breakpoint-Unzuverlaessigkeit umgeht (z.B. durch feinere Polling-Aufloesung anstelle bedingter Breakpoints).
"""

worklog_text = """
## Update 194 (2026-09-20): Found a vtable family with a shared base class -- many inherited methods shared between our ring-buffer class and at least one sibling class

**Finding:** directly next to our subobject's vtable (`0x4F6940`) sits a SECOND, closely related vtable at `0x4F69C0` (exactly `0x80` bytes / 32 function pointers further). Slot 0 (destructor) and slot 1 (refill/"pump") differ (`sub_00040B00`/`sub_00040821` for our class vs. `sub_00040CCC`/`sub_00040BB0` for the sibling class), but MANY of the remaining slots are IDENTICAL between both vtables: `0x3ED16`, `0x3ED60`, `0x3E60E`, `0xEE9D1`, `0x39DA8`, `0x39DA3`, `0x370659`, `0x1AEFA1`, `0x3E7B6`, `0x4528E7` (and more) appear at the same relative slot positions in BOTH vtables.

**Interpretation:** this is the classic pattern of a C++ class hierarchy with a shared base class: several derived classes override only a handful of class-specific virtual methods (here: the destructor and the refill/pump method) while inheriting the bulk of the vtable from a common base. This refines the hypothesis from Update 192/193: an INHERITED, generic base-class method (e.g. a "cleanup"/"reset" path) could be correct for the "typical" sibling class with a smaller, differently-managed buffer, while making a wrong size assumption for OUR class (with its separately-allocated, much larger ring buffer), collaterally overwriting the allocator's boundary tag in the process.

**Checked every shared slot function individually -- this lead does NOT pan out:** `sub_0003ED16` (bit manipulation / free-list bitmap), `sub_0003ED60` (thin dispatch thunk), `sub_0003E60E`/`sub_000EE9D1`/`sub_001AEFA1`/`sub_00370659`/`sub_004528E7` (all trivial 1-5-byte no-op stubs, typical default implementations of a generic base-class interface), and `sub_00039DA3` (the one non-trivial shared function, 114 bytes -- turns out to be an iterator/value-comparison function, several small comparison routines packed into the same code block, no access to buffer-size fields). **None of these base-class methods contains a `rep stosd`/`memset` translation pattern.** The hypothesis "an inherited base-class cleanup method zeroes with the wrong size" is therefore disproven for THIS vtable family.

**Honest interim status:** the extent and character of the bug are now precisely known (Update 193: exactly `~0x8070` bytes starting at the allocator's boundary tag, matching `0x4070+0x4000`), but the CONCRETE function/call that triggers it has not yet been found. The obvious candidates (the class's own destructors, shared base-class methods) are disproven. Open candidates for the next round: (a) a function that does NOT dispatch through THIS vtable at all, but addresses the object/ring buffer via a raw, separately-stored pointer (e.g. a level/resource manager holding pointers in its own list); (b) a function in a COMPLETELY DIFFERENT, not-yet-investigated vtable family that happens to reference the same memory region; (c) an actual generic, non-virtual `memset`/`ZeroMemory` call with a size miscomputed at runtime, invoked from somewhere with nothing to do with C++ vtables at all (e.g. directly from `sub_00048B15`'s level-load flow).

**Model tier stays MAXIMUM** -- the obvious candidates are exhausted; the remaining search (candidate b/c above) needs either a much broader code search or a carefully re-engineered live-trace attempt that works around the CDB breakpoint unreliability seen repeatedly this session (e.g. finer-grained polling instead of conditional breakpoints).
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
