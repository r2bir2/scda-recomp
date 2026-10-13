# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 223 (2026-09-21): KORREKTUR zu Update 222 -- kein fehlendes Video-Backend, sondern ein echter Speicherkorruptions-Bug; Live-Breakpoints beweisen, dass die Geraeteerstellung erfolgreich laeuft und der bestehende NV2A_ACK-Mechanismus bereits ausreicht, aber MEM32(0x4AB668) wird zwischen zwei guten Aufrufen und einem schlechten mit Muell ueberschrieben

**Ausloeser:** Nutzer bat darum, mit dem Aufbau eines "minimalen
Video-Backend-Stubs" zu beginnen, basierend auf Update 222s Einschaetzung
("kein Video-Backend verdrahtet"). Vor dem Schreiben von Stub-Code wurde
`projects/scda/CMakeLists.txt`s eigene, umfangreiche Vorgeschichte
(2026-09-04 bis 2026-09-11) zu genau dieser `sub_004A12B0`-Adresse geprueft,
die Update 222 nicht querverwiesen hatte. Diese Vorgeschichte widerspricht
der "fehlendes Backend"-Einschaetzung eindeutig und warnt explizit vor genau
der Art von Fix, die ein Stub waere.

### Was fruehere Sitzungen bereits herausgefunden hatten (von Update 222 uebersehen)

1. `src/kernel/xbox_memory_layout.c` implementiert bereits einen
   funktionierenden, generischen NV2A-Bestaetigungsmechanismus:
   `NV2A_ACK[]` (eine Tabelle von `{Register-Offset, Busy-Maske}`-Paaren,
   bedient von einem Hintergrund-Thread `nv2a_ack_thread`) enthaelt bereits
   `{0x100410, 0x00010000u}` -- Kommentar: "PFB flush kick, Halo
   0x001EF930" -- d.h. **genau das Register, das `sub_004A12B0` abfragt,
   wird bereits behandelt**, hinzugefuegt fuer ein anderes Spiel mit
   demselben Hardware-Verhalten.
2. Ein eigenes xemu-Ground-Truth-Skript
   (`tools/debug/xemu_004a12b0_devptr_check.py`, noch vorhanden,
   wiederverwendbar) hatte die Frage "fehlt hier Hardware-Emulation?" fuer
   genau diesen Spin bereits beantwortet: auf echter Hardware ist
   `MEM32(0x4AB668) = 0x004AB670` (ein gueltiger Geraete-Struct) und
   `struct+0x1C28 = 0xFD000000` (die echte NV2A-Aperture-Basis) **jedes
   einzelne Mal**, ~500/500 Treffer. Das reale PFB-Register wird von echter
   Hardware beantwortet; unsere `NV2A_ACK`-Tabelle reproduziert das bereits
   korrekt, sobald die Zeigerkette gueltig ist.
3. Dieselbe fruehere Untersuchung (vor der "Update N"-Nummerierung, der
   "continued 52"-Eintrag) fand `scda_recomp` an demselben Spin mit
   `MEM32(0x4AB668) = 0x0C47C708` -- nicht Null, nicht der gueltige
   Geraete-Zeiger, sondern ein Streuwert, der zufaellig in `.text` liegt --
   und hielt explizit fest: **"forcing the device-pointer chain to a fake
   value here would be exactly the kind of workaround this project rules
   out."** Der empfohlene, nie abgeschlossene naechste Schritt war, zu
   verfolgen, was auf diesem Call-Pfad Muell in `0x4AB668` schreibt.

Das bedeutet: Update 222s "kein Video-Backend verdrahtet"-Einschaetzung
trifft zwar auf den *urspruenglichen* 2026-09-11-Befund zu (`MEM32(0x4AB668)`
las sich waehrend eines ganzen 10-Minuten-Soaks als `0`, vor dem
Cursor-Formel-Fix), beschreibt aber die *aktuelle* Situation ueberhaupt
nicht: es existiert bereits substantielle NV2A-Bring-up-Arbeit, die fuer
genau dieses Register in genau dieser Funktion bereits als ausreichend
befunden wurde, von einer Sitzung, die weiter kam als Update 222.

### Live-Nachpruefung diese Runde

Eine echte (nicht `-pv`, Breakpoint-basierte) `cdb`-Sitzung aufgebaut, die
einen frischen `scda_recomp.exe` direkt unter dem Debugger startet
(`cdb -cf <skript> scda_recomp.exe`, nicht `-pv -p <pid>` -- echte
Breakpoints erfordern den Besitz der Debug-Sitzung), mit Breakpoints auf
`scda_recomp!sub_0049D210` (Geraeteerstellung) und `scda_recomp!sub_004A12B0`
(die Poll-Schleife), die jeweils `MEM32(0x4AB668)` (uebersetzt ueber den
Guest-Speicher-Offset `0x0000700000000000` dieses Builds) und einen kurzen
Stack ausgeben und dann automatisch fortsetzen (`g`).

Ergebnis, drei Treffer in Reihenfolge:

1. `sub_0049D210`-Eintritt, aufgerufen ueber `sub_002E2BB1 -> sub_002E75CE
   -> sub_0004934E` (genau die Kette, die die fruehere "continued
   38"-xemu-Arbeit als echten Hardware-Ausloeser fuer die
   Geraeteerstellung identifizierte) -- die Geraeteerstellung laeuft.
