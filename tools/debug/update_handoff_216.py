status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG (seit Update 213, in Updates 214/215/216 bestaetigt)

**MODEL RECOMMENDATION:** Normale Arbeitsstufe. Naechster Schritt ist eine
begrenzte, klar definierte Funktions-Lese-Aufgabe (drei benannte
Funktionen), kein offenes Reverse-Engineering.

**Seit Update 215 -- Update 216 (diese Sitzung, LIVE-XEMU-ABGLEICH
DURCHGEFUEHRT, INSTRUMENTIERUNG VOLLSTAENDIG ZURUECKGESETZT):**
Frischer xemu-Lauf heute (nicht das alte Update-185-Log) reproduzierte
Update 185s exakten Kurzlesevorgang bytegenau
(`want=0x20000 got=0x15F71`, ueber Aufrufstelle 2/`0x40983`), mit
`restplatz_bis_pufferende=0x1E22` an dieser Stelle. Ein vollstaendiges
Neu-Lesen von `sub_00040821` plus eine passende neue recomp-seitige
Erfassung (Callsite 18) entwirrte die Rollen der zwei Aufrufstellen
endgueltig: **Aufrufstelle 1 liefert auf recomp den validen Read,
Aufrufstelle 2 (danach, im selben Zyklus) findet das Objekt bereits
genullt** -- die eigentliche Korruption passiert exakt in der Kompaktierung
zwischen Aufrufstelle 1 und 2 (Callsite 16->17-Uebergang, bestaetigt
Update 213 erneut). Die TATSAECHLICHE Root Cause erwies sich als eine
DRITTE, bereits vier Tage frueher (Update 177, 17.09.2026) aufgeworfene,
nie abgeschlossene Frage: ob/wo die hoeherliegende Aufrufkette
(`sub_0007DE25`/`sub_0007F412`/`sub_00064B00`, mehrere KB Originalcode,
bisher nur auf einen Zero-Fill-Schreiber geprueft, NICHT auf Terminator-
Logik) ein Terminator-Tag korrekt erkennt. Da drei intern konsistente,
aber unterschiedliche Fix-Strategien moeglich bleiben (siehe Update 216),
wurde bewusst KEIN Fix implementiert.

**Durchgefuehrte Housekeeping-Aktion:** die gesamte temporaere
Instrumentierung dieser Sitzung (Updates 210-216: 18
`SCDA-DISC-ERR-INSTRUMENTATION`-Callsites in den generierten
`recomp_0001.c`/`recomp_0002.c`/`recomp_types.h`, `scda_capture_wraparound_once`
und der vergroesserte Ringpuffer in `recomp_manual.c`) wurde
VOLLSTAENDIG zurueckgesetzt: `recomp_manual.c` per
`git checkout da01250 -- ...` auf den exakten Vor-Update-210-Stand
zurueckgeholt (177 Zeilen entfernt, verifiziert sauber), alle markierten
Zeilen aus den gitignorten generierten Dateien programmatisch entfernt
(28+3+3 Zeilen plus die verwaiste Funktionsdeklaration), **neu gebaut --
kompiliert sauber ohne Fehler**, und mit einem frischen Lauf verifiziert:
identisches Verhalten wie vor jeder Instrumentierung (Speicherverbrauch
173.496K, exakt im historischen Normbereich, Prozess friert wie erwartet
am bekannten `jmp $` in `sub_0003FCA5` ein -- unveraendertes Verhalten,
da bewusst kein funktionaler Fix gemacht wurde).

**Repository:** `upgrade/selective-upstream`. Alle bisherigen Instrumentierungs-
Commits (`1e903d6`, `e63a491`, `7a64b83`) bleiben in der Historie (sie waren
zum jeweiligen Zeitpunkt korrekt dokumentiert); dieser Revert wird als
eigener, neuer Commit hinzugefuegt. Kein Fix committet -- wie in Update 216
begruendet.

**Aktueller Blocker:** die Root Cause ist auf drei konkrete, sich
gegenseitig ausschliessende Erklaerungen eingegrenzt (siehe Update 216),
von denen keine bestaetigt ist. Der naechste Schritt ist rein diagnostisch
(Funktionen lesen), kein Fix-Versuch.

**Bewiesen (kumulativ, inkl. neu in Update 216):** Alle Eintraege aus
Updates 109-215 unveraendert gueltig; **NEU: frischer Live-xemu-Beweis
(nicht nur das alte Update-185-Log) bestaetigt den identischen
Kurzlesevorgang erneut (216); Aufrufstelle 1 liefert den validen Read,
die Kompaktierung DANACH (vor Aufrufstelle 2) ist der exakte Korruptions-
moment (216, praezisiert gegenueber 215); die wahre Root Cause ist Update
177s vier Tage aeltere, nie abgeschlossene Terminator-Tag-Frage, nicht
(nur) der Cursor-Formel-Defekt aus Update 215 (216).**

**Offen (Stand nach Update 216), priorisiert fuer STRONG:**
1. `sub_0007DE25`, `sub_0007F412`, `sub_00064B00` vollstaendig auf
   Terminator-/Tag-Pruef-Logik lesen (Update 177s eigener, nie
   abgeschlossener naechster Schritt) -- noch nicht begonnen.
2. Je nach Ergebnis: eine von drei Fix-Strategien waehlen (siehe Update
   216 fuer alle drei mit Begruendung).
3. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Naechster Test (Prioritaet, fuer STRONG):** siehe Update 216, Punkte 1-4.
Kurz: die drei genannten Funktionen lesen -> je nach Fund Fix waehlen ->
verifizieren -> (Instrumentierung ist bereits zurueckgesetzt, dieser
Schritt entfaellt jetzt) -> Weiterarbeit bis zum naechsten Blocker.

**Instrumentierung:** VOLLSTAENDIG ZURUECKGESETZT (siehe oben). Fuer die
naechste Phase (Terminator-Logik in `sub_0007DE25` etc.) wird bei Bedarf
neue, dediziert dafuer passende Instrumentierung noetig sein -- nicht die
alte wiederverwenden.

**Relevante neue Artefakte (bleiben bestehen, keine Instrumentierung):**
`tools/debug/append_update216.py`, `tools/debug/append_update216_status.py`,
`tools/debug/update_handoff_216.py`,
`tools/debug/xemu_object_refill_wantgot_remaining.py` (neues, dauerhaftes
xemu-Debug-Tool nach etabliertem Update-185-Muster, nicht Teil der
zurueckgesetzten Instrumentierung).

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen. xemu wurde
nach der Messung sauber beendet. Upstream-Stand unveraendert
(`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
