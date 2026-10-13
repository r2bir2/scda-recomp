path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

text = """

## Update 212 (2026-09-21): sub_000516B7 selbst ist nachweislich sauber (Objekt beim eigenen Eintritt noch valide); das Objekt kippt auf Null zwischen zwei direkt aufeinanderfolgenden, gleicher-Thread-Eintritten in sub_000516B7 -- zeigt auf das native Kernel-Bridge-Warten, nicht auf rekompilierten Guest-Code

**Methode:** zwei weitere Ringpuffer-Checkpoints hinzugefuegt (dieselbe
Diagnosetechnik wie Updates 210/211, weiterhin null CDB-Polling): Callsite
11 an der allerersten Zeile von `sub_000516B7` selbst (sichert `ebx` in
einer lokalen Variable, bevor der eigene Prolog das Register als Scratch
wiederverwendet -- als noetig bestaetigt durch Lesen des Prologs:
`PUSH32(esp, ebx); ... ebx = 0;` wenige Zeilen spaeter), und Callsite 12
unmittelbar nach dem einen Aufruf, den `sub_000516B7` an `sub_00052A85`
macht (der generische Kernel-Bridge-Aufruf-und-SetLastError-Wrapper,
`recomp_0002.c:50908` -- eine 23-Byte-Funktion, die selbst nur einen
indirekten Aufruf ueber den Tabellenslot `MEM32(0x4F5180)` macht und das
Ergebnis weiterreicht, d.h. die eigentliche blockierende Arbeit geschieht
vollstaendig in welcher nativen Host-Funktion auch immer dieser Tabellenslot
aktuell aufloest). Ein weiterer frischer, voellig ungestoerter Lauf, ein
einzelner statischer Read nach dem Einfrieren, wie zuvor.

**Ergebnis:** das letzte 8-Eintrag-Fenster war, in exakter chronologischer
Reihenfolge:
1. Zwei Callsite-7-Eintraege (`sub_00040821`-Eintritte, beide mit validem
   Objekt) ohne Checkpoint dazwischen -- konsistent mit mindestens einem
   gewoehnlichen `sub_00040821`-Aufruf, der diesen Zyklus keinen frischen
   Read brauchte.
2. Callsite 11 (`sub_000516B7`-Eintritt) -- **Objekt weiterhin vollstaendig
   valide** (`handle=0x48000005`, `puffergroesse=0x24000`,
   `ringpuffer=0x1139998`), beweist, dass die Korruption beim Eintritt in
   `sub_000516B7` selbst noch nicht geschehen ist.
3. **Ein zweiter Callsite-11-Eintrag unmittelbar danach, gleiche Thread-ID,
   OHNE Callsite-9 (der "sub_000516B7 ist zurueckgekehrt"-Checkpoint im
   Aufrufer) dazwischen -- und dieser zweite Eintrag zeigt das Objekt
   bereits vollstaendig genullt.**
4. Callsite 12 (unmittelbar nach dem `sub_00052A85`-Aufruf innerhalb dieser
   zweiten, bereits korrupten Invocation) -- weiterhin Null, konsistent.
5. Callsite 9 (Rueckkehr zum Aufrufer) -- Null.
6. Callsite 10, dann Callsite 5 (der Dispatch selbst, Ruecksprungadresse
   wieder `0x40992`) -- beide Null/konsistent mit Updates 210/211.

**Was das beweist:** `sub_000516B7`s eigener uebersetzter Koerper ist
unschuldig -- das Objekt ist beim eigenen Eintritt nachweislich noch valide
(Punkt 2), also kann nichts in seinem Prolog oder in den direkten
Wertparametern, die es erhaelt (es wird mit einzelnen skalaren Werten
aufgerufen, nicht mit dem Objektzeiger selbst -- bestaetigt durch erneutes
Lesen des Aufruf-Setups in `sub_00040821`, das `MEM32(ebx+0x3C)`, eine
berechnete Pufferadresse, und reine Skalare pusht, niemals `ebx` selbst) der
Schreiber sein; es hat strukturell ueberhaupt keinen Bezug zu diesem Objekt
ausser durch Zufall. Der interessante Fund ist Punkt 3: ein **zweiter
Eintritt in denselben `RECOMP_TRACE_PC(0x000516B7)`-Checkpoint, auf
DEMSELBEN Thread, BEVOR der Abschluss-Checkpoint (Callsite 9) des ERSTEN
Eintritts gefeuert hat.** Da `sub_000516B7` ~35 weitere Aufrufstellen quer
durch die gesamte Codebasis hat (gegrept: `recomp_0001/0002/0015/0018/0023/
0026/0027.c`), ist das nicht zwingend Rekursion im klassischen C-Sinn -- aber
dass der Objektzeiger beim zweiten Eintritt wieder `0x1135928` ist, auf
identischem Thread, ohne zwischenzeitliche Rueckkehr, bedeutet: was auch
immer diesen zweiten Eintritt erreicht hat, tat dies, WAEHREND der erste
Aufruf noch logisch in Bearbeitung war (also waehrend der eine
`sub_00052A85`/Tabellenslot-`0x4F5180`-native-Aufruf etwas tut -- fast
sicher ein blockierendes oder alertable Warten). Das passt entweder zu einem
Same-Thread-APC/Callback, das waehrend eines alertable Waits im nativen
Kernel-Bridge feuert, oder zu einem zweiten, tatsaechlich anderen Thread, der
nur dann dieselbe `GetCurrentThreadId()` meldet, wenn die Thread-ID-Emulation
dieser Umgebung selbst fragwuerdig ist (noch nicht geprueft) -- beide
Moeglichkeiten zeigen weg vom bereits vollstaendig gelesenen rekompilierten
Guest-Code (`sub_00040821`, `sub_0004086E`, `sub_000516B7`, `sub_00052A85`
sind jetzt alle als sauber bestaetigt) und hin zur **nativen Host-
Implementierung dessen, was `MEM32(0x4F5180)` aktuell aufloest** (der
tatsaechliche Disc-Read-Kernel-Export), in dieser Sitzung noch nicht
untersucht.

**Modell-Stufe:** unveraendert MAXIMUM -- die rekompilierte-Guest-Code-Seite
dieser Untersuchung ist jetzt ausgeschoepft (jede Funktion in der Kette von
`sub_00040821` bis zum Kernel-Bridge-Wrapper ist gelesen und sauber); die
verbleibende Arbeit liegt vollstaendig in nativem Host-C (`src/kernel/`),
einer qualitativ anderen Art Code als alles bisher in diesem Untersuchungs-
strang Gelesene.

MODEL STATUS: ESCALATION RECOMMENDED
Tier: MAXIMUM
Reason: die gesamte in diesen Bug involvierte rekompilierte Guest-Aufruf-
kette ist jetzt vollstaendig gelesen und als sauber bewiesen; die Korruption
muss im nativen Host-Code entstehen (das Kernel-Bridge-Warten, ein Worker-
Thread-Pool, oder ein Puffer-Wiederverwendungspfad), was sich qualitativ von
der x86-Recompiler-Analyse unterscheidet, die dieser gesamte
Untersuchungsstrang bisher genutzt hat, und ein eigenes, sorgfaeltiges
Von-Grund-auf-Lesen braucht.

**Naechster Test (fuer MAXIMUM):**
1. Identifizieren, was `MEM32(0x4F5180)` aktuell aufloest (der Kernel-
   Export-Tabellenslot, ueber den `sub_00052A85` dispatcht) und dessen
   native C-Implementierung in `src/kernel/` (Kandidat: `kernel_file.c`,
   laut Update 186s Verweis auf `scda_disc_zero_short_read_tail` dort)
   vollstaendig lesen, insbesondere auf: Worker-Thread-Uebergabe, jede
   Puffer-Pool-/Freiliste-Wiederverwendung, oder einen Abschluss-Callback,
   der plausibel eine unabhaengige ~0x8070-Byte-Region nullen koennte.
2. Pruefen, ob das `GetCurrentThreadId()`-Aequivalent dieser Umgebung unter
   irgendeinen Umstaenden dieselbe ID fuer zwei tatsaechlich verschiedene
   OS-Threads melden koennte (wuerde die Interpretation des "gleiche
   Thread-ID, keine zwischenzeitliche Rueckkehr"-Befunds dieser Runde
   wesentlich aendern) -- ein billiger, schneller Check, bevor dem
   Einzel-Thread-Befund weiter vertraut wird.
3. Falls ein Worker-Thread-Pool bestaetigt beteiligt ist: eine native
   (nicht rekompilierte) Diagnose direkt im Warte-/Abschlusspfad der
   Kernel-Bridge hinzufuegen -- die echte OS-Thread-ID (nicht die Guest-
   Emulation) und einen Zeitstempel unmittelbar vor und nach der
   tatsaechlichen blockierenden Primitive aufzeichnen, um zu sehen, ob ein
   zweiter, tatsaechlich anderer Thread waehrend genau dieses Fensters
   aktiv ist.

**Instrumentierungsstatus (unveraenderte Vorgabe):** zwei weitere temporaere
Checkpoints (Callsite 11 an `sub_000516B7`s Eintritt, Callsite 12 nach
dessen `sub_00052A85`-Aufruf) in `recomp_0002.c` hinzugefuegt, dieselben
`SCDA-DISC-ERR-INSTRUMENTATION`-Marker, dieselbe Ringpuffer-Infrastruktur
aus Update 210. Weiterhin rein diagnostisch; wird zurueckgesetzt, sobald der
tatsaechliche Schreiber im nativen Kernel-Bridge-Code gefunden ist oder
diese Untersuchung anderweitig abgeschlossen wird.
"""

with open(path, "wb") as f:
    f.write(raw + text.replace("\n", "\r\n").encode("utf-8"))

print("done, new size", len(raw) + len(text.replace("\n", "\r\n").encode("utf-8")))
