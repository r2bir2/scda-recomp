path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

text = """

## Update 213 (2026-09-21): ROOT CAUSE GEFUNDEN UND BYTE-VERIFIZIERT -- die Header-Felder des Ringpuffer-Unterobjekts werden durch eine unterlaufende Memcpy-Zieladresse in sub_00040821s eigener Puffer-Wraparound-Kompaktierungslogik genullt; wortwoertlich in der Original-XBE bestaetigt, kein Recompiler-Bug

**Korrektur zu Update 212:** die "native Kernel-Bridge"-Hypothese war
falsch. Update 212 schloss auf native Ursache, weil zwei direkt
aufeinanderfolgende, gleicher-Thread-Eintritte in `sub_000516B7`s eigenen
`RECOMP_TRACE_PC`-Checkpoint ohne dazwischenliegenden Rueckkehr-Checkpoint
auftraten -- das war jedoch eine Instrumentierungsluecke, kein Beweis fuer
Reentrancy: `sub_00040821` hat **zwei separate Aufrufstellen** zu
`sub_000516B7` (Guest `0x408E1` und Guest `0x40983`), beide textlich fast
identisch, und Update 212s Checkpoints deckten nur die zweite ab. Nachdem
auch die erste Aufrufstelle instrumentiert wurde (siehe unten), loesten
sich die "zwei gleicher-Thread-Eintritte" als zwei voellig gewoehnliche,
sequenzielle Aufrufe innerhalb EINER Schleifeniteration auf -- keine
Cross-Thread-Aktivitaet, kein APC, kein nativer Code beteiligt.

**Methode:** vier weitere Ringpuffer-Checkpoints (weiterhin dieselbe
Update-210-212-Technik, weiterhin null CDB-Polling), gezielt gewaehlt, um
die ~140-Byte-Spanne zwischen den zwei `sub_000516B7`-Aufrufstellen zu
bisektieren: Callsite 13 unmittelbar nach Rueckkehr des ERSTEN Aufrufs
(`loc_000408E1`, zuvor komplett uninstrumentiert), Callsite 14 bei
`loc_00040909` (Beginn des "lies zwei 4-Byte-Datensatz-Header-Felder an der
aktuellen Cursor-Position"-Blocks), Callsite 16 bei `loc_00040942`
(Eintritt in den Puffer-Kompaktierungsblock, unmittelbar vor dessen zwei
`memcpy`/`rep movsd`+`rep movsb`-Aufrufen), und Callsite 17 unmittelbar
nachdem diese beiden Kopien abgeschlossen sind. Ringpuffer-Groesse von 8 auf
24 Eintraege erhoeht (`recomp_manual.c`), damit ein voller Zyklus (bis zu
10+ Checkpoint-Ausloesungen) ohne Verdraengung sichtbar bleibt. Ein weiterer
frischer, ungestoerter Lauf, ein einzelner statischer Read nach dem
Einfrieren.

**Ergebnis -- der Uebergang ist auf zwei benachbarte Codezeilen
eingegrenzt:** in exakter chronologischer Reihenfolge fuer den fatalen
Zyklus: Callsite 17 (Ende einer VORHERIGEN Iterations-Kompaktierung,
valide) -> Callsite 11 (`sub_000516B7`-Eintritt, Aufrufstelle 1, valide) ->
Callsite 13 (Aufrufstelle 1 kehrt zurueck, valide) -> Callsite 14
(`loc_00040909`, valide) -> **Callsite 16 (`loc_00040942`, unmittelbar vor
den zwei Memcpy-Operationen -- weiterhin vollstaendig valide:
`this=0x1135928`, `handle=0x48000005`, `puffergroesse=0x24000`,
`ringpuffer=0x1139998`) -> Callsite 17 (unmittelbar nachdem beide
Memcpy-Operationen abgeschlossen sind -- `handle=0`, `puffergroesse=0`,
`ringpuffer=0`, alles genullt)**. Zwischen Callsite 16 und 17 laeuft nichts
ausser den zwei Kopien selbst (`recomp_0001.c:68725-68739`).

**Der exakte Mechanismus (byte-verifiziert gegen `default.xbe` per
Capstone, `tools/debug/verify_40909_capstone.py`, Guest `0x40909`-`0x40992`
-- stimmt Instruktion fuer Instruktion exakt mit dem generierten C ueberein,
bestaetigt: KEIN Recompiler-Fehlerkennung):**
```
loc_0004087B: ecx = Ringpuffer_Groesse - Cursor          ; "verbleibender zusammenhaengender Platz"
              edx = 0x4000                               ; feste Konstante
              if (verbleibend >= Datensatzlaenge) goto loc_00040909  ; genug Platz, kein Wrap noetig
              ... (nur erreicht, wenn verbleibend < Datensatzlaenge, also ein
                   echter Wraparound: der naechste Datensatz passt nicht mehr
                   vor das physische Ende des Puffers) ...
loc_00040942: esi = Cursor_nach_Headern + Ringpuffer_Basis        ; Kopierquelle
              edi = Ringpuffer_Basis - verbleibend + 0x4000       ; Kopier-ZIEL
              memcpy(edi, esi, verbleibend);                      ; verschiebt den Rest
```
`edi`s Zielformel, `Ringpuffer_Basis - verbleibend + 0x4000`, liegt nur
dann innerhalb der Allokation, wenn `verbleibend <= 0x4000`. Wenn der
Datensatz, der die Wraparound-Pruefung ausgeloest hat, eine Laenge groesser
als `0x4000` deklariert, WAEHREND `verbleibend` (der tatsaechliche
zusammenhaengende Restplatz) EBENFALLS groesser als `0x4000` ist,
berechnet sich `edi` zu einer Adresse **vor** der eigenen Allokation des
Ringpuffers -- unterlaeuft direkt in die ~0x4070-Byte-Luecke, die diesen
Puffer (`0x1139998`) vom eigenen Header des besitzenden Unterobjekts
(`0x1135928`) trennt, exakt die Region, deren Nullung diese gesamte
Untersuchung (Updates 109-212) seit Update 190 verfolgt hat. Das `memcpy`
(ein `rep movsd`/`rep movsb`-Paar, das `verbleibend` Bytes kopiert)
ueberschreibt diese Region dann mit Daten, die aus dem Disc-Puffer gelesen
wurden -- erscheint als "genullt", wann immer die Quellbytes an dieser
Stelle zufaellig Null sind, was fuer ungefuellten Puffer-Restplatz ueblich
ist (deckungsgleich mit `scda_disc_zero_short_read_tail`, die anderswo
legitim ungelesene Restbytes nullt, Update 186).

**Dies ist Byte-fuer-Byte im Original-Xbox-Binary vorhanden -- kein
scda_recomp-Bug.** Ob es sich um einen echten Original-Spiel-Grenzfall
handelt (nur ausgeloest durch eine spezifische, seltene Kombination aus
Datensatzlaenge und Puffer-Cursor-Position, die der Disc-Image-Inhalt
dieser rekompilierten Umgebung zufaellig erreicht, waehrend echte Hardware
oder der tatsaechliche Byte-Strom der Retail-Disc dies moeglicherweise nie
tut) oder eine Design-Annahme des Original-Codes, die der virtuelle Disc-/
ISO-Inhalt dieses Projekts verletzt, ist die eine verbleibende offene Frage
-- aber der Korruptionsmechanismus selbst ist jetzt bis zur Instruktion
vollstaendig verstanden.

**Modell-Stufe:** das technische Raetsel, das dieser gesamte Strang verfolgt
hat, ist geschlossen. Verbleibende Arbeit ist Klassifikation (spieldaten-
abhaengiger Grenzfall vs. echter Ship-Bug vs. XBE/ISO-Inhaltsunterschied
gegenueber einer Retail-Disc), keine weitere Reverse-Engineering-Arbeit an
unbekannten Mechanismen.

MODEL STATUS: DE-ESCALATION RECOMMENDED
Tier: STRONG
Reason: die Root Cause ist jetzt vollstaendig identifiziert und byte-
verifiziert gegen das Original-Binary; was bleibt, ist die Bestaetigung der
Laufzeitwerte (die tatsaechliche Datensatzlaenge und die Restplatz-Zahlen,
die den Unterlauf ausloesen) und eine Fix-Strategie-Entscheidung, beides
mechanischer als das ergebnisoffene Reverse-Engineering, das diese
Untersuchung bis Update 212 brauchte.

**Naechster Test (fuer STRONG):**
1. Die tatsaechlichen Laufzeitwerte von `ecx` (verbleibender Platz) und
   `esi` (Datensatzlaenge) am Branch (`loc_00040940`, `jae 0x409a9`) fuer
   die fatale Iteration mit derselben Ringpuffer-Technik aufzeichnen, um die
   exakten Zahlen zu bestaetigen, die den Unterlauf ausloesen (erwartet:
   `esi > 0x4000` und `ecx` ebenfalls `> 0x4000`, aber `< esi`).
2. Einen Fix entscheiden: entweder (a) die Zielberechnung so absichern, dass
   sie nie unter `Ringpuffer_Basis` unterlaeuft, unabhaengig von
   `verbleibend` (defensiver Clamp -- aendert rekompiliertes Verhalten,
   braucht eine Entscheidung ueber den Scope), oder (b) herausfinden, warum
   der Disc-Inhalt dieses Projekts eine Datensatzlaenge erzeugt, die das
   Original-Spiel auf echter Hardware/der Retail-Disc offenbar nie
   antraf (Datenformat-Root-Cause, behoben durch Korrektur des servierten
   Dateiinhalts statt des Codes).
3. Sobald geklaert: ALLE temporaeren `SCDA-DISC-ERR-INSTRUMENTATION`-Marker
   zuruecksetzen (siebzehn Callsites quer durch `recomp_0001.c`/
   `recomp_0002.c`, den Ringpuffer-Block in `recomp_manual.c`, und die drei
   Makro-Checks in `recomp_types.h`) gemaess der Standing Instruction, dass
   diese Instrumentierung nicht als dauerhafter Fix bestehen bleiben darf.

**Instrumentierungsstatus:** vier weitere temporaere Checkpoints (13, 14,
16, 17) in dieser Runde hinzugefuegt, Ringpuffer von 8 auf 24 Eintraege
vergroessert (`SCDA_DISC_ERR_RING_SIZE`, `recomp_manual.c`). Alles
weiterhin rein diagnostisch.
"""

with open(path, "wb") as f:
    f.write(raw + text.replace("\n", "\r\n").encode("utf-8"))

print("done, new size", len(raw) + len(text.replace("\n", "\r\n").encode("utf-8")))
