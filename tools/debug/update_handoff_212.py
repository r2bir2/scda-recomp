status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** MAXIMUM (seit Update 209, in Update 210/211/212 bestaetigt)

**MODEL RECOMMENDATION:** In Codex: staerkste verfuegbare Analyse-Stufe.
In Claude: staerkstes verfuegbares Modell mit hoechster Reasoning-Tiefe.

MODEL STATUS: ESCALATION RECOMMENDED
Tier: MAXIMUM
Reason: Die gesamte rekompilierte Guest-Code-Kette dieses Bugs
(`sub_00040821`, `sub_0004086E`, `sub_000516B7`, `sub_00052A85`) ist jetzt
vollstaendig gelesen und als sauber bewiesen (Updates 210-212). Die
Korruption muss im NATIVEN Host-C-Code liegen (Kernel-Bridge-Wartepfad
hinter `MEM32(0x4F5180)`), noch nicht in dieser Sitzung untersucht -- ein
qualitativ anderer Codetyp als alles bisher in diesem Strang Gelesene.

**Seit Update 211 -- Update 212 (diese Sitzung):**
Zwei weitere Ringpuffer-Checkpoints (Callsite 11=`sub_000516B7`-Eintritt,
12=nach dessen `sub_00052A85`-Aufruf) hinzugefuegt. Ergebnis: das Objekt ist
bei `sub_000516B7`s EIGENEM Eintritt noch valide (widerlegt "Recompiler-Bug
in `sub_000516B7`s Setup-Code" endgueltig), aber ein ZWEITER Eintritt in
denselben Checkpoint feuert unmittelbar danach, AUF DEMSELBEN THREAD, OHNE
dass der erste Aufruf ueber seinen eigenen Abschluss-Checkpoint (Callsite 9)
zurueckgekehrt waere -- und dieser zweite Eintritt zeigt das Objekt bereits
genullt. Da `sub_000516B7` nur EINEN einzigen internen Aufruf macht
(`sub_00052A85`, ein 23-Byte-Wrapper, der nur `MEM32(0x4F5180)` indirekt
aufruft und das Ergebnis per SetLastError weiterreicht), muss die
tatsaechliche blockierende Arbeit -- und damit hoechstwahrscheinlich die
Nullung -- in der NATIVEN Host-Implementierung hinter diesem Tabellenslot
liegen, waehrend Thread 0 dort blockiert/alertable wartet (Same-Thread-APC
oder echter Cross-Thread-Zugriff, noch nicht unterschieden).

**Repository:** `upgrade/selective-upstream`, Code-Commits `1e903d6`
(Update 210) und `6e0fb35` (Update 211, nur Doku). Diese Sitzungs-
Instrumentierungserweiterungen (Callsite 7-12) liegen weiterhin nur in den
gitignorten `tools/recomp/output/scda_v060/gen/`-Dateien, nicht committet.

**Aktueller Blocker (praezisiert seit Update 212, jetzt auf natives Host-C
statt rekompilierten Guest-Code eingegrenzt):** WAEHREND des einen
`sub_00052A85`-Aufrufs (indirekt via `MEM32(0x4F5180)`) innerhalb von
`sub_000516B7` wird das Ringpuffer-Unterobjekt (`this=0x1135928`, Vtable
`0x4F6940`) komplett genullt. Der konkrete native Schreiber ist weiterhin
nicht gefunden.

**Bewiesen (kumulativ, inkl. neu in Update 212):** Alle Eintraege aus
Updates 109-211 unveraendert gueltig; **NEU: `sub_000516B7` selbst ist beim
eigenen Eintritt nachweislich noch valide -- der Recompiler-Bug-Verdacht
gegen `sub_000516B7`s eigenen uebersetzten Code ist damit endgueltig
ausgeschlossen (212). Ein zweiter, Same-Thread-Eintritt in denselben
Checkpoint OHNE zwischenzeitliche Rueckkehr des ersten Aufrufs zeigt das
Objekt bereits genullt (212) -- die gesamte rekompilierte Guest-Code-Kette
von `sub_00040821` bis `sub_00052A85` ist jetzt vollstaendig gelesen und
sauber.**

**Widerlegt/relativiert (kumulativ, inkl. neu in Update 212):** Jede
verbleibende Hypothese eines Fehlers im REKOMPILIERTEN Guest-Code fuer
diesen spezifischen Pfad (`sub_00040821`/`sub_0004086E`/`sub_000516B7`/
`sub_00052A85`) ist jetzt ausgeschlossen -- alle vier vollstaendig gelesen
und byte-/zustandsgenau als sauber bestaetigt.

**Offen (Stand nach Update 212), priorisiert fuer MAXIMUM:**
1. **Native Host-C-Implementierung hinter `MEM32(0x4F5180)`:** noch nicht
   in dieser Sitzung gelesen. Kandidat-Datei laut Update 186:
   `src/kernel/kernel_file.c`. Muss von Grund auf auf Worker-Thread-
   Uebergabe, Puffer-Pool/Freilisten-Wiederverwendung, oder einen
   Abschluss-Callback geprueft werden, der eine ~0x8070-Byte-Region nullen
   koennte.
2. **Ob die beobachtete "gleiche Thread-ID" tatsaechlich denselben OS-
   Thread bedeutet** oder ob dieser Umgebungs-`GetCurrentThreadId()` unter
   bestimmten Bedingungen mehrdeutig sein koennte -- noch nicht geprueft,
   aber wichtig fuer die Interpretation von Update 212.
3. Alles aus Update 210 Punkt 3 (ABI-Check-Fund bei Guest-VA `0x49CC50`)
   bleibt offen, niedrigere Prioritaet.
4. Restliche niedrigprioritaere Punkte aus Updates 194/195/204/205
   unveraendert.

**Naechster Test (Prioritaet, fuer MAXIMUM):**
1. `MEM32(0x4F5180)` aufloesen (welche native Funktion/welcher Kernel-Export
   liegt dort) und deren/dessen komplette Implementierung in `src/kernel/`
   lesen -- dies ist der erste Schritt in NATIVEM C-Code fuer diese gesamte
   Untersuchung, andere Herangehensweise als bisher noetig.
2. Pruefen, ob ein Thread-Pool/Worker-Thread in diese Wartezeit involviert
   ist; falls ja, native (nicht Guest-)Instrumentierung direkt im
   Warte-/Abschlusspfad mit echter OS-Thread-ID und Zeitstempel hinzufuegen.
3. `GetCurrentThreadId()`-Aequivalent dieser Umgebung kurz verifizieren
   (billiger Check).
4. Weiterhin: kein Dauerpolling, keine bedingten Breakpoints ueber laengere
   Zeit, keine Wiederholung der 188/189-Hardware-Watchpoint-Methode ohne
   neue Idee.

**Instrumentierung (WICHTIG, temporaer, siehe Updates 210/211/212):**
mittlerweile zwoelf `SCDA-DISC-ERR-INSTRUMENTATION`-Marker (Callsite 1-12)
in `tools/recomp/output/scda_v060/gen/recomp_0001.c` und `recomp_0002.c`
(beide gitignoriert, nicht committet). Ringpuffer-Infrastruktur in
`projects/scda/src/recomp_manual.c` (committet in `1e903d6`) unveraendert.
**Weiterhin rein diagnostisch, darf nicht als dauerhafter Fix bestehen
bleiben.**

**Relevante neue Artefakte:** `tools/debug/append_update212.py`,
`tools/debug/append_update212_status.py`, `tools/debug/update_handoff_212.py`.

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen mehr (PID
11788, in dieser Runde gestartet, wurde nach Extraktion der Diagnosedaten
sauber per `taskkill` beendet). Upstream-Stand unveraendert (`origin/main`
bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
