status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 192 (2026-09-20): Allocator als dlmalloc-artiger Small-Block-Heap identifiziert; Verbindung zu einem alten, nie abgeschlossenen Heap-Freilisten-Bugreport gefunden; Reframing von "Objekt-Bug" zu "moeglicher Heap-Metadaten-Korruption"

**Same-Process-Beweis erneut bestaetigt (mit aktuellem, Frame-Pointer-gefixtem Build):** Leser-Objekt bleibt am Fatal-Callback vollstaendig intakt (`this` via Live-Suche gefunden, Vtable `0x4F6F08`, `+0x3C=0x1135928` weiterhin gueltig gesetzt, `+0x44` (verbrauchte Bytes) = `0x26f1ec51`, ein plausibler, grosser Wert). Das referenzierte Unterobjekt ist weiterhin vollstaendig genullt. Identisch zu Update 187, jetzt mit dem Frame-Pointer-Fix reproduziert -- die Objekt/Owner-Beziehung selbst ist nicht die Fehlerquelle.

**Speicherlayout des Unterobjekts vollstaendig aufgeklaert:** Live gelesen (frischer Lauf, GOOD-State): Header `this=0x1135928..0x1139990` (`0x4068` Bytes, exakt Allocator-Blockgroesse), `+0x4058`(Puffergroesse)=`0x24000`, **`+0x405C`(Ringpuffer-Zeiger)=`0x1139998`** -- nur 8 Bytes NACH dem Ende des Headers! Header und Ringpuffer-Daten wurden vom selben Allocator-Aufruf in unmittelbarer Folge alloziert.

**Allocator identifiziert:** `MEM32(0x5B57F8)` ist NICHT eine gescopte Arena, sondern das Spiel-eigene, durchgehend genutzte Small-Block-Heap (Vtable `0x4F62E4`; Slot+0=Allocate -> `sub_00077B38` -> `sub_0003EDBF`; Slot+8=Free -> `sub_00077176` -> `sub_0003EDDE`). Ueber alle generierten Quellen: **552** Aufrufstellen fuer Free, **202** fuer Allocate -- statische Suche nach "welcher konkrete Call gibt UNSER Objekt frei" ist praktisch nicht durchfuehrbar.

**Free-Pfad bis zur eigentlichen Freilisten-Logik verfolgt:** `sub_0029FA18` -> `sub_00077176` -> `sub_000767ED` (Lazy-Init + `RtlEnterCriticalSection(0x6B27A0)`, Ordinal 277/291 live bestaetigt) -> `sub_00077049` -> `sub_0007708F` usw. **`sub_00077049`/`sub_0007708F` sind ein klassischer dlmalloc/ptmalloc-artiger Small-Block-Allocator**: Boundary-Tag-Chunk-Header (`MEM32(chunk-4)` = Size|Flags, `PREV_INUSE`-artiges Low-Bit), Size-Class-Freilisten-Array bei `0x64D29C`, ein "Top-Chunk"-Zeiger bei `0x64D2D0` (klassisches dlmalloc-Konzept), ein weiterer Listenkopf bei `0x64D2E0`, Chunk-Coalescing mit benachbarten freien Bloecken. Kein `memset(0, ...)`-Aufruf o.ae. wurde in diesem Pfad bisher gefunden -- die Nullung koennte aus dem NORMALEN Freigabe-/Coalescing-Ablauf eines bereits woanders **korrumpierten Chunk-Headers** resultieren, nicht aus einem expliziten Aufruf auf unser Objekt.

