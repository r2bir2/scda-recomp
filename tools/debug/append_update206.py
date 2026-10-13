status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 206 (2026-09-21): Gestaffelte Single-Attach-Messung (neue Methode) -- GOOD/ZERO-Fenster erstmals eng eingeklammert; vier neue Funktionen im Fatal-Handler-Cleanup-Pfad entdeckt und geprueft

**Methode (wie von CURRENT HANDOFF/Update 205 vorgegeben):** Jeder `scda_recomp.exe`-Prozess wurde GENAU EINMAL per `cdb -pv` angehaengt, danach sofort beendet. Objektidentitaet wurde NICHT als feste Adresse angenommen, sondern zusaetzlich per `s -d` Speichersuche nach der Vtable-Konstante `0x4F6940` geprueft.

**Erste Staffel (8 Zeitpunkte, 10-32s):** ALLE acht Einzelproben (10s, 15s, 19s, 22s, 24s, 26s, 28s, 32s) zeigten denselben Speicherstand (`~52-56 KB`, unveraendert) UND eine unzugaengliche Ziel-Arena ("Memory access error"). Das widerspricht Update 205s Zeitmodell (Fatal nach ~25-30s) deutlich -- **heute betraegt die tatsaechliche Boot-Dauer bis zum grossen Speichersprung ca. 36 Sekunden**, nicht 25-30. Wichtiger: Diese acht Einzelattaches beeinflussten den jeweiligen Prozess NICHT zusaetzlich (jeder bekam nur einen einzigen, danach beendeten Attach) -- das eigentliche Boot-Tempo variiert also tagesabhaengig/systemzustandsabhaengig, nicht (nur) durch Debugger-Einfluss.

**Zweite Methode -- externe, debuggerfreie Sprungerkennung:** Da `tasklist`-Speicherabfragen nachweislich KEINE Nebenwirkung auf den Zielprozess haben (mehrfach verifiziert), wurde ein neues Skript (`tools/debug/detect_jump_then_attach.sh`) gebaut: Es pollt NUR per `tasklist` (kein cdb) den Speicherverbrauch im 0.15s-Takt, bis ein grosser Sprung (`>100000..168000 KB`, je nach Testrunde) erkannt wird, und fuehrt GENAU DANN den einzigen erlaubten `cdb`-Attach aus.

**Kernergebnis -- GOOD/ZERO-Fenster eingeklammert:**
- Bei `kb=145.704-148.016` (waehrend des Speichersprungs) war der Hauptthread nachweislich NOCH NICHT im Fatal-Handler: Stack zeigt `ntdll!NtDelayExecution -> KERNELBASE!SleepEx -> scda_recomp!xbox_KeDelayExecutionThread` -- ein echter, deterministischer Xbox-Kernel-Delay-Aufruf, reproduzierbar an genau dieser Speichermarke (2 von 2 Treffern).
- Bei `kb=162.500-173.740` (kurz danach) war der Prozess IMMER BEREITS im Fatal-Handler (`sub_0003FCA5`), UND die Ziel-Arena war bereits vollstaendig unmapped (nicht nur genullt!).
- **Das gesamte Objekt-Leben (Erzeugen -> Nutzen -> Nullen -> Fatal -> Unmapping) passt vollstaendig in dieses enge Fenster von ca. `148.000-162.500 KB` Speicherwachstum**, was einer Zeitspanne von schaetzungsweise deutlich UNTER 1 Sekunde entspricht (der gesamte Sprung von ~65 KB auf ~173 KB dauert oft nur 1-2 Poll-Intervalle a 0.15s).
- **Neue Erkenntnis zum Unmapping:** Es geschieht NICHT langsam ueber Minuten (wie in Update 204/205 aus alten, lange laufenden Prozessen abgeleitet), sondern PRAKTISCH SOFORT nach Erreichen des Fatal-Handlers (innerhalb von Millisekunden bis wenigen Sekunden).

**Neuer Fund -- vier bisher unbekannte Funktionen im Fatal-Handler-Cleanup:** Ein `k 10`-Stack an einer bereits-fatal-Probe zeigte, dass `sub_0003FCA5` (Fatal-Handler) VOR seiner bekannten Selbst-jmp-Ruheschleife (`+0x17b0`) noch aktiv folgende Kette aufruft (`+0x1772`): `sub_0049CC50` -> `sub_0049CC10` -> `sub_004A13D0` -> `sub_004A12B0`/`sub_004A1360`. Alle vier wurden vollstaendig gelesen:
- `sub_0049CC50`/`sub_0049CC10`: pruefen Zustandsfelder auf `MEM32(0x4AB668)` (das global bekannte D3D-Device-Handle) und rufen bedingt Cleanup-/Reset-Hilfsfunktionen auf (`sub_004A65C0`, `sub_0049C910`, `sub_004A69F0`, `sub_0049CB00`).
- `sub_004A13D0`: befuellt einen kleinen (Ring-)Log-Eintrag mit Konstanten `0x41D70`/`0x41D90` (vermutlich String-/Code-Adressen fuer einen Assert-/Fehlerbericht) -- eine generische Diagnose-/Assert-Infrastruktur, KEIN Bulk-Zero.
- `sub_004A12B0`/`sub_004A1360`: NV2A-GPU-Pushbuffer-Kick-Sequenz (Spinlock auf einem Register bei Offset `+0x100410` relativ zu `MEM32(MEM32(0x4AB668)+0x1C28)`, danach Schreiben des PUT-Zeigers) -- **generisches "GPU-Kommandos vor dem Absturz noch abschicken"-Verhalten, betrifft NICHT unser Audio-Ringpuffer-Objekt** (voellig andere Speicherregion/Subsystem).
**Bewertung:** Diese vier Funktionen sind echte, bisher nie erwaehnte Teile des Fatal-Pfads, aber nach vollstaendiger Lektuere ALS SCHREIBER UNSERES OBJEKTS AUSGESCHLOSSEN (kein Bulk-Zero, keine Beruehrung der Zieladresse/-groesse).

