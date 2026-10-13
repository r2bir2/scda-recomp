# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG. Der cursor-Formel-Bug ist vollstaendig geklaert,
dauerhaft gefixt und Ende-zu-Ende verifiziert. Der neue
`sub_004A12B0`-Blocker ist unbewertet.

**MODEL RECOMMENDATION:** Direkt mit der Untersuchung von
`sub_004A12B0`/`sub_004A1600`/`sub_004A1740` fortfahren -- zuerst pruefen,
ob `recomp_manual.c`s eigener `sub_00040A88`-Kommentar (2026-09-05/11,
UMOD-Reader-Untersuchung) dieselbe Adresse bereits einmal behandelt hat und
ob dieser aeltere Fix hier greift oder ein separates Problem vorliegt.

**Seit Update 220 -- Update 221 (diese Sitzung, NATIVER I/O-AUDIT
ABGESCHLOSSEN, FIX DAUERHAFT GEMACHT, VOLLSTAENDIGE PIPELINE VERIFIZIERT):**

1. **Nativer NtReadFile-Audit:** One-Shot-Erfassung direkt in
   `xbox_NtReadFile` (mit temporaer zurueckgesetztem Update-220-Fix, damit
   der ORIGINALE Codepfad den problematischen Read tatsaechlich erzeugt)
   ergab: `requested=131072, transferred=89969, filesize=1924977` -- ein
   echter, ehrlich gemeldeter Kurzlesevorgang am wahren Dateiende (89969 =
   1924977-1835008, exakt Update 214s bereits bekannte letzte-Chunk-Groesse).
   Verglichen mit Updates 185/216s bereits etablierter Live-xemu-
   Grundwahrheit: **xemu erlebt DENSELBEN Kurzlesevorgang** (want=0x20000
   got=0x15F71=89969) -- KEINE I/O-Semantik-Diskrepanz zwischen den
   Plattformen. Kernel-Bruecke, Host-I/O und Commit e1b6823 sind alle
   korrekt; der Defekt liegt ausschliesslich in der Guest-seitigen
   Cursor-Formel (bestaetigt Update 220 endgueltig).
2. **Fix dauerhaft gemacht:** `sub_00040821`/`sub_0004086E` mechanisch (per
   Skript, kein Abtippen) aus dem generierten Code extrahiert, der
   Zwei-Zeilen-Fix angewandt, nach `projects/scda/src/recomp_manual.c`
   verschoben, per bereits etabliertem `--exclude-manual`-Mechanismus von
   der Auto-Generierung ausgeschlossen (Praezedenzfall: `sub_00040A88`).
3. **Unabhaengiger Datendrift-Bug gefunden und behoben:** `sub_00040841`
   (ein echter Mid-Function-Wiedereintrittspunkt, per Capstone bytegenau
   bestaetigt) fehlte in der aktuellen Func-ID-Datenbank, verursachte einen
   Link-Fehler. Behoben via `function_seeds.json`-Eintrag (etablierter
   Projekt-Mechanismus) + vollstaendigem Drei-Stufen-Pipeline-Neulauf
   (disasm -> func_id -> recomp).
4. **Ende-zu-Ende verifiziert:** frischer Build nach vollstaendigem
   Regenerate erreicht denselben neuen `sub_004A12B0`-Blocker (nicht mehr
   das alte `sub_0003FCA5`-Einfrieren) -- der Fix uebersteht einen echten
   Neu-Regenerate von Grund auf.

**Durchgefuehrte Housekeeping-Aktion:** `SCDA-DIAG-221` (nativer Capture in
`kernel_file.c`) vollstaendig per `git checkout` zurueckgesetzt.

**Repository:** `upgrade/selective-upstream`. Committed:
`projects/scda/function_seeds.json` (4 Zeilen), `projects/scda/src/recomp_manual.c`
(657 Zeilen). Generierte Verzeichnisse bleiben gitignored, reproduzierbar
per dokumentierter Drei-Befehle-Pipeline (siehe Update 221).

**Aktueller Blocker:** `sub_004A12B0`/`sub_004A1600`/`sub_004A1740`, ein
niedrig-CPU (~16%) Poll-Loop an einer zuvor nie erreichten Stelle -- noch
nicht untersucht. Moeglicher Zusammenhang mit einer frueheren (2026-09-05/11)
UMOD-Reader-Untersuchung, die dieselbe Adresse bereits erwaehnt.

**Bewiesen (kumulativ, inkl. neu in Update 221):** Alle Eintraege aus
Updates 109-220 unveraendert gueltig. **NEU: der Kurzlesevorgang ist
nachweislich echt und plattformunabhaengig identisch (xemu und recomp
liefern dieselben Zahlen) -- die native I/O-Schicht ist vollstaendig
entlastet; der Fix aus Update 220 ist jetzt in einer regenerationsfesten
Quelle (`recomp_manual.c`) verankert und uebersteht einen vollstaendigen
Pipeline-Regenerate, verifiziert per Live-Snapshot (221).**

**Offen (Stand nach Update 221), priorisiert fuer STRONG:**
1. `sub_004A12B0`/`sub_004A1600`/`sub_004A1740` untersuchen -- zuerst den
   bestehenden `sub_00040A88`-Kommentar in `recomp_manual.c` (erwaehnt
   dieselbe Adresse) auf Relevanz pruefen.
2. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Instrumentierung:** Aktuell KEINE aktiv (voller Revert von
`SCDA-DIAG-221` in `kernel_file.c` erfolgt, verifiziert).

**Relevante neue Artefakte:** `tools/debug/append_update221.py`,
`tools/debug/append_update221_status.py`, `tools/debug/update_handoff_221.py`
(reine Doku-Skripte); `projects/scda/function_seeds.json` und
`projects/scda/src/recomp_manual.c` (der eigentliche, committete Fix).

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen. Upstream-
Stand unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