**Wichtiger Fund -- Verbindung zu einem alten, nicht abgeschlossenen Bugreport:** Der Critical-Section-Handle `0x6B27A0` liegt nur `0x20` Bytes hinter `0x006B2780`, dem Heap-Objekt, das in einem VIEL AELTEREN, weiter unten in dieser Datei erhaltenen Bericht (26. August 2026) als Ursache eines *anderen* Crashes benannt wurde: `sub_00056B22` liest eine nicht initialisierte Heap-Freiliste (`[heap+0x180+4]` ist Null statt eines gueltigen Blocks) und stuerzt mit `0xC0000005` ab. Der dortige Stack lautet `sub_00056B22 -> sub_00076818 -> sub_00076849 -> sub_00077194 -> sub_000774E6 -> sub_00077B38 -> sub_0003EDBF -> ... -> sub_00048B15` -- **`sub_00077B38`/`sub_0003EDBF` sind exakt unsere heute bestaetigte Allocate-Funktion, und `sub_00048B15` ist unsere heutige Level-/Map-Load-Einstiegsfunktion.** Beide Berichte betreffen denselben Small-Block-Heap, denselben Aufrufkontext (`sub_00048B15`), und der alte Bericht empfahl bereits: "die Erzeugung/Initialisierung des Heapobjekts bei `0x006B2780` bzw. des temporären Heapbereichs `0x00F80000` rückwärts bis zum ersten fehlenden Initialisierungsschritt untersuchen" -- eine Empfehlung, die nie weiterverfolgt wurde, weil sich die Untersuchung seitdem auf das Ringpuffer-Unterobjekt-Symptom verlagert hat.

**Reframing:** Die urspruengliche Annahme "ein spezifisches Objekt wird faelschlich zerstoert" ist moeglicherweise zu eng. Die neue Arbeitshypothese: **eine Heap-Metadaten-Korruption** (z.B. ein Off-by-N-Schreibfehler, der den Boundary-Tag-Header EINES ANDEREN, fruehen Chunks ueberschreibt) **fuehrt dazu, dass der Allocator spaeter -- beim Freigeben/Koaleszieren eines voellig anderen, unverdaechtigen Blocks -- unseren Ringpuffer-Header als Kollateralschaden ueberschreibt**, ohne dass jemals ein Destruktor oder ein expliziter Free-Aufruf FUER UNSER OBJEKT stattfindet. Das erklaert zwanglos: warum kein bekannter Destruktorpfad feuert (bewiesen, Update 190), warum die Leser-Beziehung intakt bleibt (dangling erst NACH der Korruption sichtbar), warum die Adresse laufuebergreifend deterministisch ist (Allocator-Verhalten ist bei identischer Alloc/Free-Sequenz deterministisch), und moeglicherweise auch, warum reale Hardware nicht betroffen ist (eine geringfuegig andere Alloc/Free-Reihenfolge oder -Groesse wuerde denselben Korruptionsmechanismus knapp verfehlen).

**MODEL TIER:** MAXIMUM

**MODEL RECOMMENDATION:** In Codex: staerkste verfuegbare Analyse-Stufe. In
Claude: staerkstes verfuegbares Modell mit hoechster Reasoning-Tiefe.

**REASON:** Das Problem hat sich von "ein Objekt wird falsch behandelt" zu
"moeglicher Heap-Metadaten-Korruptionsfehler in einem dlmalloc-artigen
Allocator, der mit einem alten, unabhaengig dokumentierten Bugreport
zusammenhaengt" entwickelt. Das erfordert das gleichzeitige Verstehen von:
Recompiler-Codegenerierung, x86-Allocator-Interna (Boundary-Tags, Binning,
Coalescing), der urspruenglichen Spiellogik UND einer bereits Wochen alten,
unabhaengigen Untersuchung desselben Subsystems aus einem anderen Blickwinkel
-- klassisches Kriterium fuer die hoechste Eskalationsstufe, nicht mehr fuer
STRONG allein.

**Naechster Test (fuer MAXIMUM-Modell):**
1. Das `0x006B2780`-Heapobjekt und den `0x00F80000`-Temp-Heap-Bereich aus dem
   alten Bericht (siehe Abschnitt "Aktueller reproduzierbarer Blocker"
   weiter unten in dieser Datei) mit dem heutigen `sub_00077049`-Allocator
   zusammenfuehren: sind es dieselbe Heap-Instanz? Falls ja, ist die
   urspruengliche Init-Reihenfolge dieses Heaps der naheliegendste
   gemeinsame Ursprung fuer BEIDE Symptome.
