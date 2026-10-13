status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG (seit Update 213, in Update 214/215 bestaetigt)

**MODEL RECOMMENDATION:** Normale Arbeitsstufe. Verbleibende Arbeit: ein
einzelner, gezielter xemu-Feldvergleich mit bereits vorhandenem Tooling
(Update 185), danach Fixentscheidung -- kein offenes Reverse-Engineering.

**Seit Update 214 -- Update 215 (diese Sitzung, BUG AUF EXAKTE ZEILE
EINGEGRENZT):**
`sub_00040821` UND `sub_000516B7` vollstaendig neu/komplett gelesen (nicht
nur Fragmente wie in Updates 211/212). Ergebnis: **Aufrufstelle 1**
(`sub_000516B7` bei Ruecksprungadresse `0x000408E1`, der "<8 Bytes vor
Pufferende"-Refill-Pfad) **ist die tatsaechliche Ausfuehrung von
common.lin's letztem 89.969-Byte-Kurzlesevorgang** (bestaetigt per
Arithmetik: `0x4000 - restplatz(7) = 0x3FF9`, plus 8 Header-Bytes =
`0x4001` = der in Updates 213/214 erfasste `read_cursor`). Der Bug sitzt
in EINER exakten, unbedingten Codezeile bei `loc_000408F0`:
`MEM32(ebx+0x4060) [Cursor] = 0x4000 - MEM32(ebp-12) [Vor-Read-Restplatz]`
-- diese Formel liest NIE die echte uebertragene Bytezahl
(`MEM32(ebp-4)`, die eine Zeile darueber verfuegbar ist und korrekt fuer
einen ANDEREN Zaehler [+0x4064] benutzt wird, aber nicht fuer den Cursor).
**Das erklaert, warum Update 186s bereits aktiver Kernel-Fix
(`scda_disc_zero_short_read_tail`, ehrliches Byte-Count-Reporting) das
Problem nicht vollstaendig loest**: die Formel konsultiert die gemeldete
Bytezahl strukturell gar nicht -- keine noch so ehrliche Kernel-Meldung
kann eine Formel reparieren, die sie nie liest. Dies ist ein echter,
bytegenau bestaetigter Defekt im ORIGINALEN Spielcode selbst.

**Verbleibende, praezise eingegrenzte Frage:** Update 185 bewies bereits,
dass echte Hardware per xemu denselben Kurzlesevorgang erlebt
(`want=0x20000 got=0x15F71`) OHNE abzustuerzen. Da echte Hardware
dieselbe (bytegenau identische) fehlerhafte Formel ausfuehrt, muss der
EINGABEWERT (`restplatz_bis_pufferende` VOR Aufrufstelle 1, recomp=`7`)
auf echter Hardware anders sein (vermutlich `0`, wodurch die Formel
zufaellig `cursor=0x4000` exakt ergibt -- die korrekte Grenze zu frischen
Daten). Noch nicht live per xemu bestaetigt.

**Repository:** `upgrade/selective-upstream`, Code-Commits `1e903d6`
(Update 210), `6e0fb35`/`dcc76fc`/`7a64b83` (Updates 211/212/214, nur Doku
bzw. Instrumentierung). Kein Fix implementiert, wie explizit angewiesen.

**Aktueller Blocker:** die exakte Fehlerzeile ist bekannt (siehe oben),
aber WARUM recomps `restplatz_bis_pufferende` an dieser Stelle `7` statt
(vermutlich) `0` ist, im Vergleich zu echter Hardware, ist noch nicht per
xemu bestaetigt.

**Bewiesen (kumulativ, inkl. neu in Update 215):** Alle Eintraege aus
Updates 109-214 unveraendert gueltig; **NEU: Aufrufstelle 1 (`0x408E1`) ist
der tatsaechliche common.lin-Kurzlesevorgang (215); die Bug-Zeile ist auf
`loc_000408F0`s Cursor-Fortschreibung eingegrenzt, unbedingt, unabhaengig
vom `eax`-Rueckgabewert (215); Update 186s aktiver Fix kann dies
strukturell nicht loesen, da die Formel die gemeldete Bytezahl nie liest
(215); `sub_000516B7` hat zwei Rueckgabepfade (eax=1 sofortiger Erfolg,
eax=0 ueber Wait/sub_00052A85), was erklaert, warum kein Fatal-Dispatch
direkt an Aufrufstelle 1 passiert (215, kein Bug, sondern Design).**

**Offen (Stand nach Update 215), priorisiert fuer STRONG:**
1. **Der letzte fehlende Datenpunkt:** `restplatz_bis_pufferende` auf
   echter Hardware (xemu) im Moment von Aufrufstelle 1 fuer common.lins
   letzten Chunk -- erwartet `0`, noch nicht gemessen.
2. Nach Bestaetigung: Fix waehlen (Cursor-Formel korrigieren, echte
   Bytezahl nutzen -- sicher, da nur der bereits kaputte Kurzlese-Pfad
   betroffen ist) vs. weiter untersuchen, warum recomps eigenes
   Bookkeeping ueberhaupt zu `7` statt `0` kommt.
3. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Naechster Test (Prioritaet, fuer STRONG):**
1. `tools/debug/xemu_object_refill_wantgot.py` (bereits vorhanden, Update
   185) um das Auslesen von `MEM32(ebx+0x4058)`/`MEM32(ebx+0x4060)` (bzw.
   `ecx`/`eax` vor dem Aufruf) am selben Breakpoint erweitern, fuer den
   Treffer mit `want=0x20000 got=0x15F71`.
2. Vergleichen: recomp=`7` vs. xemu=? -- klaert die Fix-Richtung endgueltig.
3. Fix implementieren + verifizieren, DANACH komplette Instrumentierungs-
   Rueckbau (17 `SCDA-DISC-ERR-INSTRUMENTATION`-Callsites,
   `scda_capture_wraparound_once`, Ringpuffer-Groesse zurueck auf 8),
   vollstaendige Recomp-Pipeline, canonical Build, Runtime-Verifikation,
   Weiterarbeit bis zum naechsten Blocker.

**Instrumentierung (WICHTIG, temporaer, siehe Updates 210-215):**
unveraendert -- 17 Callsites + `scda_capture_wraparound_once` + Ringpuffer
Groesse 24 in den gitignorten `tools/recomp/output/scda_v060/gen/`-Dateien
und `projects/scda/src/recomp_manual.c`. **Weiterhin rein diagnostisch,
kein dauerhafter Fix.**

**Relevante neue Artefakte:** `tools/debug/append_update215.py`,
`tools/debug/append_update215_status.py`, `tools/debug/update_handoff_215.py`.
Fuer den naechsten Schritt: `tools/debug/xemu_object_refill_wantgot.py`
UND `tools/debug/xemu_object_fileobj.py` (beide bereits vorhanden, Update
185) erweitern statt neu schreiben.

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen. Upstream-
Stand unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
