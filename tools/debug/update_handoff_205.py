status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG (Herabstufung von MAXIMUM empfohlen)

**MODEL RECOMMENDATION:** In Codex: starke, aber nicht die hoechste Analyse-Stufe.
In Claude: starkes Modell, hohe aber nicht maximale Reasoning-Tiefe.

MODEL STATUS: DE-ESCALATION RECOMMENDED
Tier: STRONG
Reason: Update 205 bestand ausschliesslich aus vorbereiteten Polling-Laeufen,
Prozess-Neustarts und Log-/Stack-Auswertung -- keine konzeptionell schwierige
neue Codeanalyse. Sobald die naechste Runde entweder den GOOD-Zustand live
einfaengt oder neue widerspruechliche Evidenz liefert, wieder MAXIMUM
empfehlen.

**Seit Update 204 -- Update 205 (diese Sitzung):** Vier Live-Capture-Versuche
fuer den GOOD->ZERO-Uebergang, alle ohne GOOD-Zustand, aber mit wichtigen
Korrekturen:
1. **Timing-Korrektur:** Ein komplett unbeobachteter frischer Prozess
   erreicht den Fatal-Zustand bereits nach ca. 25-30 Sekunden (nicht Minuten
   wie zuvor angenommen). Das GOOD-Fenster ist vermutlich nur wenige
   Sekunden lang.
2. **Methodik-Einschraenkung:** Kontinuierliches `cdb -pv`-Polling ueber
   Minuten kann den Zielprozess entweder mid-boot haengen lassen (PID 31684)
   oder ihn dauerhaft unattachierbar machen (`Win32 error 0n156`, PID 3660
   und 47872, `Suspend: 40` beobachtet). Polling darf kuenftig nur noch in
   kurzen, gezielten Bursts erfolgen, nicht als Dauerschleife.
3. **Methodik-Korrektur (Stack-Interpretation):** cdb-Stackframe-Offsets
   (`sub_X+0xY`) beziehen sich auf den KOMPILIERTEN Host-x64-Code, nicht auf
   den im Quelltext dokumentierten Guest-x86-Adressbereich. Guest-Adresse +
   Host-Offset ergibt KEINE gueltige neue Guest-Adresse (in dieser Runde
   fälschlich versucht und wieder verworfen).
4. Drei neue Frames aus einem live gefangenen Fatal-Stack geprueft und
   ausgeschlossen: `sub_0007DC43` (Vtable-Thunk), `sub_0007DE25`
   (Dispatch-Relay), `sub_001CA0A5` (Konstruktor eines kleinen, unverwandten
   Objekts, Felder bis max. +0x1E8). Zwei Frames (`sub_0007F412+0x32b7`,
   `sub_00064B00+0x33ee`) bleiben wegen des o.g. Host/Guest-Offset-Problems
   ungeklaert (erfordern Host-Binary-Disassemblierung, nicht Quelltext).

**Repository:** `upgrade/selective-upstream`, HEAD weiterhin `79e6c19` als
letzter Code-Commit (nur Doku-Commits seither).

**Aktueller Blocker (unveraendert):** Ringpuffer-Unterobjekt (`this`, Vtable
`0x4F6940`, Adresse `0x1135928`) ist am Fatal-Callback vollstaendig genullt
(`~0x8070` Bytes ab dem Allocator-Boundary-Tag). Der konkrete Schreiber ist
weiterhin nicht gefunden.

**Bewiesen (kumulativ, inkl. neu in Update 205):** Exakte Ausdehnung `0x8070`
Bytes (193); Schreiber muss Chunk-Zeiger nutzen (195); Free() nullt nie
Nutzdaten (195); alte "falsches Handle"-Beobachtung = dasselbe Ereignis
(196); nur 2 Threads beim Absturz (197); 4 Thread-Erzeugungen insgesamt,
Bink-Worker enden vorher, Lipsync lebt weiter (198-199); Timer/APC-
Mechanismus wird nie genutzt (204); Virtual-Memory-Release-Pfad wird nie
aufgerufen (204); **NEU: Gesamtlebensdauer bis Fatal betraegt nur ca.
25-30 Sekunden ab Prozessstart (205); wiederholtes cdb-Polling kann den
Zielprozess selbst blockieren oder dauerhaft unattachierbar machen (205).**

