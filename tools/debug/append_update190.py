status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 190 (2026-09-18): KORREKTUR zu Update 188 -- weder der volle Destruktor noch die einfache Schliessfunktion feuern jemals; die Nullung erfolgt ohne jeden bekannten Objekt-Lifecycle-Aufruf

**Test 1:** Breakpoint ausschliesslich auf `sub_00040B00` (der volle "scalar deleting destructor" aus Update 188) plus `sub_0003FCA5`, minimaler Overhead (kein zusaetzliches Tracking), ueber einen kompletten Boot-Lauf bis zum Fatal-Callback. **Ergebnis: `sub_00040B00` feuert kein einziges Mal.** Breakpoint-Index 1 (`sub_0003FCA5`) wird direkt erreicht.

**Test 2:** Gleicher Aufbau, diesmal mit `sub_000407F7` (die einfachere "schliessen ohne Objekt freizugeben"-Funktion, die separat von zwei anderen Stellen aus direkt aufgerufen wird). **Ergebnis: ebenfalls kein einziger Treffer.**

**Das widerlegt die Kernaussage von Update 188 vollstaendig:** Es gibt keinen Hinweis darauf, dass irgendein bekannter Konstruktor-/Destruktor-Pfad fuer dieses Unterobjekt die Nullung verursacht -- weder auf Thread 0 noch (da beide Breakpoints ueber ALLE Threads hinweg gelten) auf irgendeinem anderen Thread. Die Thread-Race-Hypothese aus Update 188 ist damit als Erklaerung ausgeschlossen, nicht nur unbewiesen.

**Zusaetzlicher Zwischenbefund (aus einem laenger laufenden Versuch mit paralleler `sub_00040821`-Verfolgung):** Das Unterobjekt (`0x1135928`) wird ueber HUNDERTE bis ueber 1700 aufeinanderfolgende, vollstaendig normale Refill-Aufrufe hinweg konsistent auf demselben Thread verwendet, bevor der Fatal-Callback ueberhaupt erreicht wird -- die vorherige Annahme, der Crash trete "kurz nach" dem ersten Short-Read auf, gilt offenbar nur fuer EINEN Teil der beobachteten Laeufe; in mindestens einem Lauf lief der Stream noch sehr lange normal weiter.

**Neue Arbeitshypothese fuer die naechste Sitzung:** Da keine objekt-spezifische Aufraeumfunktion die Nullung erklaert, deutet dies eher auf einen **pauschalen Speicher-Reset** (z.B. ein Arena-/Pool-Allokator, der bei einem Level- oder Frame-Reset einen ganzen Speicherblock ohne Aufruf einzelner Destruktoren nullt) oder auf **Heap-Korruption durch unabhaengigen Code** (ein Off-by-one/Buffer-Overflow anderswo, das zufaellig genau diesen Speicherbereich trifft) hin. Naechster sinnvoller Schritt: Hardware-Watchpoint erneut versuchen, aber diesmal OHNE vorherigen Fehlversuch in derselben Sitzung (moeglicherweise war das DR7-Register durch die vorherige `cdb`-Sitzung in einem inkonsistenten Zustand) -- oder alternativ die Adresse ueber mehrere kurze, gezielte Snapshots (nicht-invasiver Attach alle paar Sekunden) beobachten, um den ungefaehren Zeitpunkt der Nullung einzugrenzen, statt eine durchgehende Live-Verfolgung zu erzwingen.
"""

worklog_text = """
## Update 190 (2026-09-18): CORRECTION to Update 188 -- neither the full destructor nor the simple close function ever fires; the zeroing happens without any known object-lifecycle call

**Test 1:** breakpoint exclusively on `sub_00040B00` (the full "scalar deleting destructor" from Update 188) plus `sub_0003FCA5`, minimal overhead (no additional tracking), through a complete boot run to the fatal callback. **Result: `sub_00040B00` never fires once.** Breakpoint index 1 (`sub_0003FCA5`) is reached directly.

**Test 2:** same setup, this time with `sub_000407F7` (the simpler "close without freeing the object" function, called directly from two other sites separately). **Result: also zero hits.**

**This fully disproves the core claim of Update 188:** there is no evidence that any known constructor/destructor path for this subobject causes the zeroing -- not on thread 0, and (since both breakpoints apply across all threads) not on any other thread either. The Update 188 thread-race hypothesis is therefore ruled out as an explanation, not merely unproven.

**Additional interim finding (from a longer-running attempt with parallel `sub_00040821` tracking):** the subobject (`0x1135928`) is used consistently on the same thread across hundreds to over 1700 consecutive, entirely normal refill calls before the fatal callback is even reached -- the earlier assumption that the crash occurs "shortly after" the first short read apparently only held for some of the observed runs; in at least one run the stream kept operating normally for a much longer stretch.

**New working hypothesis for the next session:** since no object-specific cleanup function explains the zeroing, this points more toward a **blanket memory reset** (e.g. an arena/pool allocator that zeroes an entire block on a level or frame reset without calling individual destructors) or **heap corruption from unrelated code** (an off-by-one/buffer overflow elsewhere that happens to land on exactly this memory region). A reasonable next step: retry the hardware watchpoint, but without a prior failed attempt in the same session (the DR7 debug register may have been left in an inconsistent state by the earlier cdb session) -- or alternatively, observe the address via several short, targeted snapshots (non-invasive attach every few seconds) to narrow down roughly when the zeroing happens, instead of forcing one continuous live trace.
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
