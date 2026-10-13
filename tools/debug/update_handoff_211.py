status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** MAXIMUM (seit Update 209, in Update 210/211 bestaetigt)

**MODEL RECOMMENDATION:** In Codex: staerkste verfuegbare Analyse-Stufe.
In Claude: staerkstes verfuegbares Modell mit hoechster Reasoning-Tiefe.

MODEL STATUS: ESCALATION RECOMMENDED
Tier: MAXIMUM
Reason: Update 211 hat die Root Cause auf EINEN einzigen Funktionsaufruf
eingegrenzt (das Ringpuffer-Unterobjekt ist beim Eintritt in eine konkrete
`sub_00040821`-Invocation valide und beim Verlassen von deren
`sub_000516B7`-Aufruf bereits vollstaendig genullt), aber den tatsaechlichen
Schreiber zu finden erfordert entweder native I/O-Wartepfad-Instrumentierung
oder eine neue, in dieser Sandbox bisher unzuverlaessige Cross-Thread-
Beobachtungsmethode (Hardware-Watchpoints scheiterten in Updates 188/189).

**Seit Update 210 -- Update 211 (diese Sitzung):**
Ringpuffer-Instrumentierung um vier Checkpoints in `sub_00040821`/
`sub_0004086E` erweitert (Callsite 7=Eintritt, 8=Eintritt via
Fortsetzungs-Einsprung, 9=nach `sub_000516B7`-Rueckkehr, 10=vor Dispatch).
Ergebnis ueber zwei frische Laeufe: die letzten 8 aufgezeichneten Events vor
dem Fatal-Dispatch zeigen chronologisch EXAKT: fuenf valide Callsite-7-
Eintraege (`this=0x1135928`, `handle=0x48000005`, `puffergroesse=0x24000`,
`ringpuffer=0x1139998`) -> **Callsite 9 DERSELBEN naechsten Invocation zeigt
alle drei Felder als 0** -> Callsite 10 (weiterhin 0) -> Callsite 5
(Dispatch). Da kein weiterer Callsite-7-Eintrag zwischen dem letzten validen
Zustand und Callsite 9 liegt, ist bewiesen: das Objekt ist beim EIGENEN
Eintritt dieser letzten Invocation valide und beim Verlassen von
`sub_000516B7` INNERHALB DERSELBEN Invocation bereits genullt. Da
`sub_00040821`s eigener Code (Updates 179-181) UND `sub_000516B7`s eigener
Koerper (Update 196) beide bereits vollstaendig gelesen und sauber sind,
zeigt dies auf einen echten Cross-Thread-Schreibzugriff waehrend der
blockierenden I/O-Wartezeit -- die urspruengliche, in Update 190 nur
TEILWEISE widerlegte Thread-Race-Hypothese aus Update 188.

**Repository:** `upgrade/selective-upstream`, Code-Commits `1e903d6` (Update
210, Instrumentierung + Doku) sowie diese Sitzung mit weiteren, noch nicht
committeten Instrumentierungs-Erweiterungen (Callsite 7-10) in den
generierten (gitignorten) `tools/recomp/output/scda_v060/gen/`-Dateien.

**Aktueller Blocker (praezisiert seit Update 210, jetzt auf einen einzigen
Funktionsaufruf eingegrenzt):** WAEHREND `sub_00040821`s `sub_000516B7`-
Aufruf (der blockierende Disc-Read) wird das Ringpuffer-Unterobjekt
(`this=0x1135928`, Vtable `0x4F6940`) komplett genullt. Der konkrete
Schreiber (vermutlich ein anderer Thread) ist weiterhin nicht gefunden.

**Bewiesen (kumulativ, inkl. neu in Update 211):** Alle Eintraege aus
Updates 109-210 unveraendert gueltig; **NEU: das Objekt ist beim Eintritt in
die fatale `sub_00040821`-Invocation valide UND beim Verlassen von deren
`sub_000516B7`-Aufruf (derselbe Aufruf) bereits vollstaendig genullt (211) --
die Nullung geschieht nachweislich waehrend genau dieses einen
Funktionsaufrufs, nicht davor und nicht in der Dispatch-Logik danach.**