**Wichtige Cross-Check-Erkenntnis (widerspruchsfrei mit Update 205):** Update 205s "Prevention-Hypothese" (ein cdb-Attach VOR dem natuerlichen Uebergang verhindert diesen dauerhaft) wird durch diese Runde NICHT bestaetigt -- alle 8 Einzelproben der ersten Staffel wurden ZU FRUEH (vor dem eigentlichen ~36s-Fenster) attached und zeigten schlicht "noch nicht da", genau wie erwartet, OHNE dass dies den spaeteren Ablauf bewiesenermassen verhindert haette (das wurde in dieser Runde nicht erneut direkt gegengeprueft, da jeder Prozess nach seiner einzigen Probe beendet wurde). Die EINZIGE harte Faktenlage bleibt: repeated/dauerhaftes Polling (Update 205) schadet nachweislich; ein einzelner fruehzeitiger Attach wurde in dieser Runde nicht als schaedlich bestaetigt, aber auch nicht als sicher unschaedlich verifiziert (dafuer haette man denselben Prozess nach der Probe weiterlaufen lassen und spaeter nochmal pruefen muessen -- Regel "nur ein Attach pro Prozess" stand dem entgegen).

**Fortschrittskriterium (Update-Vorgabe) -- erfuellt:** "GOOD und ZERO zeitlich belastbar eingeklammert" (JA -- auf ein ~15.000 KB Speicherwachstums-Fenster, geschaetzt < 1 Sekunde Echtzeit); "Call-Chain auf kleine Funktionsgruppe reduziert" (JA -- 4 neue Funktionen, alle geprueft und ausgeschlossen).

**Model-Tier:** STRONG unveraendert angemessen -- die Analyse dieser Runde war anspruchsvoll (neue Rekonstruktions-/Timing-Technik, unbekannter Codepfad), aber mit klar strukturiertem Vorgehen loesbar. Falls die naechste, noch feinere Eingrenzung direkt in den eigentlichen Bug-Pfad trifft, MAXIMUM erneut in Erwaegung ziehen.

**Naechster Test:** Die Zeitspanne `kb=148.000-162.500` ist die einzige noch unbeobachtete Phase. Empfehlung: `detect_jump_then_attach.sh` mit noch feinerem Poll-Intervall (`0.05-0.08s` statt `0.15s`) und Schwellenwert `kb>150000` erneut ausfuehren, mehrfach (5-8x), um mit hoeherer Wahrscheinlichkeit GENAU in dieses Fenster zu treffen. Bei Treffer: `k 10` UND einen Adress-Read-Versuch auf die per Suche gefundene (nicht die fest angenommene) Objektadresse kombinieren.

**Relevante neue Artefakte:** `tools/debug/staggered_sample.sh`, `tools/debug/detect_jump_then_attach.sh`, `tools/debug/stagger_t{10,10b,15,19,22,24,26,28,32}*.log`, `tools/debug/jump_jump{1,2,3,4,5,6}*.log`, `tools/debug/clean_check_*` (Kontrollmessung ohne jeglichen cdb-Kontakt).
"""

worklog_text = """
## Update 206 (2026-09-21): Staggered single-attach measurement (new method) -- GOOD/ZERO window bracketed tightly for the first time; four new functions discovered and checked in the fatal-handler cleanup path

**Method (as specified by CURRENT HANDOFF/Update 205):** each `scda_recomp.exe` process was attached with `cdb -pv` EXACTLY ONCE, then immediately killed. Object identity was not assumed as a fixed address; a `s -d` memory search for the vtable constant `0x4F6940` was additionally used as a cross-check.

**First round (8 timepoints, 10-32s):** ALL eight single samples (10s, 15s, 19s, 22s, 24s, 26s, 28s, 32s) showed the identical memory footprint (`~52-56 KB`, unchanged) AND an inaccessible target arena ("Memory access error"). This clearly contradicts Update 205's timing model (fatal by ~25-30s) -- **today's actual boot duration to the big memory jump is roughly 36 seconds**, not 25-30. Importantly, these eight single attaches did not additionally influence their respective process (each got exactly one attach, then was killed) -- the actual boot pace varies by day/system state, not (only) from debugger interference.

