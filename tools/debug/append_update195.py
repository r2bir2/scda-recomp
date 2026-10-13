status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 195 (2026-09-20): Unabhaengiger Root-Cause-Audit (MAXIMUM) -- Allocator-Engine (Allocate/Realloc/Free/Slot 0xC) vollstaendig auf Zero-Fill geprueft und ausgeschlossen; sub_00048B15 gelesen und ausgeschlossen; neue strukturelle Erklaerung fuer 0x8070

**Auftrag dieser Runde:** Statt weiter die Kandidatenliste aus Update 194 mechanisch abzuarbeiten, wurde ein unabhaengiger Audit durchgefuehrt: Welche Mechanismen koennen ueberhaupt exakt `0x8070` Bytes nullen, was bedeuten `0x8070`/`0x4070`/`0x4000` strukturell, und welche ganzen Mechanismus-Klassen lassen sich ausschliessen.

**1. Strukturelle Bedeutung von 0x8070/0x4070/0x4000 (praezisiert):** Der genullte Bereich beginnt exakt am Allocator-Chunk-Anfang (`this-8`, dem Boundary-Tag/Size-Feld) und erstreckt sich ueber GENAU `chunk_size_unseres_objekts (0x4070, inkl. 8-Byte-Header) + ein direkt anschliessendes Ringpuffer-Segment (0x4000)`. Wichtig, neu herausgearbeitet: Das Ende der Nullung liegt bei `this+0x8068`, was rechnerisch EXAKT `(this-8) + 0x4070 + 0x4000` entspricht -- die Grenze ist also nicht irgendein gerundeter Schaetzwert, sondern eine bytegenaue Summe aus zwei konkreten, bekannten Groessen. Daraus folgt: Der Schreiber muss (a) mit einem CHUNK-Zeiger arbeiten (`this-8`/`this-4`), nicht mit dem User-Zeiger `this` selbst, UND (b) eine Groesse verwenden, die sich aus "Groesse des eigenen Chunks" plus "ein Puffersegment" zusammensetzt. Das schraenkt legitime Verursacher stark ein: Ausserhalb der Allocator-Implementierung selbst hat normaler C++-Code fast nie einen Chunk-Zeiger (nur den User-Zeiger); ein Aufrufer muesste also entweder (i) ein Allocator-internes Konstrukt sein, oder (ii) selbst -4/-8-Zeigerarithmetik betreiben (z.B. ein eigenes Buchfuehrungsfeld direkt vor einem weitergereichten Zeiger).

**2. Realloc-Engine der Allocator-Vtable (Slot 4, `sub_00077B61` -> `sub_000778BC`) vollstaendig gelesen -- ausgeschlossen:** `sub_000778BC` (521 Bytes, 189 Instruktionen) ist eine vollstaendige, korrekte dlmalloc-artige `realloc()`-Implementierung: Groessenklassenberechnung, In-Place-Wachstum in den Top-Chunk, In-Place-Wachstum durch Verschmelzen mit einem freien Nachbar-Chunk (inkl. Freilisten-Unlink), Fallback auf Neuallokation (`sub_000774E6`) + `rep movsd`/`movsb`-Kopie der ALTEN Nutzdaten + Freigabe des alten Chunks. **Es gibt an keiner Stelle einen Zero-Fill des neu hinzugewonnenen Speichers** -- ein Standard-`realloc()` liefert dort undefinierten/alten Inhalt, keine Nullen. Zusaetzlich: Die konkrete Aufruferkette dieses Realloc-Slots wurde verfolgt (ueber den Dispatch `sub_0003EDCC`, der aber selbst 0 direkte Aufrufer im gesamten generierten Code hat, sowie ueber zwei generische Container-Resize-Hilfsfunktionen in `recomp_0018.c`/`recomp_0023.c`, insbesondere `sub_004518CC`, die an >30 Stellen quer durchs ganze Programm verwendet wird) -- keiner dieser Aufrufpfade liegt in der Naehe unseres Objekts (`0x40xxx`-Adressbereich) oder von `sub_00048B15`. **Kandidat "unser Objekt wird realloziert" ist damit sowohl vom Code-Inhalt als auch vom Call-Graph her ausgeschlossen.**