2. `sub_004A12B0`, aufgerufen ueber `sub_004A13D0 -> sub_004A2220 ->
   sub_0049D210` (direkt im Anschluss an die Geraeteerstellung):
   `MEM32(0x4AB668) = 0x004AB670` -- **der korrekte, gueltige
   Geraete-Zeiger**, passt exakt zu echter Hardware und zu der Konstante,
   die `sub_0049D210` selbst bei Erfolg schreibt.
3. `sub_004A12B0` erneut, aufgerufen ueber `sub_0049B0C0 -> sub_004A1740 ->
   sub_004A1600` (ein anderer, weiter unten liegender Call-Pfad):
   `MEM32(0x4AB668) = 0x7400087D` -- **derselbe Muellwert, den Update 222**
   **erfasst hatte**, jetzt bestaetigt als AUFTRETEND NACH zwei bestaetigt
   guten Lesevorgaengen, nicht von Anfang an.

**Die Geraeteerstellung ist nicht das Problem, und der bestehende
NV2A-Bestaetigungsmechanismus ist nicht das Problem. Etwas ueberschreibt ein
zuvor gueltiges `0x4AB668` zwischen Treffer #2 und Treffer #3 mit Muell.**
Das ist ein Korruptions-Bug, strukturell dieselbe Klasse wie jeder andere in
dieser gesamten Sitzung behobene Bug (die Ringbuffer-Cursor-Formel, die
Disc-Error-Falle usw.) -- kein neues Subsystem.

### Erster Versuch, den Schreibzugriff zu fangen: nicht eindeutig, aber kein Holzweg

Erneut frisch unter `cdb` mit einem Hardware-Schreib-Breakpoint
(`ba w4 <Host-Adresse von 0x4AB668>`) gestartet, von Prozessbeginn an
scharf. Er loeste korrekt einmal aus, beim selben legitimen
`sub_0049D210`-Schreibzugriff von oben (Wert `0x004AB670`), was beweist,
dass Breakpoint und Adressuebersetzung beide korrekt sind. Er loeste in
zwei separaten Beobachtungsfenstern von je ca. 2-3 Minuten kein zweites
Mal aus, obwohl ein anschliessender nicht-invasiver `cdb -pv`-Schnappschuss
desselben laufenden Prozesses bestaetigte, dass er zu dem Zeitpunkt bereits
den korrumpierten Spin erreicht hatte (`sub_004A12B0+0x1e8`, Stack zeigt
`0x7400087D`). Die korrumpierende Aenderung an diesem Speicher geschah also,
ohne einen 4-Byte-ausgerichteten Schreib-Watchpoint darauf auszuloesen --
konsistent mit der eigenen fruehen Erfahrung dieses Projekts zu genau dieser
Frage (`continued 53`s cdb-Hardware-Watchpoint-Versuche wurden ebenfalls
"defeated by tooling/environment issues rather than a clean negative
result"). Noch nicht geloest; naechste Kandidaten-Erklaerungen: ein
breiterer/anders ausgerichteter Store, der diese 4 Bytes trotzdem
ueberlappt, aber vor der Synchronisation des Breakpoints auf einen spaeter
erstellten Thread eingerichtet wurde, ein Nicht-CPU-Store-Schreibpfad
(unwahrscheinlich, aber nicht ausgeschlossen), oder schlicht ein laengeres
Beobachtungsfenster / mehrere Versuche angesichts der gut dokumentierten
Lauf-zu-Lauf-Nichtdeterminismus dieses Projekts beim Thread-Scheduling.

### Explizite Korrektur und Empfehlung

**Update 222s Einschaetzung "braucht ein neues Video-Backend-Subsystem,
zuerst mit dem Nutzer scopen" wird zurueckgezogen.** Der richtige naechste
Schritt ist dieselbe Art von Root-Cause-Suche, die diese Sitzung bereits
sechsmal erfolgreich durchgefuehrt hat: herausfinden, was zwischen den
beiden oben bestaetigten Aufrufen in `0x4AB668` schreibt (am
wahrscheinlichsten ein weiterer Guest-Schreibzugriff ueber einen
falsch berechneten/veralteten Zeiger, da `sub_0049D210`s eigene drei
statische Schreibzugriffe von der fruehereren Sitzung bereits per
statischem Grep ausgeschlossen wurden). **Explizit nicht empfohlen:** einen
"Video-Backend-Stub" zu schreiben, der `0x4AB668` auf einen gefaelschten
gueltigen Wert zwingt -- das wurde in genau dieser Codebasis fuer genau
diese Adresse bereits sinngemaess versucht und namentlich verworfen, aus
gutem Grund (es wuerde den echten Bug verdecken statt ihn zu beheben, und
der bestehende `NV2A_ACK`/`nv2a_ack_thread`-Mechanismus beweist bereits,
dass keine neue Hardware-Emulation noetig ist, sobald der Zeiger gueltig
ist).

### Prozess-Hygiene

Zwei temporaere `cdb`-Kommandoskripte nur im Scratchpad-Verzeichnis
verwendet (`cdb_devcreate_check.txt`, `cdb_write_watch_4ab668.txt`); keine
Quelldateien diese Runde angefasst. Logs zur Referenz behalten:
`tools/debug/devcreate_check_out.log`,
`tools/debug/write_watch_4ab668_out.log`. `git diff --stat` bestaetigt
keine Quelltextaenderungen. Alle `scda_recomp.exe`/`cdb.exe`-Instanzen am
Ende dieses Eintrags beendet.
"""

new_raw = raw.rstrip(b"\r\n") + b"\r\n" + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