**Widerlegt/relativiert (kumulativ, inkl. neu in Update 211):** Update 190s
Ausschluss von `sub_00040B00`/`sub_000407F7` bleibt gueltig, aber die
uebergeordnete Schlussfolgerung "Thread-Race-Hypothese ausgeschlossen"
(Update 190) war zu weitgehend formuliert -- **Update 211 zeigt, dass ein
Cross-Thread-Mechanismus (nur eben nicht ueber diese zwei spezifischen
Funktionen) weiterhin die plausibelste Erklaerung ist.**

**Offen (Stand nach Update 211), priorisiert fuer MAXIMUM:**
1. **Der konkrete Schreiber innerhalb/waehrend `sub_000516B7`:** geschieht
   die Nullung in `sub_000516B7`s eigenem uebersetzten Setup (single-
   threaded, waere dann doch ein Recompiler-Bug trotz Update-196-"sauber"-
   Befund an anderer Stelle) oder tatsaechlich cross-thread waehrend der
   nativen I/O-Wartezeit? Noch nicht unterschieden.
2. Falls Cross-Thread: WELCHER Thread, WELCHER Code-Pfad schreibt die
   Nullen? Keine der bisher gepruesten Funktionen (`sub_00040B00`,
   `sub_000407F7`) ist verantwortlich.
3. Alles Weitere aus Update 210 Punkt 3 (ABI-Check-Fund bei Guest-VA
   `0x49CC50`) bleibt offen, niedrigere Prioritaet.
4. Restliche niedrigprioritaere Punkte aus Updates 194/195/204/205
   unveraendert.

**Naechster Test (Prioritaet, fuer MAXIMUM):**
1. Einen Checkpoint an `sub_000516B7`s eigenem Eintritt hinzufuegen (gleiche
   Ringpuffer-Technik), um zu bestimmen, ob das Objekt zu diesem Zeitpunkt
   noch valide ist -- unterscheidet "Nullung in `sub_000516B7`s eigenem
   Setup-Code" vs. "Nullung waehrend des eigentlichen blockierenden Wartens
   tiefer im nativen I/O-Layer".
2. Falls Cross-Thread bestaetigt: native Kernel-Bridge-Wartefunktion
   instrumentieren, um den aktiven Thread beim Warte-Beginn/-Ende
   festzuhalten.
3. Alternative, potenziell zuverlaessigere Methode statt Hardware-
   Watchpoints (die in 188/189 scheiterten): `VirtualProtect` auf die
   Objekt-Seite fuer die Dauer des `sub_000516B7`-Aufrufs, um den Schreiber
   per Access-Violation-Fault abzufangen -- haengt nicht von cdb-Debug-
   Registern ab.
4. Weiterhin: kein Dauerpolling, keine bedingten Breakpoints ueber laengere
   Zeit, keine Wiederholung der 188/189-Hardware-Watchpoint-Methode ohne
   neue Idee.

**Instrumentierung (WICHTIG, temporaer, siehe Updates 210/211):** neben den
vier Update-210-Markern jetzt zusaetzlich vier weitere
(`SCDA-DISC-ERR-INSTRUMENTATION`, Callsite 7/8/9/10) in
`tools/recomp/output/scda_v060/gen/recomp_0001.c` (gitignoriert, nicht
committet). Ringpuffer-Infrastruktur in `projects/scda/src/recomp_manual.c`
(committet in `1e903d6`) unveraendert. **Weiterhin rein diagnostisch, darf
nicht als dauerhafter Fix bestehen bleiben** -- bleibt bestehen, da
Naechster Test #1 dieselbe Infrastruktur direkt weiterverwendet.

**Relevante neue Artefakte:** `tools/debug/append_update211.py`,
`tools/debug/append_update211_status.py`, `tools/debug/update_handoff_211.py`.

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen mehr (PID
45420 und 32956, beide in dieser Runde gestartet, wurden nach Extraktion
ihrer Diagnosedaten sauber per `taskkill` beendet). Upstream-Stand
unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