**3. Free-/Coalescing-Pfad (`sub_0007708F`) vollstaendig gelesen -- ausgeschlossen, mit wichtigem Korollar:** Die komplette Freiliste-Einfuege-/Verschmelzungslogik (Vorwaerts-/Rueckwaerts-Coalescing, Top-Chunk-Erweiterung, "Trim an OS zurueckgeben ab Schwelle 0x8000") wurde gelesen. **Sie enthaelt an keiner Stelle einen Zero-Fill der Nutzdaten** -- nur Zeiger-/Groessenfelder der Freiliste selbst werden geschrieben. **Wichtiges Korollar (neu):** Da `Free()` grundsaetzlich NIE die Nutzdaten nullt, kann ein verfruehter oder doppelter `Free()`-Aufruf auf unser Objekt -- selbst falls ein solcher Bug irgendwo existiert -- die beobachtete VOLLSTAENDIGE Nullung von `0x8070` Bytes NICHT allein erklaeren. Ein reiner Free()-Bug wuerde Altdaten/Muell hinterlassen, keine Nullen. **Der Schreiber muss also ein EXPLIZITER, gezielter Bulk-Zero-Aufruf sein (memset/ZeroMemory/`rep stosd`-Schleife), keine Nebenwirkung normaler (oder fehlerhafter) Allocator-Buchfuehrung.** Das schliesst eine ganze, bisher nicht explizit ausgeschlossene Erklaerungsklasse aus ("es ist einfach ein Doppel-Free").

**4. Vtable-Slot 0xC (`sub_0007702F`) geprueft -- ausgeschlossen:** Kleiner, bedingter, einmaliger Init-/Maintenance-Hook (prueft ein Flag, ruft ggf. `sub_00076F00` und betritt/verlaesst die Critical Section `0x6B27A0`). Keine Groessen- oder Adressparameter, kein Zero-Fill. Damit sind jetzt ALLE fuenf bekannten Allocator-Vtable-Slots (0=Allocate, 4=Realloc, 8=Free, 0xC=dieser Hook, 0x10=trivialer 1-5-Byte-Stub aus Update 194) einzeln gelesen und ausgeschlossen.

**5. `sub_00048B15` (Kandidat "c" aus Update 194) vollstaendig gelesen -- ausgeschlossen:** Diese Funktion (1032 Bytes/258 Instruktionen) ist der Level-/Session-Bootstrap: Sie installiert am Anfang selbst den Fatal-Handler-Zeiger (`MEM32(0x5E6E0C) = 0x3FCA5`, also `sub_0003FCA5` -- der Callback, an dem wir die Korruption beobachten) und orchestriert danach gut ein Dutzend Subsystem-Init-Aufrufe. Sie enthaelt genau EINEN `rep stosd`-Zero-Fill: `0x80` Bytes an der FIXEN globalen Adresse `0x64D638` (nahe den Allocator-Metadaten `0x64D29C`-`0x64D5EC`, aber selbst kein Allocator-Feld), unmittelbar gefolgt vom Befuellen dieses Bereichs mit ~16 Funktionszeigern verschiedenster Subsysteme (eine Registrierungstabelle, kein Bezug zu unserem Objekt). Ausserdem eine einzelne, unabhaengige `0x270`-Byte-Allokation ueber denselben Allocator-Singleton (`0x5B57F8`) -- unrelated Groesse. Ein Sub-Aufruf (`sub_001D47CF`, urspruenglich als moeglicher Audio-Init vermutet) wurde ebenfalls gelesen: Es ist ein String-/Konfigurations-Formatierungs-Hilfsprogramm, kein Audio- oder Pufferbezug. **Kandidat (c) aus Update 194 ("generischer memset-Aufruf direkt aus sub_00048B15") ist damit widerlegt, nicht nur unbeprueft.**