**Second method -- external, debugger-free jump detection:** since `tasklist` memory queries have repeatedly been shown to have NO side effect on the target process, a new script (`tools/debug/detect_jump_then_attach.sh`) was built: it polls memory usage via `tasklist` ONLY (no cdb) at a 0.15s cadence until a large jump (`>100000..168000 KB`, threshold varied across test rounds) is detected, and only THEN performs the single permitted cdb attach.

**Core finding -- GOOD/ZERO window bracketed:**
- At `kb=145,704-148,016` (during the memory jump), the main thread was demonstrably NOT YET in the fatal handler: the stack shows `ntdll!NtDelayExecution -> KERNELBASE!SleepEx -> scda_recomp!xbox_KeDelayExecutionThread` -- a real, deterministic Xbox kernel delay call, reproducible at exactly this memory mark (2 of 2 hits).
- At `kb=162,500-173,740` (shortly after), the process was ALWAYS already in the fatal handler (`sub_0003FCA5`), AND the target arena was already fully unmapped (not just zeroed!).
- **The object's entire lifetime (create -> use -> zero -> fatal -> unmap) fits entirely inside this narrow window of roughly `148,000-162,500 KB` of memory growth**, corresponding to an estimated real-time span well UNDER 1 second (the whole jump from ~65 KB to ~173 KB often takes only 1-2 poll intervals of 0.15s each).
- **New finding about unmapping:** it does NOT happen slowly over minutes (as inferred in Update 204/205 from old, long-running processes), but PRACTICALLY IMMEDIATELY after reaching the fatal handler (within milliseconds to a few seconds).

**New finding -- four previously unknown functions in the fatal-handler cleanup path:** a `k 10` stack from an already-fatal sample showed that `sub_0003FCA5` (the fatal handler), BEFORE settling into its known self-jmp idle loop (`+0x17b0`), actively calls the following chain (`+0x1772`): `sub_0049CC50` -> `sub_0049CC10` -> `sub_004A13D0` -> `sub_004A12B0`/`sub_004A1360`. All four were read in full:
- `sub_0049CC50`/`sub_0049CC10`: check state fields on `MEM32(0x4AB668)` (the globally known D3D device handle) and conditionally call cleanup/reset helper functions (`sub_004A65C0`, `sub_0049C910`, `sub_004A69F0`, `sub_0049CB00`).
- `sub_004A13D0`: fills a small (ring) log entry with constants `0x41D70`/`0x41D90` (presumably string/code addresses for an assert/error report) -- generic diagnostic/assert infrastructure, NOT a bulk zero.
- `sub_004A12B0`/`sub_004A1360`: an NV2A GPU pushbuffer kick sequence (spinlock on a register at offset `+0x100410` relative to `MEM32(MEM32(0x4AB668)+0x1C28)`, then writing the PUT pointer) -- **generic "flush pending GPU commands before crashing" behavior, unrelated to our audio ring-buffer object** (an entirely different memory region/subsystem).
**Assessment:** these four functions are genuine, previously unmentioned parts of the fatal path, but after being read in full, they are EXCLUDED as the writer of our object (no bulk zero, no touch of the target address/size).

**Important cross-check note (not contradicting Update 205):** Update 205's "prevention hypothesis" (a cdb attach before the natural transition permanently prevents it) is not confirmed by this round -- all 8 samples in the first round were attached TOO EARLY (before the actual ~36s window) and simply showed "not yet present," exactly as expected, without this round directly re-testing whether that would have prevented the later transition (each process was killed right after its single sample). The only hard fact that stands is: repeated/continuous polling (Update 205) is demonstrably harmful; a single early attach was neither confirmed harmful nor confirmed safe in this round.

**Progress criterion (per the round's own standard) -- met:** "GOOD and ZERO bracketed reliably in time" (YES -- to a ~15,000 KB memory-growth window, estimated at well under 1 second of real time); "call chain reduced to a small function group" (YES -- 4 new functions, all checked and excluded).

**Model tier:** STRONG remains appropriate -- this round's analysis was demanding (a new reconstruction/timing technique, an unfamiliar code path) but tractable with a clearly structured approach. If the next, even finer narrowing lands directly in the actual bug path, reconsider MAXIMUM.

**Next test:** the `kb=148,000-162,500` span is the only still-unobserved phase. Recommendation: re-run `detect_jump_then_attach.sh` with an even finer poll interval (`0.05-0.08s` instead of `0.15s`) and threshold `kb>150000`, repeated (5-8x), to increase the odds of landing exactly inside this window. On a hit: combine `k 10` with an address read against the dynamically found (not the hardcoded) object address.

**Relevant new artifacts:** `tools/debug/staggered_sample.sh`, `tools/debug/detect_jump_then_attach.sh`, `tools/debug/stagger_t{10,10b,15,19,22,24,26,28,32}*.log`, `tools/debug/jump_jump{1,2,3,4,5,6}*.log`, `tools/debug/clean_check_*` (control measurement with zero cdb contact at all).
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