**Widerlegt (kumulativ, inkl. neu in Update 205):** Beide Destruktorpfade
(190); Vtable-Familie (194); Allocator-Slots 0/4/8/0xC + `sub_00048B15`
(195); Disk-Read-Kette (196); restliche main()-Kette (197); aktiver
Bink-Zero-Block mit falschem Ziel/Groesse (200); 3 von 4 Lipsync-
Indirections (202); Timer-Queue-Mechanismus komplett (204);
Virtual-Memory-Release-Pfad komplett (204); **NEU: `sub_0007DC43`,
`sub_0007DE25`, `sub_001CA0A5` als Schreiber (205).**

**Offen (Stand nach Update 205):**
1. **Ein bisher nicht identifizierter Mechanismus innerhalb des noch
   lebenden Lipsync-Workers** (der 4. interne Aufruf ist stark, aber nicht
   absolut negativ, siehe Update 204).
2. **Die alte, nie abschliessend verfolgte Verbindung zum 26.08.2026-
   Bugreport** (`sub_00056B22`, Freilisten-Korruption, Temp-Region
   `0x00F80000`).
3. **`sub_0007F412` und `sub_00064B00`** an den in Update 205 gefundenen
   tiefen Host-Offsets (`+0x32b7` bzw. `+0x33ee`) -- ungeklaert, da eine
   Zuordnung eine Host-Binary-Disassemblierung braucht statt der
   Quelltextkommentare.
4. **Der GOOD-Zustand selbst wurde noch nie live erfasst** -- alle bisherigen
   Polling-Versuche (Updates 190-205) haben entweder bereits laufende,
   schon-GOOD-Prozesse spaeter angetroffen, oder frische Prozesse verfehlt.
5. Niedrigste Prioritaet: (a)/(d) aus Update 195, (b) aus Update 194.

**Naechster Test (neue Messmethode, siehe Update 205 "Nicht wiederholen"):**
Kein weiterer Versuch mit kontinuierlichem Polling einer EINZELNEN frischen
Instanz. Stattdessen **gestaffeltes Populations-Sampling:**
1. 6-8 frische Prozesse in kurzem Abstand starten, dabei JEDEN Prozess nur
   GENAU EINMAL per `cdb -pv` beruehren (kein Wiederholungs-Polling pro
   Prozess -- vermeidet den in 205 gefundenen Attach-Verschleiss).
2. Jedem Prozess eine andere Wartezeit vor diesem einen Attach zuweisen,
   gestaffelt ueber den jetzt bekannten ~0-30-Sekunden-Bereich (z.B. 2s, 5s,
   8s, 12s, 16s, 20s, 25s, 30s).
3. Bei jedem einzelnen Attach: `?? *(unsigned long*)0x1135928` (Vtable) UND
   `~*kb` in einem einzigen `-c`-Aufruf kombinieren, um sowohl den
   Objektzustand als auch die Thread-Stacks an genau diesem Zeitpunkt zu
   bekommen.
4. Aus den 6-8 Einzel-Schnappschuessen ein Zeitstrahl-Bild zusammensetzen
   (wann GOOD, wann ZERO, welche Call-Chain bei welchem Zeitpunkt).
5. Bedingte CDB-Breakpoints UND Hardware-Watchpoints weiterhin NICHT
   versuchen. Keine Dauerschleifen-Polls mehr auf eine einzelne Instanz.
6. Sobald der GOOD->BAD-Uebergang auf ein enges Zeitfenster/eine Funktion
   eingegrenzt ist: xemu-Vergleich fuer genau diese Stelle.

**Relevante neue Artefakte:** `tools/debug/poll_spatial_wide.sh`,
`tools/debug/poll_boundary2.sh`, `tools/debug/poll_boundary3.sh`,
`tools/debug/poll_boundary{2,3,4}_out.log`, `tools/debug/launch_timestamped.sh`,
`tools/debug/wait_for_stall.sh`, `tools/debug/append_update205.py`.

**Laufende Prozesse / IN PROGRESS:** Alle Testprozesse dieser Runde (3660,
47872, 31684, 21796) wurden beendet (unattachierbar bzw. diagnostisch
ausgeschoepft). Das explizit geschuetzte Paar `cdb.exe` PID `25164` /
`scda_recomp.exe` PID `27552` bleibt unangetastet und wurde in dieser Runde
NICHT beruehrt. Upstream-Stand unveraendert (`origin/main` bei `766ecef`,
Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