**Ehrlicher Zwischenstand:** Diese MAXIMUM-Runde hat eine grosse Ursachenklasse eindeutig ausgeschlossen (Allocator-eigene Buchfuehrung als Ursache, inkl. des Korollars "reiner Free-Bug reicht nicht") und einen konkreten Kandidaten aus Update 194 widerlegt (sub_00048B15 direkt). Der Suchraum ist dadurch geschaerft: Der Schreiber ist mit hoher Sicherheit ein expliziter Zero-Fill-Aufruf ausserhalb der Allocator-Kernroutinen, entweder (a) ueber einen rohen, separat gehaltenen Zeiger (weiterhin der aussichtsreichste offene Kandidat aus Update 194) oder (d, neu/praezisiert) ueber Code, der selbst eine Chunk-artige `ptr-4`/`ptr-8`-Buchfuehrung fuehrt (z.B. ein eigenes intrusives Header-Feld direkt vor einem weitergereichten Zeiger) und dabei eine kombinierte Groesse (eigener Datensatz + ein nachfolgender Puffer) falsch berechnet. Kandidat (b) (voellig andere Vtable-Familie) bleibt offen, aber niedrigere Prioritaet, da teuer zu pruefen ohne weiteren Anhaltspunkt.

**Model-Tier bleibt MAXIMUM** -- diese Runde hat den Suchraum aktiv verkleinert (eine ganze Mechanismus-Klasse ausgeschlossen, ein konkreter Kandidat widerlegt), erfuellt also das Fortschrittskriterium; kein Grund zur Deeskalation.

**Prozess-Hinweis:** Zu Beginn dieser Runde wurden alle 18 verwaisten Hintergrund-Watcher-Tasks aus vorherigen (teils laengst ueberholten) Experimenten gestoppt (sie warteten auf Ergebnisse bereits widerlegter bedingter-Breakpoint-Versuche bzw. auf einen laengst beendeten Prozess PID 48016). Der zuvor haengende `cdb.exe`-Prozess (dritter Versuch mit bedingtem Breakpoint, siehe Update 193) hatte sich zwischenzeitlich von selbst beendet; der zugehoerige `scda_recomp.exe` (PID 46460) laeuft weiter und befindet sich bereits im BAD-Zustand (Boundary-Tag/Vtable/Body-Feld alle `0`, per einzelnem `cdb -pv`-Poll bestaetigt) -- nutzbar fuer weiteres Nicht-invasives Polling, zeigt aber keinen frischen Uebergang mehr.
"""

worklog_text = """
## Update 195 (2026-09-20): Independent root-cause audit (MAXIMUM) -- allocator engine (Allocate/Realloc/Free/Slot 0xC) fully checked for zero-fill and ruled out; sub_00048B15 read and ruled out; new structural explanation for 0x8070

**This round's mandate:** instead of mechanically working through Update 194's candidate list, an independent audit was run: which mechanisms can even produce exactly `0x8070` bytes of zeroing, what do `0x8070`/`0x4070`/`0x4000` mean structurally, and which whole mechanism classes can be ruled out.

