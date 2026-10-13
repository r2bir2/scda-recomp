status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** MAXIMUM (Eskalation von STRONG in Update 209)

**MODEL RECOMMENDATION:** In Codex: staerkste verfuegbare Analyse-Stufe.
In Claude: staerkstes verfuegbares Modell mit hoechster Reasoning-Tiefe.

MODEL STATUS: ESCALATION RECOMMENDED
Tier: MAXIMUM
Reason: `sub_00038A43` wurde vollstaendig gelesen und byte-genau gegen die
Original-XBE verifiziert (kein Recompiler-Fehler), aber die einfachste
Hypothese ("Bulk-Zero verursacht direkt den Read-Fehler") wurde durch die
Ablaufverfolgung WIDERLEGT: ein Status-Feld-Wert von exakt 0 wuerde als
Erfolg interpretiert, nicht als Fehler. Mehrere nicht-triviale, sich
gegenseitig ausschliessende Erklaerungen bleiben offen (echter negativer
NTSTATUS-Wert vs. Timeout-Pfad vs. anderer Aufrufort von `sub_0003FCA5`).

**Seit Update 208 -- Update 209 (diese Sitzung):**
`sub_00038A43` (402 Bytes/134 Instruktionen) vollstaendig gelesen. Der
Statuscode `4` (Ausloeser fuer den Disc-Fehler-Dialog `sub_0003FCA5`)
entsteht, wenn `sub_00052A9C()` (die den Reader wie ein `IO_STATUS_BLOCK`
behandelt: Feld `+0`=Status, `+4`=Bytes) `0` zurueckliefert UND der per
`sub_00052A2F()` (Thread-lokaler GetLastError-Aequivalent, FS/TEB-indiziert)
ermittelte Wert weder `0x26` (`ERROR_HANDLE_EOF`) noch `0x3E5`
(`ERROR_IO_PENDING`) ist. `sub_00052A9C` vollstaendig gelesen: liefert `0`
NUR wenn (a) eine Groesse-0-Anfrage oder ein fehlgeschlagener Wait
`ERROR_IO_INCOMPLETE` (`0x3E4`) setzt, ODER (b) das Reader-Status-Feld
einen ECHTEN NEGATIVEN Wert (Vorzeichenbit gesetzt) enthaelt, der dann per
SetLastError weitergereicht wird. **Ein Status-Feld von exakt `0`
(vollstaendig genullt) wuerde dagegen als ERFOLG (Rueckgabe `1`)
interpretiert, NICHT als Fehler** -- widerlegt die einfache "Bulk-Zero
verursacht Read-Fehler direkt"-Hypothese fuer diesen spezifischen
Mechanismus. Beide relevanten Code-Stellen (`sub_00038A43` Zeile
`0x38ae1-0x38b09`, `sub_00052A9C` komplett) per Capstone byte-genau gegen
die Original-XBE verifiziert -- STIMMEN EXAKT ueberein, kein Recompiler-Fehler.

**Repository:** `upgrade/selective-upstream`, HEAD weiterhin `79e6c19` als
letzter Code-Commit (nur Doku-Commits seither).

**Aktueller Blocker (unveraendert):** Ringpuffer-Unterobjekt (`this`,
Vtable `0x4F6940`, historisch beobachtete Adresse `0x1135928`) ist am
Fatal-Callback vollstaendig genullt (`~0x8070` Bytes ab dem Allocator-
Boundary-Tag). Der konkrete Schreiber ist weiterhin nicht gefunden.

**Bewiesen (kumulativ, inkl. neu in Update 209):** Exakte Ausdehnung
`0x8070` Bytes (193); Objekt-Leben < 1 Sekunde (206); `sub_0003FCA5` =
Disc-Fehler-Dialog (207); alle drei Dialog-Aufrufstellen identifiziert
(208); **NEU: Der Statuscode-4-Mechanismus in `sub_00038A43`/
`sub_00052A9C` ist vollstaendig verstanden und byte-verifiziert (209); ein
einfacher vollstaendiger Zero des Reader-Status-Feldes würde NICHT den
beobachteten Fehler ausloesen, sondern als Erfolg gewertet (209).**

**Widerlegt (kumulativ, inkl. neu in Update 209):** Alle bisherigen
Eintraege (siehe Updates 190-208); **NEU: Die einfache Hypothese "Bulk-
Zero des Reader-Objekts verursacht den Read-Fehler direkt" fuer den
`sub_00038A43`-Pfad (209); Recompiler-Fehler an den untersuchten Stellen
in `sub_00038A43`/`sub_00052A9C` (209, byte-verifiziert).**

**Offen (Stand nach Update 209), priorisiert fuer MAXIMUM:**
1. **Welche der drei `sub_0003FCA5`-Aufrufstellen tatsaechlich im echten
   Absturz genommen wird** (`sub_000356F1` vs. `sub_00038BD5` vs.
   `sub_00038F96`) -- noch nicht live verifiziert, nur statisch alle drei
   gelesen.
2. **Falls `sub_00038A43`-Pfad bestaetigt:** der tatsaechliche Wert im
   Reader-Status-Feld zum Absturzzeitpunkt -- negativ (echter NTSTATUS-
   Fehlercode, wuerde auf ANDERE Art Korruption als reines Zero-Fill
   hindeuten) vs. Timeout/Groesse-0-Pfad (unabhaengig vom Objektzustand).
3. **Liegt das Reader-Objekt ueberhaupt innerhalb des bekannten
   `0x8070`-Byte-Bereichs**, oder ist es eine separate, kleinere Allokation,
   die nur zeitlich korreliert?
4. `sub_0007F412`/`sub_00064B00` an tiefen Host-Offsets (Update 205),
   `sub_00052A9C`s Geschwisterfunktionen `sub_00036541` (noch ungeprueft).
5. Lipsync-Worker-Restrisiko (204); 26.08.2026-Bugreport (`sub_00056B22`);
   niedrigste Prioritaet: (a)/(d) aus 195, (b) aus 194.

**Naechster Test (Prioritaet, fuer MAXIMUM):**
1. Mit der gestaffelten Single-Attach-Technik (Update 206) mehrere
   unabhaengige Live-Stacks GENAU am Absturzpunkt (`sub_0003FCA5`-Eintritt)
   einfangen, um zu bestimmen, welche der drei Aufrufstellen tatsaechlich
   greift (Aufrufer-Adresse per Guest-Adressbereichs-Abgleich bestimmen,
   NICHT per cdb-Symbolname -- Host-Offset-Falle aus Update 205 beachten).
2. Falls `sub_00038A43`-Pfad bestaetigt: den Reader-Zeiger
   (`MEM32(decoder+0xFC)`) und dessen Status-Feld dynamisch (nicht per
   fester Adresse) live auslesen.
3. Pruefen, ob der Reader Teil des bekannten `0x8070`-Bereichs ist (Adress-
   Arithmetik gegen `0x1135928`/`this-8`..`this+0x8068`).
4. Weiterhin: kein Dauerpolling, keine bedingten Breakpoints, keine
   Wiederholung bereits ausgeschlossener Pfade ohne neue Evidenz.

**Relevante neue Artefakte:** `tools/debug/append_update209.py`,
`tools/debug/update_handoff_209.py`.

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen (reine
statische Analyse in Update 209). Das explizit geschuetzte Paar `cdb.exe`
PID `25164` / `scda_recomp.exe` PID `27552` wurde NICHT beruehrt. Upstream-
Stand unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
