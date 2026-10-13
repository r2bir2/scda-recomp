path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

text = """

## Update 211 (2026-09-21): Root-Cause-Fenster auf EINEN einzigen Funktionsaufruf eingegrenzt -- Ringpuffer-Unterobjekt nachweislich VALIDE bei sub_00040821s Eintritt und nachweislich GENULLT unmittelbar nach Rueckkehr aus sub_000516B7, innerhalb derselben Invocation

**Methode:** die Update-210-Ringpuffer-Instrumentierung (weiterhin rein
diagnostisch, weiterhin null CDB-Polling) um drei weitere Checkpoints
erweitert, alle in den generierten `sub_00040821`/`sub_0004086E`-Koerpern:
Callsite 7 an der allerersten Anweisung von `sub_00040821` (`ecx`="this"
frisch vom Aufrufer, bevor sonst etwas laeuft), Callsite 8 an der
aequivalenten Stelle in `sub_0004086E` (liest `ebx`, da dieser Einsprungpunkt
eine Funktions-Mitte-Fortsetzung ist, die bereits ein von ihrem eigenen
Aufrufer gesetztes `ebx`="this" erwartet -- defensiv hinzugefuegt, nie
tatsaechlich beobachtet), Callsite 9 unmittelbar nach Rueckkehr aus dem
`sub_000516B7`-Aufruf (vor jedem Erfolgs-/Fehlerzweig), und Callsite 10
unmittelbar vor dem indirekten Dispatch ueber `MEM32(0x5E6E0C)`. Alle vier
lesen dieselben drei bereits in Updates 187/188 etablierten Header-Felder
(`+0x3C` Dateihandle, `+0x4058` Puffergroesse, `+0x405C` Ringpuffer-Zeiger).
Zwei frische, voellig ungestoerte Laeufe; jeder eingefrorene Zustand genau
einmal per einzelnem statischem Speicher-Read ausgelesen (Ringpuffer
nachweislich unveraendert ueber wiederholte Reads), kein Live-Attach, kein
Watchpoint.

**Ergebnis:** ueber ~3,03 Millionen `sub_00040821`-Aufrufe insgesamt in einem
einzigen Boot (`g_scda_disc_err_ring_index` zurueckgelesen als `0x2e53d3`)
war das letzte 8-Eintrag-Fenster vor dem Fatal-Dispatch, in exakter
chronologischer Reihenfolge:
1. Fuenf aufeinanderfolgende Callsite-7-Eintraege (fuenf gewoehnliche
   vorangehende Aufrufe von `sub_00040821`), **alle vollstaendig valide**:
   `this=0x01135928`, `handle=0x48000005`, `puffergroesse=0x00024000`,
   `ringpuffer_zeiger=0x01139998` -- identisch ueber alle fuenf, alle auf
   Thread `0x343c`.
2. Callsite 9 -- **unmittelbar nach Rueckkehr aus `sub_000516B7`, innerhalb
   der ALLERNAECHSTEN `sub_00040821`-Invocation nach den fuenf validen oben
   (kein Callsite-7-Eintrag dazwischen)** -- gleiches `this=0x01135928`,
   gleicher Thread `0x343c`, aber `handle=0`, `puffergroesse=0`,
   `ringpuffer_zeiger=0` -- **vollstaendig genullt.**
3. Callsite 10, unmittelbar vor dem Dispatch: weiterhin alles Null
   (konsistent, nichts entnullt es dazwischen).
4. Callsite 5, der Dispatch selbst, bestaetigt Ruecksprungadresse `0x40992`
   (wie Update 210).

**Schlussfolgerung -- das exakte Fenster ist jetzt bekannt:** das Objekt ist
nachweislich intakt beim Eintritt DIESER Invocation (Callsite 7, valide,
unmittelbar vor Callsite 9 ohne einen weiteren `sub_00040821`-Aufruf
dazwischen) und nachweislich vollstaendig genullt zum Zeitpunkt der
Rueckkehr aus `sub_000516B7` INNERHALB DERSELBEN Invocation (Callsite 9).
`sub_00040821`s eigener Code zwischen seinem Eintritt und dem
`sub_000516B7`-Aufruf wurde bereits in Updates 179-181 vollstaendig gelesen
und schreibt nichts in diese Felder. Das bedeutet: die Nullung geschieht
**waehrend des `sub_000516B7`-Aufrufs selbst** -- also waehrend der
tatsaechlichen blockierenden Disc-Read-Operation (bereits in Update 196 als
`sub_000516B7` -> `xbox_NtReadFile` -> `sub_00052A85`/`sub_00052A57`
zurueckverfolgt und dort sauber/byte-verifiziert bestaetigt). Dies ist das
erste Mal in dieser gesamten Untersuchung (Updates 109-210), dass die
Nullung auf einen einzigen, konkreten Funktionsaufruf eingegrenzt wurde,
statt auf "irgendwo vor dem Fatal-Handler" oder "irgendwo in einer
1700-Aufrufe-Streaming-Schleife".

**Was das beweist und was nicht:** es identifiziert den Schreiber nicht.
Es engt das Feld aber stark ein: da `sub_00040821`s eigener Code UND
`sub_000516B7`s eigener uebersetzter Koerper bereits gelesen und sauber
sind, ist die mit allen bisherigen Befunden konsistenteste Erklaerung
(einschliesslich der urspruenglich in Update 188 vorgeschlagenen, in Update
190 nur TEILWEISE widerlegten Thread-Race-Hypothese -- Update 190 hat nur
die zwei bekannten Destruktor-/Aufraeumfunktionen `sub_00040B00`/
`sub_000407F7` als Mechanismus ausgeschlossen, niemals "irgendeine andere
Thread-Aktivitaet" allgemein) ein echter Cross-Thread-Schreibzugriff:
waehrend Thread 0 innerhalb von `sub_000516B7` auf den Abschluss der echten
I/O wartet, schreibt ein ANDERER Thread (der Prozess hat mehrere --
`nv2a_ack_thread`, mehrere `TppWorkerThread`-Instanzen, laut Update 188)
Nullen ueber genau dieses Objekt, ueber einen noch nicht identifizierten
Pfad.

**Modell-Stufe:** unveraendert MAXIMUM -- der Suchraum ist jetzt extrem eng
(ein Funktionsaufruf, eine konkrete Engstelle), aber den tatsaechlichen
Schreiber zu finden erfordert entweder Instrumentierung innerhalb des
nativen I/O-Wartepfads oder eine zuverlaessige Methode, den Schreibzugriff
eines anderen Threads zu beobachten -- keines von beidem wurde in dieser
Runde versucht.

MODEL STATUS: ESCALATION RECOMMENDED
Tier: MAXIMUM
Reason: die Root Cause ist jetzt auf einen einzigen Funktionsaufruf mit
byte-genauer Vorher-/Nachher-Evidenz eingegrenzt, aber den tatsaechlichen
Schreiber zu identifizieren erfordert entweder native Code-Instrumentierung
innerhalb des I/O-Wartepfads oder eine zuverlaessige Cross-Thread-
Schreibzugriffs-Beobachtungsmethode -- fruehere Versuche mit Letzterem
(Updates 188/189, Hardware-Watchpoints) waren in dieser Sandbox unzuverlaessig,
es braucht also einen neuen Ansatz statt einer Wiederholung.

**Naechster Test (fuer MAXIMUM):**
1. Einen weiteren Checkpoint an `sub_000516B7`s eigenem Eintritt hinzufuegen
   (Objektzustand auch dort aufzeichnen), um zu bestimmen, ob das Objekt zu
   diesem Zeitpunkt noch valide ist -- grenzt weiter ein, ob die Nullung in
   `sub_000516B7`s eigenem uebersetztem Setup-Code geschieht (Recompiler-
   seitig, single-threaded) oder tatsaechlich waehrend des blockierenden
   Wartens tiefer im nativen I/O-Layer (Cross-Thread).
2. Falls bestaetigt, dass es waehrend des tatsaechlichen Wartens geschieht:
   die native Kernel-Bridge-Warte-Primitive selbst instrumentieren (z.B. wo
   auch immer `xbox_NtReadFile`/`sub_00052A85` blockiert), um festzuhalten,
   welcher native Thread zum Zeitpunkt des Wartebeginns/-endes aktiv ist,
   um dies mit dem Thread zu korrelieren, der den tatsaechlichen
   Null-Schreibzugriff ausfuehrt.
3. Einen nativen (nicht Recomp-Guest-)Schutz in Erwaegung ziehen: die
   Speicherseite des exakten Objekts fuer die Dauer des
   `sub_000516B7`-Aufrufs per `VirtualProtect` temporaer auf Nur-Lesen
   setzen und den Schreiber ueber die resultierende Zugriffsverletzung samt
   fehlerverursachender Instruktion abfangen -- in dieser Sandbox
   vermutlich zuverlaessiger als die in Updates 188/189 gescheiterten
   Hardware-Watchpoints, da es gar nicht von cdb-Debug-Register-Handling
   abhaengt.

**Instrumentierungsstatus (unveraenderte Vorgabe aus Update 210):** die in
dieser Runde hinzugefuegten `sub_00040821`/`sub_0004086E`-Callsite-7/8/9/10-
Checkpoints sind zusaetzlicher temporaerer Diagnosecode in denselben bereits
genannten Dateien, unter denselben `SCDA-DISC-ERR-INSTRUMENTATION`-Markern.
Weiterhin rein diagnostisch, wird zurueckgesetzt, sobald der tatsaechliche
Schreiber gefunden ist oder diese Untersuchung anderweitig abgeschlossen
wird.
"""

with open(path, "wb") as f:
    f.write(raw + text.replace("\n", "\r\n").encode("utf-8"))

print("done, new size", len(raw) + len(text.replace("\n", "\r\n").encode("utf-8")))
