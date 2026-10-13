status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG (De-Eskalation von MAXIMUM in Update 213 -- Root
Cause gefunden und byte-verifiziert)

**MODEL RECOMMENDATION:** In Codex/Claude reicht ab jetzt die normale
Arbeitsstufe; die verbleibende Arbeit ist mechanisch (Laufzeitwerte
bestaetigen, Fix-Entscheidung), kein offenes Reverse-Engineering mehr.

MODEL STATUS: DE-ESCALATION RECOMMENDED
Tier: STRONG
Reason: Update 213 hat die Root Cause der gesamten "0x8070-Zeroing"/Disc-
Fehler-Untersuchung (Updates 109-212) vollstaendig gefunden und per
Capstone byte-genau gegen die Original-XBE bestaetigt: eine unterlaufende
Memcpy-Zieladresse in `sub_00040821`s Puffer-Wraparound-Kompaktierung
(`loc_00040942`, Guest `0x40942`-`0x40955`). Dies ist NACHWEISLICH im
Original-Spielcode vorhanden, kein Recompiler-Bug.

**Seit Update 212 -- Update 213 (diese Sitzung, ROOT CAUSE GEFUNDEN):**
Update 212s "native Kernel-Bridge"-Schluss war eine Fehlinterpretation:
`sub_00040821` hat ZWEI Aufrufstellen zu `sub_000516B7` (0x408E1 und
0x40983), Update 212 hatte nur die zweite instrumentiert, weshalb zwei
gewoehnliche sequenzielle Aufrufe wie "Reentrancy" aussahen. Nach
Instrumentierung der ersten Aufrufstelle PLUS zwei Checkpoints direkt um
die Puffer-Kompaktierungs-Memcpy-Aufrufe bei `loc_00040942` (Ringpuffer auf
24 Eintraege vergroessert, um den vollen ~10-Checkpoint-Zyklus sichtbar zu
halten): das Objekt ist unmittelbar VOR den zwei `memcpy`/`rep movsd`+
`rep movsb`-Kopien noch vollstaendig valide und unmittelbar DANACH
komplett genullt. Nichts anderes laeuft dazwischen. Byte-genau per
Capstone gegen `default.xbe` verifiziert (`tools/debug/
verify_40909_capstone.py`) -- STIMMT EXAKT mit dem generierten C ueberein.
Mechanismus: `edi = Ringpuffer_Basis - verbleibender_Platz + 0x4000`
unterlaeuft die Allokation, wenn sowohl die deklarierte Datensatzlaenge ALS
AUCH der verbleibende Platz `> 0x4000` sind, und schreibt dann per memcpy
in die ~0x4070-Byte-Luecke zum vorausgehenden Objekt-Header (`0x1135928`).

**Repository:** `upgrade/selective-upstream`, Code-Commits `1e903d6`
(Update 210), `6e0fb35` (Update 211, nur Doku), `dcc76fc` (Update 212, nur
Doku, teilweise ueberholt durch 213). Instrumentierungserweiterungen dieser
Sitzung (Callsite 7-17, Ringpuffer-Groesse 24) liegen in den gitignorten
`tools/recomp/output/scda_v060/gen/`-Dateien (nicht committet) und in
`projects/scda/src/recomp_manual.c` (Ringpuffergroesse, noch nicht
committet).

**Aktueller Blocker:** KEIN Blocker im Sinne von "Ursache unbekannt" mehr --
die Root Cause ist gefunden. Offen ist nur noch: (1) die exakten
Laufzeitwerte von Datensatzlaenge/verbleibendem Platz bestaetigen, (2) eine
Fix-Strategie waehlen (Code-Clamp vs. Disc-Inhalt-Korrektur), (3) die
gesamte temporaere Instrumentierung zurueckbauen.

**Bewiesen (kumulativ, inkl. neu in Update 213):** Alle Eintraege aus
Updates 109-212 unveraendert gueltig als Beweiskette dorthin; **NEU UND
ABSCHLIESSEND: die exakte Speicherstelle und der exakte Mechanismus, der
das Ringpuffer-Unterobjekt nullt, sind gefunden und byte-verifiziert (213)
-- eine unterlaufende Memcpy-Zieladresse in `sub_00040821`s eigener
Wraparound-Kompaktierungslogik bei Guest `0x40942`, vorhanden Byte-fuer-Byte
im Original-Spiel, kein Recompiler-Fehler.**

**Widerlegt (kumulativ, inkl. neu in Update 213):** Update 212s Schluss
"Korruption liegt in nativem Host-Code" -- war eine durch eine
Instrumentierungsluecke verursachte Fehlinterpretation, jetzt vollstaendig
aufgeloest zugunsten der viel praeziseren, byte-verifizierten Erklaerung
oben.

**Offen (Stand nach Update 213), priorisiert fuer STRONG:**
1. Exakte Laufzeitwerte von `ecx`(verbleibend)/`esi`(Datensatzlaenge) am
   Branch `loc_00040940` fuer die fatale Iteration bestaetigen (erwartet:
   beide `> 0x4000`).
2. Fix-Strategie entscheiden: defensiver Clamp im Code vs. Korrektur des
   servierten Disc-/ISO-Inhalts, der diese seltene Kombination ueberhaupt
   erst erzeugt.
3. Danach: komplette Instrumentierungs-Rueckbau (siehe unten).

**Naechster Test (Prioritaet, fuer STRONG):**
1. Ringpuffer-Technik einmal mehr anwenden: `ecx`/`esi` am Branch
   `loc_00040940` aufzeichnen (Callsite 18), um die konkreten Zahlen zu
   bestaetigen.
2. Mit diesen Zahlen entscheiden, ob es sich um einen echten, seltenen
   Original-Spiel-Grenzfall handelt (dann: defensiver Clamp im generierten
   Code als pragmatischer Fix, klar kommentiert) oder um einen Unterschied
   im servierten Disc-Inhalt gegenueber einer Retail-Disc (dann: Disc-
   Inhalt/ISO-Extraktion pruefen).
3. Instrumentierungs-Rueckbau: alle `SCDA-DISC-ERR-INSTRUMENTATION`-Marker
   (17 Callsites) aus `recomp_0001.c`/`recomp_0002.c`/`recomp_types.h`
   entfernen, Ringpuffer-Block aus `recomp_manual.c` entfernen, sobald der
   obige Punkt geklaert ist -- **explizite Standing Instruction, nicht
   vergessen.**

**Instrumentierung (WICHTIG, temporaer, siehe Updates 210-213):** 17
`SCDA-DISC-ERR-INSTRUMENTATION`-Marker (Callsite 1-17) in
`tools/recomp/output/scda_v060/gen/recomp_0001.c` und `recomp_0002.c`
(beide gitignoriert, nicht committet), Ringpuffer-Infrastruktur (Groesse
jetzt 24) in `projects/scda/src/recomp_manual.c` (Groessenaenderung noch
NICHT committet, urspruengliche Version in `1e903d6`). **Weiterhin rein
diagnostisch, darf nicht als dauerhafter Fix bestehen bleiben** -- Rueckbau
folgt, sobald Fix-Strategie (siehe oben) entschieden ist.

**Relevante neue Artefakte:** `tools/debug/append_update213.py`,
`tools/debug/append_update213_status.py`, `tools/debug/update_handoff_213.py`,
`tools/debug/verify_40909_capstone.py` (Capstone-Byte-Verifikation).

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen mehr (PIDs
30884, 25372, 13664, alle in dieser Runde gestartet, wurden nach Extraktion
der Diagnosedaten sauber per `taskkill` beendet). Upstream-Stand
unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
