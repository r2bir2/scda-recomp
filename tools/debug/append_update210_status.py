path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

text = """

## Update 210 (2026-09-21): One-Shot-Instrumentierung (per expliziter Anweisung) gebaut -- beweist, dass sub_0003FCA5 NICHT ueber eine der drei Update-208-Aufrufstellen erreicht wird, sondern ueber den indirekten Callback bei MEM32(0x5E6E0C); das bestaetigt (entdeckt nicht neu) den bereits in Updates 109/117/173-182/186/187 dokumentierten Mechanismus und schliesst die Schleife zurueck zur weiterhin offenen Update-190-Frage

**Befolgte Anweisung:** den Blocker nicht ueber weiteres CDB-Polling loesen;
stattdessen eine minimal-invasive One-Shot-Instrumentierung an den drei
bekannten `sub_0003FCA5`-Aufrufstellen bauen, die Callsite-ID/Reader-Zeiger/
Status/GetLastError/Thread-ID rein im Host-Speicher ablegt (kein Hot-Path-
Output), und den Snapshot genau einmal beim tatsaechlichen Eintritt in
`sub_0003FCA5` ausgeben. Genau so umgesetzt (Ringpuffer + record()/dump_once()
in `recomp_manual.c`, Deklarationen in `recomp_types.h`, vier
Aufrufstellen-Injektionen in `recomp_0001.c` -- siehe die
`SCDA-DISC-ERR-INSTRUMENTATION`-Marker). Neu gebaut, nachdem der Nutzer
explizit zugestimmt hatte, das lange geschuetzte `cdb.exe`/`scda_recomp.exe`-
Paar zu schliessen, das die exe-Datei zum Schreiben gesperrt hielt (bestaetigt
vollstaendig abgestuerzt, kein verbleibender diagnostischer Wert).

**Ergebnis 1 -- ein voellig ungestoerter Lauf reproduziert NICHT innerhalb des
60-fachen des bisher angenommenen Fensters:** frisch gebaute, instrumentierte
exe ganz ohne Debugger gestartet. Nach **36 Minuten** Wall-Clock-Zeit (gegen
das von Update 206 auf ~30-36s eingegrenzte Fenster) war das Diagnose-Log
weiterhin komplett leer, der Prozess lief weiter, einen CPU-Kern auslastend
(`SetProcessAffinityMask(..., 1)`, bereits seit vor Update 190 bestehender,
unveraenderter Code aus einer aelteren, unabhaengigen Untersuchung -- erklaert
die Diskrepanz nicht von allein). Eine zweite frische Instanz, bei t=50s
weiterhin ohne Eintrag geprueft, danach mit GENAU EINEM nicht-invasiven
`cdb -pv -p <PID> -c "q"`-Attach-und-sofort-quit (entspricht Update 206s
eigener etablierter Minimalmethode, kein "Polling"), war zum Zeitpunkt dieses
einen Attaches bereits in einer engen `jmp $`-Selbstschleife eingefroren.

**Ergebnis 2 -- der Einfrierpunkt liegt tatsaechlich innerhalb von
`sub_0003FCA5` selbst, kein Symbolaufloesungs-Artefakt:** die live Host-Adresse
gegen die vom Linker selbst sortierte "Publics by Value"-Map-Datei-Sektion
gegengeprueft (unabhaengig von cdbs PDB, die vor einem Checksummen-Mismatch
warnte): die eingefrorene Instruktion liegt bei RVA `0x335770`, strikt
zwischen `sub_0003FCA5` (`0x333fb0`) und dem naechsten Symbol `sub_0003FCDD`
(`0x335d70`) -- bestaetigt tatsaechlich innerhalb von `sub_0003FCA5`s eigenem
kompiliertem Koerper, Offset `+0x17c0`. Disassemblierung dort zeigt
`mov ecx, 0x49CC50; mov edx, r14d; call recomp_abi_violation_log; jmp $`.
`RECOMP_ABI_CHECK` ist eine bereits bestehende, aktuell aktivierte Diagnose
fuer dieses Target (`projects/scda/CMakeLists.txt`, aus einer aelteren,
unabhaengigen ABI-Clobber-Untersuchung, docs Zeile ~8745), die (einmal pro
eindeutiger VA, nach `stderr`) protokolliert, wann immer ein
`RECOMP_ABI_CALL`-gewrappter Aufruf mit veraendertem `g_ebx`/`g_esi`/`g_edi`
oder zu niedrigem `g_esp` zurueckkehrt. Das heisst: waehrend `sub_0003FCA5`
seinen eigenen Koerper ausfuehrt, macht es einen inneren Aufruf zur Guest-VA
`0x49CC50`, dessen uebersetzter Epilog die erwarteten Register/den Stack nicht
vollstaendig wiederherstellt -- eine echte, bisher nicht gemeldete ABI-
Verletzung, nicht durch unsere Instrumentierung verursacht (unser injizierter
Aufruf liegt davor und beruehrt `g_ebx/g_esi/g_edi/g_esp` nicht). Die
Selbstschleife selbst ist nicht neu: es ist dieselbe bereits in Updates
179-182 beschriebene "sub_0003FCA5-jmp $-Schleife" als Endzustand dieser
Funktion (kehrt nie zurueck) in dieser Umgebung.

**Ergebnis 3 -- direkter Beweis, dass keine der drei Update-208-Aufrufstellen
in diesem Lauf feuerte:** das Auslesen der Globals des eingefrorenen Prozesses
(ein einzelner statischer Speicher-Read, sicher, da das Ziel nachweislich
eingefroren/unveraendert ist -- ueber zwei Reads eine Minute auseinander als
unveraendert bestaetigt) zeigte `g_scda_disc_err_dumped=1` (also wurde
`sub_0003FCA5` tatsaechlich betreten und seine erste Anweisung, unser
Dump-Aufruf, lief wirklich), aber `g_scda_disc_err_ring_count=0` (also hat
KEINE der drei instrumentierten Aufrufstellen -- `sub_000356F1`,
`sub_00038BD5`, `sub_00038F96` -- jemals einen Treffer aufgezeichnet). Da
unser Record-Aufruf unbedingt direkt vor dem jeweils bestehenden Aufruf zu
`sub_0003FCA5` an jeder Stelle sitzt, ist das schluessig: der Eintritt in
`sub_0003FCA5` in diesem Lauf kam weder ueber den direkten
`sub_00038A43`-Status-4-Pfad (Updates 207-209) noch ueber `sub_000356F1`.

**Ergebnis 4 -- dieselbe Technik auf die Indirect-Call-Dispatch-Makros
erweitert und einen exakten Treffer erhalten:** `sub_0003FCA5` hat auch einen
`recomp_dispatch.c`-Tabelleneintrag, kann also ueber `RECOMP_ICALL`/
`RECOMP_ICALL_SAFE`/`RECOMP_ITAIL` (zur Laufzeit per Guest-VA nachgeschlagen)
erreicht werden -- ein Pfad, den Update 208s statische Aufrufstellen-Auflistung
per Grep nach woertlichem `RECOMP_ABI_CALL(0x0003FCA5u, ...)`-Text nicht haette
finden koennen. Je einen abgesicherten Record-Aufruf in alle drei Makros in
der generierten `recomp_types.h` eingefuegt
(`if (_va == 0x0003FCA5u) scda_disc_error_record(...)`), neu gebaut, erneut
gestartet. Eine frische Instanz fror an derselben `sub_0003FCA5+0x17c0`-Stelle
ein, und diesmal enthielt der Ringpuffer genau einen Eintrag: `callsite_id=5`
(`RECOMP_ICALL_SAFE`), `a=g_esp` beim Aufruf, `b=MEM32(g_esp)` (die vom Makro
selbst gepushte Guest-Ruecksprungadresse) `=0x00040992`.

**Ergebnis 5 -- zurueckverfolgt zu einer Aufrufstelle, die selbst bereits
laengst dokumentiert ist, nicht neu:** Guest-`0x40992` ist die
Ruecksprungadresse von `RECOMP_ICALL_SAFE(eax, ...)` bei Guest-`0x40990`,
innerhalb von `sub_00040821` (`recomp_0001.c:68556`, Kategorie
`game_vtable`) -- das ist die Ringpuffer-Nachfuellfunktion, bereits vollstaendig
in Updates 179-181 gelesen. Direktes Lesen des Quellcodes (kein cdb noetig)
zeigt: nachdem `sub_000516B7` `0` (Erfolg) zurueckliefert UND
`MEM32(0x5E6E0C) != 0` ist, ruft der Code unbedingt ueber
`MEM32(0x5E6E0C)` -- **das ist genau der globale "Fatal-Callback"-Zeiger,
bereits in Update 181 gefunden und per Hardware-Watchpoint geprueft**, der
im gesamten damaligen Lauf genau EINEN Schreibzugriff zeigte:
`sub_00048B15+0x185` (`recomp_0002.c:15018`, `MEM32(0x5E6E0C) = 0x3FCA5;`),
unbedingt, ganz am Anfang dieser Funktion, vor jeder I/O. `sub_00048B15`
selbst wurde bereits (Updates 109/117) zurueckverfolgt: einmal pro Boot, mit
drei Null-Argumenten aufgerufen, von
`sub_00054722 <- sub_00054701/sub_00054720 <- sub_000546FB` (letztere ein
indirekter Tail-Jump ueber einen weiteren globalen Slot, `MEM32(0x4F5208)`)
-- alles deckungsgleich mit dem, was diese Sitzung unabhaengig aus dem
aktuellen Build per Quellcode-Lesen neu hergeleitet hat, ohne einen ueber
Updates 109/117/173-182 hinausgehenden neuen Aufrufer.

**Was das tatsaechlich klaert vs. was nicht:** dies schliesst die Schleife
zurueck zu Update 190, nicht darueber hinaus. Die Drei-Aufrufstellen-Rahmung
aus Update 208 (direkter `sub_00038A43`-Status-4 -> `sub_0003FCA5`) war
plausibel, aber fuer diesen Build/Lauf NICHT der tatsaechlich genommene Pfad
-- der lebende Mechanismus ist der viel aeltere, bereits verstandene, einmal
scharf geschaltete globale Callback. Die eigentlich offene Frage ist
unveraendert seit Update 190: `sub_00040821` dispatcht ueber
`MEM32(0x5E6E0C)` erst NACHDEM sein eigener vorangehender
`sub_000516B7`-Aufruf erfolgreich war, an einem Ringpuffer-Unterobjekt, das
Updates 187/190 bereits als vollstaendig genullt/nie initialisiert gezeigt
haben, ohne jeden bekannten Konstruktor-/Destruktor-/Allocator-Aufruf. Diese
Sitzung hat diese konkrete Nullungs-Frage nicht neu reproduziert oder
vorangetrieben; sie hat (per cdb-unabhaengiger, quellcode-basierter Methode,
gemaess der expliziten Anweisung dieser Runde) bestaetigt, dass der in
Updates 109-190 beschriebene nachgelagerte Mechanismus im aktuellen Build
weiterhin genau so feuert, und zusaetzlich einen moeglicherweise neuen,
bisher ungeklaerten Datenpunkt markiert: eine echte
`RECOMP_ABI_CHECK`-Verletzung bei einem inneren Aufruf innerhalb von
`sub_0003FCA5` selbst (Guest-VA `0x49CC50`), noch nicht gegen die Notizen der
aelteren Sitzungen abgeglichen.

**Modell-Stufe:** unveraendert MAXIMUM (Update 209) -- die zugrundeliegende
offene Frage (was laesst das Ringpuffer-Unterobjekt genullt/nicht
initialisiert zurueck, bevor `sub_00040821` es nutzt) ist genauso schwer wie
am Ende von Update 190, jetzt als weiterhin aktuell bestaetigt statt geloest.

MODEL STATUS: ESCALATION RECOMMENDED
Tier: MAXIMUM
Reason: die eigentliche Root-Cause-Frage (Update 190) bleibt offen trotz
eines unabhaengig neu hergeleiteten, vollstaendig konsistenten Mehrschritt-
Mechanismus ueber zwei weit auseinanderliegende Untersuchungsstraenge hinweg
(Updates 109-190 und 205-209); ihre Klaerung braucht dieselbe Art nicht-
trivialer, dateiuebergreifender, thread-uebergreifender Analyse, die bereits
zwei lange Untersuchungsstraenge ohne Aufloesung verbraucht hat.

**Naechster Test (fuer MAXIMUM), ersetzt Update 209s Punkte 1-2 (jetzt
beantwortet):**
1. Die in dieser Runde bewaehrte, zuverlaessige Technik (Aufzeichnung in einen
   Host-Ringpuffer an der interessierenden Stelle; einmaliges Auslesen per
   statischem Speicher-Read, NACHDEM das Ziel nachweislich eingefroren ist --
   kein Live-CDB-Polling, keine Timing-Abhaengigkeit) auf `sub_00040821`s
   eigenen Eintrittspunkt anwenden: `ecx` ("this") und die Schluesselfelder
   des Ringpuffer-Unterobjekts (`+0x3C`, `+0x4058`, `+0x405C`) bei JEDEM
   Aufruf aufzeichnen, nur die letzten paar im Ring behalten, damit der genaue
   Aufruf, bei dem das Unterobjekt erstmals genullt zurueckgelesen wird,
   ohne fragilen Live-Watchpoint oder Attach-Timing identifiziert werden kann.
2. Sobald der genaue Aufruf isoliert ist, dieselbe nicht-invasive Methode, die
   die Frage dieser Runde beantwortet hat (Map-Datei-Abgleich, Quellcode-
   Lesen, kein Vertrauen in cdbs PDB/Symbolnamen), auf das anwenden, was
   diesem konkreten Aufruf vorausgeht -- statt den historischen Update-
   188/189-Hardware-Watchpoint-Ansatz zu wiederholen, der sich in dieser
   Sandbox wiederholt als unzuverlaessig erwiesen hat.
3. Separat, niedrigere Prioritaet: pruefen, ob die `RECOMP_ABI_CHECK`-
   Verletzung bei Guest-VA `0x49CC50` (ausgeloest innerhalb von
   `sub_0003FCA5`, Ergebnis 2 oben) bereits ein bekannter, akzeptierter Fund
   aus der aelteren ABI-Clobber-Untersuchung (docs Zeile ~8745) ist oder eine
   tatsaechlich neue Spur.

**Instrumentierungsstatus (gemaess expliziter Standing Instruction --
"Instrumentierung ist diagnostisch und darf nicht als Fix bestehen
bleiben"):** alles davon (die vier `recomp_0001.c`-Aufrufstellen-Marker, die
drei `recomp_types.h`-Makro-Checks und der `recomp_manual.c`-Ringpuffer-/
Record-/Dump-Once-Block) bleibt vorerst bestehen, weil die von dieser
Anweisung geforderte Rueckwaertsverfolgung weiterhin aktiv ist und genau
dieser Mechanismus der schnellste Weg ist, sie fortzusetzen (siehe Naechster
Test Nr. 1 oben, der ihn wiederverwendet). Es ist temporaer und wird
zurueckgesetzt, sobald der tatsaechliche Nullungs-Schreiber (die echte,
weiterhin offene Root Cause) gefunden ist oder diese konkrete Untersuchung
abgeschlossen wird -- nicht als dauerhafter Fix belassen.
"""

with open(path, "wb") as f:
    f.write(raw + text.replace("\n", "\r\n").encode("utf-8"))

print("done, new size", len(raw) + len(text.replace("\n", "\r\n").encode("utf-8")))