2. Live: Boundary-Tag-Header direkt VOR dem Ringpuffer-Header
   (`0x1135928 - 8 = 0x1135920`, `MEM32(0x1135924)`=Size|Flags-Feld) im
   GOOD-State auslesen und mit den erwarteten dlmalloc-Invarianten
   (PREV_INUSE-Bit, Groesse rundet auf 8, Top-Chunk-Grenzen) abgleichen --
   zeigt sofort, ob DIESER Header bereits im GOOD-State plausibel oder schon
   subtil falsch ist.
3. Wenn Zeit reicht: eine minimale Heap-Integritaetspruefung (Walk aller
   Chunks von `MEM32(0x64D2D0)` "top" abwaerts, Boundary-Tag-Konsistenz
   pruefen) bei jedem `sub_00077049`-Aufruf einbauen -- aber nur wenn die
   Aufruffrequenz das zulaesst (bisher unbekannt, noch nicht gemessen; falls
   zu heiss, per Sampling statt jedes Aufrufs).

**Relevante neue Evidenz:** `tools/recomp/output/scda_v060/gen/recomp_0003.c`
(Zeilen 50537-50700+, Allocator-Kern), `recomp_0001.c` (Zeilen 60820-60915,
Vtable-Thunks), `tools/debug/good_dd_lines.txt`/`zero_samepid_dd_lines.txt`
(weiterhin gueltig), Abschnitt "Aktueller reproduzierbarer Blocker" in
diesem Dokument (der alte, 26.08.2026 datierte Bericht).
"""

worklog_text = """
## Update 192 (2026-09-20): Allocator identified as a dlmalloc-style small-block heap; found a connection to an old, never-completed heap free-list bug report; reframed from "single object bug" to "possible heap metadata corruption"

**Re-confirmed the same-process finding with the current, frame-pointer-fixed build:** the reader object stays fully intact at the fatal callback (found via live memory search, vtable `0x4F6F08`, `+0x3C=0x1135928` still validly set, `+0x44` (consumed bytes) = `0x26f1ec51`, a plausible large value). The referenced subobject remains completely zeroed. Identical to Update 187, now reproduced with the frame-pointer fix in place -- the object/owner relationship itself is not the fault.

**Fully mapped the subobject's memory layout:** live-read from a fresh run (GOOD state): header `this=0x1135928..0x1139990` (`0x4068` bytes, exactly the allocator's block size), `+0x4058` (buffer size) = `0x24000`, **`+0x405C` (ring buffer pointer) = `0x1139998`** -- only 8 bytes AFTER the header's own end. The header and its ring buffer data were allocated back-to-back from the same allocator.

**Identified the allocator:** `MEM32(0x5B57F8)` is not a scoped arena -- it's the title's own persistent, general-purpose small-block heap (vtable `0x4F62E4`; slot+0 = Allocate -> `sub_00077B38` -> `sub_0003EDBF`; slot+8 = Free -> `sub_00077176` -> `sub_0003EDDE`). Across all generated sources: **552** call sites for Free, **202** for Allocate -- a static search for "which specific call frees OUR object" is not practically tractable.

**Traced the Free path down to the actual free-list logic:** `sub_0029FA18` -> `sub_00077176` -> `sub_000767ED` (lazy-init + `RtlEnterCriticalSection(0x6B27A0)`, ordinals 277/291 confirmed live) -> `sub_00077049` -> `sub_0007708F` etc. **`sub_00077049`/`sub_0007708F` are a classic dlmalloc/ptmalloc-style small-block allocator**: boundary-tag chunk headers (`MEM32(chunk-4)` = size|flags, a `PREV_INUSE`-style low bit), a size-class free-list array at `0x64D29C`, a "top chunk" pointer at `0x64D2D0` (a textbook dlmalloc concept), another list head at `0x64D2E0`, and chunk coalescing with adjacent free blocks. No explicit `memset(0, ...)`-style call has been found in this path so far -- the zeroing could result from the allocator's NORMAL free/coalesce flow acting on a chunk header that was **corrupted elsewhere**, rather than from an explicit call targeting our object.

