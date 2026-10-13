path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

text = """

## Update 214 (2026-09-21): Vollstaendige Parametererfassung des unterlaufenden memcpy durchgefuehrt; Datei/Offset empirisch als common.lin @ 0x1C0000 identifiziert -- und damit die gesamte 190-213-Untersuchung mit einem BEREITS 2026-09-17/18 untersuchten, teilweise gefixten Bug (Updates 171-188) verbunden, den diese Sitzung nicht gegengelesen hatte

**Wichtige Korrektur, vor allem anderen:** diese Runde hat aufgedeckt, dass
Updates 171-188 (17./18.09.2026, vier Tage vor Beginn der "0x8070-Nullung"-
Rahmung in Update 190) bereits GENAU DENSELBEN unterlaufenden memcpy
gefunden, byte-verifiziert und teilweise gefixt haben, den diese Sitzung in
Update 213 unabhaengig neu entdeckt hat -- dieselbe Funktion
(`sub_00040821`), dieselben Zeilen, derselbe `common.lin`-Kurzlese-Ausloeser,
per `xemu` gegen echte Hardware bestaetigt. Diese Sitzungs-Updates 190-213
kannten diesen Strang nicht und haben einen grossen Teil davon unabhaengig
aus einem anderen Blickwinkel neu hergeleitet (ausgehend von "das Objekt
ist beim Fatal-Callback genullt" statt "warum bekommt der naechste
NtReadFile ein Muell-Handle"). Dieses Update fuehrt beide zusammen.

**Befolgter Auftrag:** den in Update 213 gefundenen memcpy als bestaetigten
Schreiber behandeln, aber noch nicht als geklaerte Root Cause; die
vorhandene Instrumentierung fuer einen weiteren voellig ungestoerten Lauf
nutzen, um einmalig ALLE Eingangsparameter des fehlerhaften memcpy zu
erfassen (Ringpuffer-Basis, Source, Destination, Copy-Length, Restplatz,
Datensatz-/Chunk-Laenge, Cursor, Reader/Parent, Thread, Datei, exakter
Datei-Offset); danach ausschliesslich die Herkunft der Werte zurueckverfolgen,
die den Underflow ermoeglichen; noch keinen Clamp einbauen.

**Instrumentierung:** neue `scda_capture_wraparound_once()` in
`recomp_manual.c`, aufgerufen einmalig aus `loc_00040942` (unmittelbar
bevor die `rep movsd`/`rep movsb`-Kopien laufen), nur ausgeloest wenn
`remaining_space > 0x4000` (die tatsaechlich gefaehrliche Bedingung).
Erfasst: `this`, die eigenen Ringpuffer-Felder (`databuf`/`buffer_size`),
`source`, `destination`, `copy_length`, `remaining_space`
(`MEM32(ebp-12)`), `record_length` (`MEM32(ebp-16)`), das andere
4-Byte-Header-Feld (`MEM32(ebp-20)`), den Lese-Cursor
(`MEM32(this+0x4060)`), einen laufenden Zaehler (`MEM32(this+0x4064)`), das
Datei-Handle (`MEM32(this+0x3C)`), die Thread-ID, dazu einen Live-
`SetFilePointerEx`/`GetFinalPathNameByHandleW`-Query auf das Handle, und
einen vollstaendigen linearen Scan des Guest-RAM
(`0x00010000`-`0x04000000`, eine einzige zusammenhaengende `VirtualAlloc`-
Region, sicher direkt scannbar) nach jedem 4-Byte-Wert gleich `this`, um
ein "Reader"/Parent-Objekt zu finden, das auf dieses Unterobjekt zeigt.

**Ergebnis -- einmalig erfasst, in einem frischen ungestoerten Lauf:**
```
this=0x01135928 databuf=0x01139998 buffer_size=0x00024000
source=0x0113D999 destination=0x0111D999 copy_length=0x0001FFFF
remaining_space=0x0001FFFF record_length=0x7E000000 header_field_a=0x00000000
read_cursor=0x00004001 total_field_4064=0x001D5F71 file_handle=0x48000005
reader_candidates: 2 gefunden -- 0x00F7B140 (ein Guest-STACK-Slot mit einer
  gesicherten Kopie von `ebx`, ein Fehlalarm -- Ruecksprungadressen
  0x40983/0x40992 liegen direkt daneben, bestaetigt Stack-Frame statt
  persistentem Feld) und 0x01105204, was exakt `0x011051C8 + 0x3C`
  entspricht -- ein echtes Objekt, dessen eigenes `+0x3C`-Feld auf unser
  Unterobjekt zeigt, deckungsgleich mit dem bereits in Update 188
  beschriebenen "Reader"-Muster.
```
`SetFilePointerEx`/`GetFinalPathNameByHandleW` auf `file_handle=0x48000005`
**schlugen beide fehl** -- kein aktuell gueltiges Win32-HANDLE an dieser
Stelle (konsistent damit, dass der Objekt-Header zu diesem spaeten,
seltenen Zeitpunkt bereits Muell/veraltet ist; nicht weiter aussagekraeftig).

**Datei/Offset empirisch statt ueber In-Memory-Strings bestimmt:** das
Reader-Objekt bei `0x011051C8` hat ein dateinamensartiges Feld etwas
weiter, das `"DataXb\\Maps\\x06_cozumel_a.unr"` liest -- das erwies sich
jedoch als Fehlspur (vermutlich ein unabhaengiger FName-/String-Pool-
Eintrag, oder ein interner PC-Original-Engine-Paketname, der in Engine-
Metadaten eingebettet ist, keine literale Datei dieses Disc-Abbilds -- eine
`.unr`-Datei existiert im extrahierten Disc-Abbild gar nicht, nur `.lin`).
Statt diesem String zu vertrauen, wurden die erfassten Rohbytes an der
Lese-Cursor-Position (`source`, also `0x24` Null-Bytes gefolgt von
`7E C4 67 1C F9 89 FC 4C C6 A3 BC 06 26 05 F0 0A ...`) woertlich im
GESAMTEN extrahierten Disc-Abbild gesucht (`tools/debug/find_wrap_bytes2.py`).
**Exakter Treffer in `Default/DataXb/LMaps/common.lin` bei Byte-Offset
`0x1C0000` (1.835.008)** -- bestaetigt die eigene Hypothese des Nutzers,
dass `record_length` auf `common.lin` zurueckgeht, nicht auf die Cozumel-
Karte. `common.lin`s Gesamtgroesse ist 1.924.977 Bytes;
`1.924.977 - 1.835.008 = 89.969` -- und das oben erfasste `total_field_4064`
(`0x1D5F71` = 1.924.977, exakt die volle Dateigroesse) zeigt, dass der
eigene laufende Zaehler des Codes an dieser Stelle bereits exakt der
kompletten Dateigroesse entspricht, d.h. diese Erfassung liegt unmittelbar
nach dem LETZTEN Chunk der Datei.

**Das ist kein neuer Bug -- es ist der bereits diagnostizierte
`common.lin`-Kurzlese-Bug aus Updates 171-186, noch nicht vollstaendig
abgeschlossen:**
- Update 171 (17.09.2026) fand genau diesen memcpy, genau diese Funktion,
  und verfolgte ihn live per cdb bis zu `common.lin`s 15. Chunk-Read --
  ein Kurzlesevorgang von 89.969 Bytes nach 14 vollen 128-KiB-(`0x20000`)-
  Chunks (`14 * 0x20000 = 0x1C0000`, exakt der heute unabhaengig per
  Byte-Suche gefundene Offset). Per Capstone byte-verifiziert gegen die
  Original-XBE bei `0x00040909`-`0x00040992`: **Uebersetzung ist exakt,
  kein Recompiler-Bug** (heute in Update 213 unabhaengig erneut bestaetigt).
- Update 173 fand den Kernmechanismus: die Schreibposition-Formel bei
  `loc_00040992` (`wpos = 0x4000 - restplatz_vor_dem_read`) **liest
  NIRGENDS die tatsaechlich uebertragene Bytezahl** -- sie geht immer davon
  aus, dass der volle angeforderte Chunk (`0x20000`) geliefert wurde,
  unabhaengig davon, was `IoStatusBlock->Information` tatsaechlich meldet.
  Bei einem echten Kurzlesevorgang werden dadurch ~41 KiB nie tatsaechlich
  beschriebener Pufferbereich trotzdem als "gueltig" markiert, und der
  naechste Datensatz-Header-Parse liest Muell aus diesem Bereich.
- Update 185 hat den genau in dieser Runde angeforderten xemu-
  Grundwahrheits-Abgleich bereits gemacht:
  `tools/debug/xemu_object_refill_wantgot.py` bestaetigte, dass echte
  Hardware **exakt denselben Kurzlesevorgang** erlebt
  (`want=0x00020000 got=0x00015F71`, bytegenau identisch zu recomp) am
  selben Objekt/derselben Position -- aber danach macht echte Hardware
  **ueberhaupt keine weiteren `sub_000516B7`-Aufrufe fuer diesen Stream
  mehr**; sie stoppt sauber. Kein Absturz auf echter Hardware.
- Update 186 hat das unehrliche Byte-Count-Reporting (`scda_disc_fill_overrun`,
  das einen Kurzlesevorgang kuenstlich auf die volle angeforderte Groesse
  auffuellte) durch `scda_disc_zero_short_read_tail` ersetzt
  (`src/kernel/kernel_file.c`, weiterhin unveraendert vorhanden, von dieser
  Sitzung bereits in Update 212s inzwischen ueberholtem Native-Code-Umweg
  gegengelesen): meldet den echten Byte-Count, nullt defensiv nur den
  unbeschriebenen Rest. Bestaetigt aktiv im aktuellen Build, erzeugt den
  sauberen `0x00000000...`-Nulllauf, den diese Sitzung heute am Anfang der
  erfassten `source`-Bytes fand.
- Trotz dieses Fixes fand Update 186, dass DERSELBE Fatal-Callback weiterhin
  feuert, damals ueber einen scheinbar korrumpierten `NtReadFile`-Handle-
  Aufruf einen Schritt spaeter, und liess das explizit offen ("noch nicht
  auf Zeilenebene isoliert"). **Genau diese fehlende Isolierung ist, was
  Update 213/214 heute liefert**: der Zero-Fill-Fix verhindert, dass
  plausibel aussehender Muell (wie das urspruenglich in Update 171
  gesehene `0xE1F9F6F3`-Handle) in den Objekt-Header geschrieben wird,
  stoppt aber nicht den zugrundeliegenden Mechanismus -- der Code versucht
  weiterhin, einen Datensatz-Header aus dem (jetzt sauber genullten, aber
  weiterhin logisch bedeutungslosen) unverbrauchten Rest zu parsen, die
  gelesene `record_length` ist weiterhin Unsinn (jetzt geformt durch
  benachbarte veraltete Ringpuffer-Inhalte statt durch
  `scda_disc_fill_overrun`s alte Auffuellung), und derselbe unterlaufende
  `memcpy` feuert weiterhin, nullt weiterhin die eigenen Header-Felder des
  Objekts (`+0x3C` Handle, `+0x4058` Groesse, `+0x405C` Pufferzeiger) --
  genau die "0x8070-Nullung", die dieser gesamte Update-190-213-Strang
  unter anderem Namen verfolgt hat.

**Direkte Antworten auf die konkreten Fragen dieser Runde:**
- **Stammt `record_length` direkt aus `common.lin`?** Ja und nein -- es
  wird an genau der Pufferposition gelesen, die auf `common.lin`-Byte-Offset
  `0x1C0000` und kurz danach abbildet, aber diese Position liegt HINTER dem
  echten 89.969-Byte-letzten-Chunk der Datei. Was dort tatsaechlich
  geparst wird, ist eine Handvoll legitim genullter Padding-Bytes (Update
  186s eigener Fix), unmittelbar gefolgt von einem einzelnen veralteten
  Rest-Byte (`0x7E`) aus einem FRUEHEREN, unabhaengigen Lesezyklus, der in
  demselben wiederverwendeten Ringpuffer-Slot zurueckblieb -- kein echter
  `common.lin`-Inhalt. Das `record_length`-Feld ist also eine Fehl-
  Interpretation von Padding-plus-Altdaten, kein jemals in `common.lin`
  vorhandener echter Wert.
- **Stammt `remaining_space` aus dem Ringpuffer-Bookkeeping?** Ja, exakt
  bestaetigt: `remaining_space = puffergroesse(0x24000) - cursor_nach_dem_
  8-byte-header(0x4001) = 0x1FFFF`, beide Felder direkt aus den
  persistenten Bookkeeping-Feldern des Unterobjekts gelesen (`+0x4058`,
  `+0x4060`), bytegenau passend zum Code.
- **Bytevergleich gegen eine andere SCDA-Kopie:** als separater Schritt
  nicht noetig -- Update 173 hat den aequivalenten Abgleich bereits gegen
  eine echte Retail-ISO gemacht (`SC4_xbe_hack/output/
  scda_graphics_fixes_30fps.iso`, common.lin bei ISO-Offset `0x24F22000`,
  vollstaendiger 1.924.977-Byte-Abgleich bestaetigt). Die Bytes auf der
  echten Disc unmittelbar nach common.lins deklariertem Ende sind selbst
  echter, deterministischer XISO-Verzeichnisinhalt (kein Padding) -- das
  erwies sich aber NICHT als der Grund, warum echte Hardware ueberlebt
  (siehe unten); Update 185 stellte spaeter fest, dass echte Hardware an
  dieser exakten Stelle ebenfalls einen sauberen Kurzlesevorgang sieht,
  keine Fortsetzung in Verzeichnisbytes.
- **Gezielter xemu-Abgleich (Datensatzlaenge, Restplatz, Cursor, Datei-
  Offset, resultierende Zieladresse):** bereits in Update 185 fuer den
  Kurzlesevorgang selbst gemacht (want/got bytegenau identisch). Echte
  Hardware erreicht den Kompaktierungs-memcpy fuer diesen Stream danach
  ueberhaupt nicht mehr -- es gibt also keine xemu-"record_length"/
  "Zieladresse" an dieser tieferen Stelle zu vergleichen; die Abweichung
  passiert eine Ebene hoeher, in dem, was entscheidet, ob `sub_00040821`/
  `sub_000516B7` nach diesem Kurzlesevorgang ueberhaupt nochmal aufgerufen
  wird.
- **Gehoert der Bereich vor dem Objekt zu einer groesseren Parent-/Arena-
  Allocation?** Dieser Zweig der Anweisung war an "falls xemu dieselben
  Eingangswerte und dieselbe Zieladresse zeigt" geknuepft -- das ist nicht
  der Fall (Update 185: xemu erreicht diesen memcpy fuer diesen Stream gar
  nicht), die Frage stellt sich also nicht und wurde diese Runde nicht
  weiterverfolgt.

**Was tatsaechlich noch ungeklaert ist (die eigentlich verbleibende
Frage):** warum macht der rekompilierte Build nach dem Kurzlesevorgang
ueberhaupt noch einen weiteren `sub_00040821`/`sub_000516B7`-Aufruf fuer
diesen Stream, waehrend echte Hardware das nicht tut? Sowohl die Kurzlese-
Bytezahlen (Update 185) als auch `sub_00040821`s eigener uebersetzter Code
(Updates 171/213, byte-verifiziert) sind identisch zur Original-XBE. Die
Schreibposition-Formel bei `loc_00040992` laeuft unbedingt, unabhaengig
vom Rueckgabestatus von `sub_000516B7` (Erfolg und Fehlschlag erreichen
beide dieselbe Stelle) -- ein einfacher "STATUS_END_OF_FILE statt
STATUS_SUCCESS fuer einen kurzen-aber-nicht-leeren Read"-Fix in
`xbox_NtReadFile` wuerde also allein nicht verhindern, dass die
Cursor-Fortschreibung falsch bleibt -- die Abweichung muss in etwas
hoeherliegendem liegen, das entscheidet, ueberhaupt erneut nach Daten zu
fragen, und wurde noch nicht lokalisiert.

**Kein Patch in dieser Runde implementiert, wie explizit angewiesen** -- der
Mechanismus des Underflows war bereits verstanden (Update 213) und ist
jetzt vollstaendig bis zu seiner realen Datenherkunft zurueckverfolgt
(dieses Update), aber der eigentliche Ausloeser -- warum recomp weiterhin
an einem erschoepften Stream zieht, den echte Hardware sauber aufgibt --
ist noch nicht gefunden, und die Symptomstelle (den memcpy selbst) zu
patchen, bevor das gefunden ist, wuerde es genauso verdecken wie
`scda_disc_fill_overrun` es einst tat (Update 185s eigener expliziter Fund).

**Naechster Test (fuer STRONG):**
1. Finden, was nach dem Kurzlesevorgang entscheidet, `sub_00040821`/
   `sub_000516B7` fuer diesen Stream erneut aufzurufen -- vermutlich ein bis
   zwei Frames oberhalb von `sub_00040821` in der bereits in Updates
   173/195/196 kartierten Aufrufkette (`sub_0004438C` und Aufrufer). Gegen
   dieselbe Aufrufkette auf xemu um Update 185s t=27.828s-Treffer
   vergleichen, um zu sehen, wo beide divergieren.
2. Pruefen, ob `sub_00040821`s eigenes `+0x44`("consumed")-artiges
   Bookkeeping-Feld, oder ein aufruferseitiges "erwartete Gesamtgroesse"-
   Feld, unmittelbar nach dem Kurzlesevorgang auf recomp vs. xemu einen
   unterschiedlichen Wert haelt -- Update 185 hat bereits diese Art Feld
   erfasst (`tools/debug/xemu_object_fileobj.py`) fuer ein aehnliches
   Objekt; dasselbe Skript auf genau diese Stelle zu erweitern gaebe einen
   direkten xemu-vs-recomp-Diff ohne neuen Erfassungsmechanismus.
3. Erst wenn diese Abweichung gefunden ist: einen Fix auf der Ebene waehlen,
   auf der der eigentliche Bug liegt (vermutlich die hoeherliegende Stream-
   Fortsetzungs-Entscheidung, nicht der memcpy selbst, und kein defensiver
   Clamp in `sub_00040821`, der das Symptom fixen wuerde, waehrend die
   eigentliche "warum stoppt recomp nicht"-Frage offen bliebe, wie
   `scda_disc_fill_overrun` bereits einmal als falsche Fix-Art fuer diese
   Bug-Familie demonstriert hat).
4. Nach gewaehltem und verifiziertem echten Fix: ALLE temporaeren
   `SCDA-DISC-ERR-INSTRUMENTATION`-Marker und die Ringpuffer-/Capture-
   Infrastruktur gemaess Standing Instruction zuruecksetzen, vollstaendige
   Recomp-Pipeline + canonical Build durchlaufen, bis zum naechsten
   Blocker weiterarbeiten.

**Modell-Stufe:** unveraendert STRONG (Update 213) -- die verbleibende
Arbeit ist ein begrenzter Abgleich gegen bereits vorhandenes xemu-Tooling
und Skripte aus Update 185, kein ergebnisoffenes Reverse-Engineering mehr.
"""

with open(path, "wb") as f:
    f.write(raw + text.replace("\n", "\r\n").encode("utf-8"))

print("done, new size", len(raw) + len(text.replace("\n", "\r\n").encode("utf-8")))
