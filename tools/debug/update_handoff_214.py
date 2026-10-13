status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG (seit Update 213, in Update 214 bestaetigt)

**MODEL RECOMMENDATION:** Normale Arbeitsstufe reicht. Die verbleibende
Arbeit ist ein begrenzter xemu-vs-recomp-Feldvergleich mit bereits
vorhandenen Skripten (Update 185), kein offenes Reverse-Engineering mehr.

**Seit Update 213 -- Update 214 (diese Sitzung, WICHTIGE ZUSAMMENFUEHRUNG
MIT AELTEREM STRANG):**
Vollstaendige Parametererfassung des unterlaufenden memcpy durchgefuehrt
(neue `scda_capture_wraparound_once()`, feuert einmalig bei
`remaining_space > 0x4000`). Erfasste Werte (this=0x1135928,
databuf=0x1139998, bufsize=0x24000, source=0x113D999, dest=0x111D999,
copy_length=remaining_space=0x1FFFF, record_length=0x7E000000,
header_field_a=0, read_cursor=0x4001, total_field_4064=0x1D5F71,
handle=0x48000005 [ungueltig zum Erfassungszeitpunkt]). Reverse-Scan des
Guest-RAM fand das echte "Reader"-Objekt bei `0x011051C8`
(`+0x3C`-Feld=Unterobjekt-Zeiger, Muster aus Update 188). Ein dort
gefundener Dateiname-String (`DataXb\\Maps\\x06_cozumel_a.unr`) erwies sich
als Fehlspur (keine `.unr`-Datei im Disc-Abbild vorhanden). **Stattdessen
per empirischer Byte-Suche ueber das gesamte Disc-Abbild zweifelsfrei
bestimmt: die erfassten Rohbytes stammen aus `Default/DataXb/LMaps/
common.lin`, Byte-Offset `0x1C0000`** -- bestaetigt exakt die Hypothese
des Nutzers.

**KRITISCHE ZUSAMMENFUEHRUNG:** Diese exakte Stelle/dieser exakte Bug wurde
BEREITS in Updates 171-186 (17./18.09.2026, VOR Update 190) vollstaendig
gefunden, byte-verifiziert und TEILWEISE gefixt -- diese Sitzung (190-213)
hatte diesen Strang nicht gegengelesen und ihn unter dem Namen "0x8070-
Nullung" unabhaengig neu hergeleitet. Zusammengefuehrter Stand:
- Update 171: fand denselben memcpy, denselben Trigger (`common.lin`s 15.
  Chunk, Kurzlesevorgang nach 14x vollen 0x20000-Chunks =
  Offset 0x1C0000), byte-verifiziert (kein Recompiler-Bug).
- Update 173: fand den Mechanismus -- die Schreibposition-Formel
  ignoriert die tatsaechlich uebertragene Bytezahl vollstaendig, nimmt
  immer volle Chunk-Lieferung an.
- **Update 185: hat den in DIESER Runde angeforderten xemu-Abgleich
  BEREITS gemacht** -- echte Hardware erlebt exakt denselben Kurzlesevorgang
  (want=0x20000, got=0x15F71=89.969, bytegenau identisch), stoppt danach
  aber sauber (keine weiteren `sub_000516B7`-Aufrufe fuer diesen Stream).
- Update 186: implementierte den ehrlichen Byte-Count-Fix
  (`scda_disc_zero_short_read_tail`, `src/kernel/kernel_file.c`, WEITERHIN
  AKTIV im aktuellen Build) -- verhindert Muell-Handle-Werte, aber NICHT
  den eigentlichen Mechanismus: der Code versucht weiterhin, einen
  Datensatz-Header aus dem (jetzt sauber genullten) unverbrauchten Rest zu
  parsen, findet dort ein einzelnes veraltetes Byte aus einem fruehreren
  Zyklus, berechnet daraus die Muell-`record_length`, und derselbe
  memcpy-Underflow feuert weiterhin -- **das ist exakt das, was diese
  Sitzung als "0x8070-Nullung" verfolgt hat.**

**Aktueller Blocker (praezisiert):** WARUM ruft der rekompilierte Build
nach dem Kurzlesevorgang ueberhaupt noch einen weiteren
`sub_00040821`/`sub_000516B7`-Aufruf fuer diesen Stream auf, waehrend echte
Hardware das nachweislich nicht tut (Update 185)? Beide Seiten (Kurzlese-
Bytezahlen UND `sub_00040821`s eigener Code) sind bytegenau identisch --
die Abweichung muss in einer hoeherliegenden "soll ich erneut Daten
anfordern"-Entscheidung liegen, noch nicht lokalisiert.