**Important finding -- connection to an old, unresolved bug report:** the critical-section handle `0x6B27A0` sits only `0x20` bytes past `0x006B2780`, the heap object named in a MUCH OLDER report preserved further down in `PROJECT_STATUS.md` (dated 2026-08-26) as the cause of a *different* crash: `sub_00056B22` reads an uninitialized heap free-list (`[heap+0x180+4]` is null instead of a valid block) and crashes with `0xC0000005`. That report's stack is `sub_00056B22 -> sub_00076818 -> sub_00076849 -> sub_00077194 -> sub_000774E6 -> sub_00077B38 -> sub_0003EDBF -> ... -> sub_00048B15` -- **`sub_00077B38`/`sub_0003EDBF` are exactly today's confirmed Allocate function, and `sub_00048B15` is today's confirmed level/map-load entry function.** Both reports touch the same small-block heap, in the same calling context (`sub_00048B15`), and the old report already recommended: "investigate the heap object's creation/initialization at `0x006B2780`, or the temporary heap region `0x00F80000`, working backward to the first missing initialization step" -- a recommendation that was never followed up on because the investigation since shifted to the ring-buffer-subobject symptom instead.

**Reframing:** the original assumption ("a specific object is wrongly destroyed") may be too narrow. New working hypothesis: **heap metadata corruption** (e.g. an off-by-N write that overwrites the boundary-tag header of a DIFFERENT, earlier chunk) **causes the allocator, later, while freeing/coalescing some completely different, unsuspicious block, to overwrite our ring-buffer header as collateral damage**, without any destructor or explicit Free call ever targeting our object directly. This cleanly explains: why no known destructor path ever fires (proven, Update 190), why the reader relationship stays intact (the dangling reference only becomes visible AFTER the corruption), why the address is deterministic across runs (allocator behavior is deterministic given an identical alloc/free sequence), and plausibly why real hardware is unaffected (a slightly different alloc/free order or size would narrowly miss the same corruption trigger).

**MODEL TIER:** MAXIMUM

**MODEL RECOMMENDATION:** In Codex: the strongest available analysis tier.
In Claude: the strongest available model at maximum reasoning depth.

**REASON:** the problem has grown from "one object is mishandled" to
"a possible heap-metadata-corruption bug in a dlmalloc-style allocator,
connected to an old, independently documented bug report." That requires
simultaneously understanding recompiler codegen, x86 allocator internals
(boundary tags, binning, coalescing), the original game logic, AND a
weeks-old independent investigation of the same subsystem from a different
angle -- the textbook case for the highest escalation tier, beyond STRONG
alone.

**Next test (for the MAXIMUM-tier model):**
1. Reconcile the `0x006B2780` heap object and `0x00F80000` temp-heap region
   from the old report (see the "Aktueller reproduzierbarer Blocker"
   section further down in `PROJECT_STATUS.md`) with today's
   `sub_00077049` allocator: is it the same heap instance? If so, this
   heap's own initialization order is the most likely shared root cause
   for BOTH symptoms.
2. Live: read the boundary-tag header immediately before the ring-buffer
   header (`0x1135928 - 8 = 0x1135920`, `MEM32(0x1135924)` = size|flags
   field) in the GOOD state and check it against the expected dlmalloc
   invariants (PREV_INUSE bit, size rounds to 8, top-chunk bounds) -- this
   immediately shows whether this header is already subtly wrong even in
   the GOOD state.
3. If time allows: add a minimal heap-integrity walk (from `MEM32(0x64D2D0)`
   "top" downward, checking boundary-tag consistency) on every
   `sub_00077049` call -- only if call frequency allows it (not yet
   measured; sample instead of tracing every call if it's too hot).
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