**1. Structural meaning of 0x8070/0x4070/0x4000 (refined):** the zeroed region starts exactly at the allocator chunk's own start (`this-8`, the boundary-tag/size field) and spans EXACTLY `our object's own chunk size (0x4070, including the 8-byte header) + one immediately-following ring-buffer segment (0x4000)`. Newly worked out: the end of the zeroing sits at `this+0x8068`, which arithmetically is EXACTLY `(this-8) + 0x4070 + 0x4000` -- the boundary is not a rounded estimate but a byte-exact sum of two concrete, known sizes. This implies the writer (a) operates on a CHUNK pointer (`this-8`/`this-4`), not the user pointer `this` itself, AND (b) uses a size composed of "this chunk's own size" plus "one buffer segment." This strongly constrains legitimate culprits: outside the allocator implementation itself, ordinary C++ code almost never holds a chunk pointer (only the user pointer); a caller would have to either (i) be allocator-internal code, or (ii) perform its own -4/-8 pointer arithmetic (e.g. its own bookkeeping field placed directly before a pointer it hands elsewhere).

**2. Allocator vtable Realloc engine (slot 4, `sub_00077B61` -> `sub_000778BC`) read in full -- ruled out:** `sub_000778BC` (521 bytes, 189 instructions) is a complete, correct dlmalloc-style `realloc()`: size-class computation, in-place growth into the top chunk, in-place growth by merging with a free neighboring chunk (including free-list unlink), fallback to fresh allocation (`sub_000774E6`) + `rep movsd`/`movsb` copy of the OLD payload + freeing the old chunk. **There is no zero-fill of the newly grown memory anywhere** -- a standard `realloc()` leaves that region as stale/undefined content, not zeroed. Additionally, this slot's actual call graph was traced (through the dispatcher `sub_0003EDCC`, which itself has zero direct callers anywhere in the generated code, and through two generic container-resize helpers in `recomp_0018.c`/`recomp_0023.c`, notably `sub_004518CC`, used at 30+ call sites scattered across the whole program) -- none of these call paths sit anywhere near our object (the `0x40xxx` address range) or near `sub_00048B15`. **The "our object gets reallocated" candidate is now ruled out both by code content and by call-graph evidence.**

**3. Free/coalescing path (`sub_0007708F`) read in full -- ruled out, with an important corollary:** the complete free-list insertion/coalescing logic (forward/backward coalescing, top-chunk extension, "trim back to the OS above an 0x8000 threshold") was read. **It contains no zero-fill of payload data anywhere** -- only the free list's own pointer/size fields are written. **Important new corollary:** since `Free()` never zeroes payload data at all, an erroneous or duplicate `Free()` call on our object -- even if such a bug exists somewhere -- CANNOT by itself explain the observed full zeroing of `0x8070` bytes. A pure Free() bug would leave stale garbage behind, not zeros. **The writer must therefore be an explicit, deliberate bulk-zero call (memset/ZeroMemory/a `rep stosd`-style loop), not a side effect of normal (or buggy) allocator bookkeeping.** This rules out a whole explanation class that had not been explicitly excluded before ("it's simply a double-free").

**4. Vtable slot 0xC (`sub_0007702F`) checked -- ruled out:** a small, conditional, one-time init/maintenance hook (checks a flag, conditionally calls `sub_00076F00` and enters/leaves the critical section `0x6B27A0`). No size or address parameters, no zero-fill. All five known allocator vtable slots (0=Allocate, 4=Realloc, 8=Free, 0xC=this hook, 0x10=the trivial 1-5-byte stub from Update 194) have now been individually read and ruled out.

**5. `sub_00048B15` (Update 194's candidate "c") read in full -- ruled out:** this function (1032 bytes/258 instructions) is the level/session bootstrap: at its very start it installs the fatal-handler pointer itself (`MEM32(0x5E6E0C) = 0x3FCA5`, i.e. `sub_0003FCA5` -- the callback where we observe the corruption) and then orchestrates roughly a dozen subsystem-init calls. It contains exactly one `rep stosd` zero-fill: `0x80` bytes at the FIXED global address `0x64D638` (near the allocator's own metadata block `0x64D29C`-`0x64D5EC`, but not itself an allocator field), immediately followed by populating that region with ~16 function pointers from various subsystems (a registration table, unrelated to our object). It also performs one independent `0x270`-byte allocation through the same allocator singleton (`0x5B57F8`) -- an unrelated size. One sub-call (`sub_001D47CF`, initially suspected as a possible audio-init call) was also read: it is a string/config-formatting helper, with no audio or buffer relevance. **Update 194's candidate (c) ("a generic memset call directly from sub_00048B15") is now disproven, not merely unexamined.**

**Honest interim status:** this MAXIMUM round definitively ruled out one large cause class (the allocator's own bookkeeping as the mechanism, including the corollary that "a pure Free bug" is insufficient) and disproved one concrete Update-194 candidate (sub_00048B15 directly). This sharpens the search: the writer is very likely an explicit zero-fill call outside the allocator's core routines, either (a) reached via a raw, separately-held pointer (still the most promising open candidate from Update 194) or (d, new/refined) via code that maintains its own chunk-like `ptr-4`/`ptr-8` bookkeeping (e.g. its own intrusive header field placed directly before a pointer it hands elsewhere) and miscomputes a combined size (its own record plus a following buffer). Candidate (b) (a completely different vtable family) remains open but lower priority, being expensive to check without a further lead.

**Model tier stays MAXIMUM** -- this round actively shrank the search space (ruled out a whole mechanism class, disproved a concrete candidate), meeting the progress bar; no reason to de-escalate.

**Process note:** at the start of this round, all 18 orphaned background watcher tasks from earlier (partly long-superseded) experiments were stopped (they were waiting on results from already-disproven conditional-breakpoint attempts, or on a long-dead process PID 48016). The previously-hung `cdb.exe` process (the third conditional-breakpoint attempt, see Update 193) had since exited on its own; the associated `scda_recomp.exe` (PID 46460) is still running and already in the BAD state (boundary tag/vtable/body field all `0`, confirmed via a single `cdb -pv` poll) -- usable for further non-invasive polling, but shows no fresh transition anymore.
"""

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** MAXIMUM (unveraendert seit Update 192)

**MODEL RECOMMENDATION:** In Codex: staerkste verfuegbare Analyse-Stufe. In
Claude: staerkstes verfuegbares Modell mit hoechster Reasoning-Tiefe.

**REASON:** Update 195 (unabhaengiger MAXIMUM-Audit) hat eine ganze
Mechanismus-Klasse (Allocator-eigene Buchfuehrung: Allocate/Realloc/Free/
Slot 0xC, alle fuenf Vtable-Slots einzeln gelesen) sowie einen konkreten
Kandidaten aus Update 194 (`sub_00048B15` direkt) definitiv ausgeschlossen
und dabei ein wichtiges Korollar bewiesen: Ein reiner Free()-Bug kann die
beobachtete VOLLSTAENDIGE Nullung nicht erklaeren, da Free() nie Nutzdaten
nullt -- der Schreiber muss ein expliziter Bulk-Zero-Aufruf sein. Der
Suchraum schrumpft weiter, das Fortschrittskriterium ist erfuellt; MAXIMUM
bleibt angemessen.

**Seit Update 194 (diese Sitzung, gleiche Modellstufe fortgesetzt):**
Update 195 hat einen unabhaengigen Root-Cause-Audit durchgefuehrt statt nur
die alte Kandidatenliste abzuarbeiten:
- **Strukturelle Erklaerung von 0x8070 praezisiert:** Ende der Nullung bei
  `this+0x8068` = exakt `(this-8) + 0x4070 (eigener Chunk) + 0x4000 (ein
  Ringpuffer-Segment)`. Der Schreiber muss also mit einem CHUNK-Zeiger
  (`this-8`/`this-4`) arbeiten, nicht mit dem User-Zeiger -- das schraenkt
  legitime Verursacher auf Allocator-internen Code ODER Code mit eigener
  `ptr-4`/`ptr-8`-Buchfuehrung ein.
- **Allocator-Vtable-Slot 4 (Realloc, `sub_00077B61`->`sub_000778BC`)
  vollstaendig gelesen:** komplette, korrekte dlmalloc-`realloc()`, KEIN
  Zero-Fill des neu gewachsenen Speichers; Call-Graph zeigt keine Naehe zu
  unserem Objekt. AUSGESCHLOSSEN.
- **Free-/Coalescing-Pfad (`sub_0007708F`) vollstaendig gelesen:** kein
  Payload-Zero-Fill irgendwo. **Wichtiges Korollar: Free() nullt nie
  Nutzdaten, also kann ein verfruehter/doppelter Free()-Aufruf allein die
  Nullung NICHT erklaeren** -- der Schreiber muss ein expliziter Bulk-Zero-
  Aufruf sein, keine Allocator-Nebenwirkung.
- **Vtable-Slot 0xC (`sub_0007702F`) geprueft:** trivialer bedingter
  Init-Hook, kein Zero-Fill. Damit sind ALLE fuenf bekannten Allocator-
  Vtable-Slots einzeln ausgeschlossen.
- **`sub_00048B15` (Update 194 Kandidat "c") vollstaendig gelesen:**
  installiert selbst den Fatal-Handler-Zeiger; enthaelt genau einen
  `rep stosd` (0x80 Bytes an fixer globaler Adresse `0x64D638`, eine
  Subsystem-Registrierungstabelle, nachweislich unabhaengig von unserem
  Objekt); ein Sub-Call (`sub_001D47CF`) ist ein String-/Konfig-Utility,
  kein Audio-/Pufferbezug. **Kandidat (c) damit WIDERLEGT, nicht nur
  unbeprueft.**

**Repository:** `upgrade/selective-upstream`, HEAD weiterhin `79e6c19`
(Frame-Pointer-Fix, behebt den Blocker nachweislich NICHT). Keine
geaenderten getrackten Dateien ausser `PROJECT_STATUS.md`/
`docs/SCDA_WORKLOG.md`/neuer `tools/debug/append_update195.py`; weiterhin
viele absichtlich erhaltene untracked Diagnoseartefakte.

**Aktueller Blocker (unveraendert):** Nach dem durch xemu belegten
korrekten Short-Read erreicht der Recomp-Lauf den originalen
Fatal-Callback `sub_0003FCA5`; am Callback ist das Ringpuffer-Unterobjekt
(`this`, Vtable `0x4F6940`, stabile Adresse `0x1135928`) vollstaendig
genullt (`~0x8070` Bytes ab dem Allocator-Boundary-Tag). Der konkrete
Schreiber ist weiterhin nicht gefunden, aber die Mechanismus-Klasse ist
jetzt praezise eingegrenzt: ein expliziter Bulk-Zero-Aufruf ausserhalb der
Allocator-Kernroutinen.

**Bewiesen (kumulativ, inkl. neu in Update 195):**
- Same-Process Vorher/Nachher-Vergleich bestaetigt sauberen GOOD->ZERO-
  Uebergang exakt am Objekt.
- Exakte Ausdehnung `~0x8070` Bytes = `0x4070` (eigener Chunk inkl.
  Header) + `0x4000` (ein Ringpuffer-Segment), Ende bytegenau bei
  `this+0x8068`.
- **NEU:** Der Schreiber MUSS mit einem Chunk-Zeiger (`this-8`) arbeiten,
  nicht dem User-Zeiger -- starke Einschraenkung auf Allocator-internen
  Code oder Code mit eigener Zeiger-Buchfuehrung.
- **NEU:** Free() nullt nachweislich nie Nutzdaten -- ein reiner Free-Bug
  reicht als Erklaerung nicht aus, der Schreiber muss explizit sein.

**Widerlegt (kumulativ, inkl. neu in Update 195):**
- Die zwei konkreten Destruktorpfade (`sub_000407F7`, `sub_00040B00`) --
  frueher, per Live-Trace.
- Die grossflaechige Arena-/Bulk-Reset-Lesart (Methodikfehler, Update 191).
- Die Vtable-Familie mit gemeinsamer Basisklasse (`0x4F69C0`) -- alle
  geteilten Methoden gelesen, kein Zero-Fill (Update 194).
- **NEU:** Allocator-Vtable Slot 4 (Realloc) -- vollstaendig gelesen, kein
  Zero-Fill, keine Call-Graph-Naehe.
- **NEU:** Allocator-Vtable Slot 8 (Free/Coalescing) -- vollstaendig
  gelesen, kein Payload-Zero-Fill irgendwo.
- **NEU:** Allocator-Vtable Slot 0xC -- trivialer Hook, kein Zero-Fill.
- **NEU:** `sub_00048B15` direkt (Update 194 Kandidat c) -- vollstaendig
  gelesen, einziger Zero-Fill ist eine unabhaengige, fixe globale Tabelle.

**Offen (Stand nach Update 195):** Der konkrete Schreiber ist weiterhin
nicht gefunden. Verbleibende Kandidaten:
1. **(a, weiterhin aussichtsreichster Kandidat)** Ein roher, separat
   gehaltener Zeiger auf unser Objekt (z.B. ein Ressourcen-/Level-Manager
   mit eigener Liste aktiver Streams), der das Objekt NICHT ueber die
   bekannte Vtable-Familie anspricht.
2. **(d, neu/praezisiert)** Code mit eigener `ptr-4`/`ptr-8`-intrusiver
   Buchfuehrung (z.B. ein eigenes Feld direkt vor einem weitergereichten
   Zeiger), der eine kombinierte Groesse (eigener Datensatz + ein
   nachfolgender Puffer) falsch berechnet und dabei explizit nullt.
3. **(b, niedrige Prioritaet)** Eine voellig andere, noch nicht
   untersuchte Vtable-Familie, die zufaellig denselben Speicherbereich
   referenziert -- teuer zu pruefen ohne weiteren Anhaltspunkt.

**Naechster Test (fuer MAXIMUM-Modell, Stand Update 195):**
1. Kandidat (a)/(d) verfolgen: nach Klassen/Managern suchen, die einen
   rohen Zeiger auf unser Objekt oder dessen `+0x405C`-Puffer halten,
   insbesondere solche mit einem eigenen Buchfuehrungsfeld direkt vor
   einem herausgegebenen Zeiger (analog zum in Update 195 gefundenen
   `sub_004518BE`-Muster: `Free(*(ptr-4))`, dort aber als generisches,
   nicht objektspezifisches Containermuster identifiziert und verworfen --
   nach einer AEHNLICHEN, aber objektspezifischen Variante suchen, die
   TATSAECHLICH mit unserem Ringpuffer in Verbindung steht).
2. Bedingte CDB-Breakpoints (`.if`/`.else` mit `.printf`/`kb`) weiterhin
   NICHT versuchen -- haengen sich reproduzierbar auf. Nur minimal-
   invasives `cdb -pv`-Polling verwenden.
3. Sobald der GOOD->BAD-Uebergang auf eine einzelne Funktion eingegrenzt
   ist: xemu-Vergleich fuer genau diese Funktion/diesen Call.

**Relevante neue Artefakte (Update 195):** `tools/debug/append_update195.py`
(dieses Update); keine neuen Live-Trace-Artefakte, reine Code-Audit-Runde.

**Laufende Prozesse / IN PROGRESS:** `scda_recomp.exe` PID `46460` laeuft
weiter, bereits im BAD-Zustand (Boundary-Tag/Vtable/Body-Feld bei
`0x1135920`/`0x1135928`/`0x113596C` alle `0`, per `cdb -pv`-Poll bestaetigt)
-- **nicht beenden**, nutzbar fuer weiteres Polling. Alle 18 verwaisten
Hintergrund-Watcher-Tasks aus fruaheren Sitzungsabschnitten wurden zu
Beginn dieser Runde gestoppt (sie warteten auf Ergebnisse bereits
widerlegter/toter Experimente). Keine haengenden `cdb.exe`-Prozesse mehr
aktiv. Upstream-Stand unveraendert seit letztem Eintrag (`origin/main` bei
`766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

with open(status_path, "ab") as f:
    f.write(status_text.replace("\n", "\r\n").encode("utf-8"))

with open(worklog_path, "rb") as f:
    raw = f.read()
raw += worklog_text.replace("\n", "\r\n").encode("utf-8")
with open(worklog_path, "wb") as f:
    f.write(raw)

print("done")