**Bewiesen (kumulativ, inkl. neu in Update 214):** Alle Eintraege aus
Updates 109-213 unveraendert gueltig; **NEU: der exakte Datei-Offset
(`common.lin` @ `0x1C0000`) empirisch bestaetigt (214); die gesamte
Update-190-213-Untersuchung ist dieselbe wie die bereits in Updates
171-186 begonnene, teilweise gefixte common.lin-Kurzlese-Untersuchung
(214); der aktuelle Zero-Fill-Fix (`scda_disc_zero_short_read_tail`) ist
korrekt und aktiv, loest aber nur das "Muell-Handle"-Symptom, nicht den
zugrundeliegenden "Code versucht nach echtem EOF weiterzulesen"-Mechanismus
(214).**

**Widerlegt (kumulativ, inkl. neu in Update 214):** Die anfaengliche
Update-173-Theorie ("echte Hardware sieht wegen XISO-Packing nie einen
Kurzlesevorgang hier") wurde bereits in Update 185 widerlegt/verfeinert --
echte Hardware sieht denselben Kurzlesevorgang, behandelt ihn aber als
sauberes Streamende. Der in dieser Runde bedingte "Arena-Allocation"-Zweig
der Nutzeranweisung entfaellt, da seine Bedingung (xemu erreicht denselben
memcpy) nicht zutrifft.

**Offen (Stand nach Update 214), priorisiert fuer STRONG:**
1. **Die eigentliche Root Cause:** was entscheidet nach dem Kurzlesevorgang,
   ob erneut Daten angefordert werden -- vermutlich 1-2 Frames oberhalb von
   `sub_00040821` (`sub_0004438C` und Aufrufer, bereits in Updates
   173/195/196 kartiert). xemu-vs-recomp-Vergleich an dieser Stelle noch
   nicht gemacht.
2. Ob `sub_00040821`s `+0x44`("consumed")-Feld oder ein aufruferseitiges
   "erwartete Groesse"-Feld auf recomp vs. xemu unmittelbar nach dem
   Kurzlesevorgang abweicht -- Update 185s `tools/debug/
   xemu_object_fileobj.py` kann dafuer erweitert werden.
3. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Naechster Test (Prioritaet, fuer STRONG):**
1. Aufrufkette oberhalb von `sub_00040821` (`sub_0004438C` etc.) auf
   recomp UND xemu (um Update 185s t=27.828s-Treffer) vergleichen, um den
   Punkt zu finden, an dem entschieden wird "noch ein Read" vs. "Stream
   fertig".
2. Sobald gefunden: Fix auf DIESER Ebene waehlen (nicht im memcpy selbst --
   das waere dieselbe Art Symptom-Fix, die `scda_disc_fill_overrun` schon
   einmal als falsch erwiesen hat).
3. Danach: komplette Instrumentierungs-Rueckbau (17 Callsites +
   `scda_capture_wraparound_once` + Ringpuffer-Groesse), vollstaendige
   Recomp-Pipeline, canonical Build, Runtime-Verifikation, dann
   Weiterarbeit bis zum naechsten Blocker.

**Instrumentierung (WICHTIG, temporaer, siehe Updates 210-214):** 17
`SCDA-DISC-ERR-INSTRUMENTATION`-Callsites (`recomp_0001.c`/`recomp_0002.c`,
gitignoriert) plus `scda_capture_wraparound_once()` + vergroesserter
Ringpuffer (`recomp_manual.c`, teils committet in `1e903d6`, Groessen-
aenderung und neue Funktion noch NICHT committet). **Weiterhin rein
diagnostisch, darf nicht als dauerhafter Fix bestehen bleiben.**

**Relevante neue Artefakte:** `tools/debug/append_update214.py`,
`tools/debug/append_update214_status.py`, `tools/debug/update_handoff_214.py`,
`tools/debug/find_wrap_bytes.py`, `tools/debug/find_wrap_bytes2.py`
(empirische Datei-Offset-Bestimmung), sowie die bereits VORHANDENEN
`tools/debug/xemu_object_fileobj.py` und `tools/debug/
xemu_object_refill_wantgot.py` aus Update 185 -- fuer den naechsten Schritt
wiederverwenden, nicht neu schreiben.

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen mehr (PID
24072, in dieser Runde gestartet, sauber per `taskkill` beendet). Upstream-
Stand unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
