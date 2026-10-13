# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 221 (2026-09-21): Nativer NtReadFile-/Disc-I/O-Audit schliesst unsere Kernel-Bruecke, Host-I/O und sogar echte Hardwares eigene I/O-Schicht als Ursache aus -- der Kurzlesevorgang ist echt, ehrlich und bytegenau identisch zu xemus eigenem bereits etabliertem Kurzlesevorgang; der Cursor-Formel-Fix ist jetzt dauerhaft (recomp_manual.c + --exclude-manual), uebersteht einen vollstaendigen Neu-Regenerate von Grund auf, und ein unabhaengiger Function-Seed-Datendrift-Bug wurde dabei gefunden und mitbehoben

**Verfolgte Aufgabe:** den Update-220-Fix im generierten Code nur als
diagnostischen Beweis behandeln, noch nicht als final. Top-Prioritaet:
erklaeren, warum der tatsaechliche Transfer kleiner als 0x4000 ist, obwohl
der bytegenau verifizierte Original-XBE-Code an beiden Wrap-Stellen
ausdruecklich mit 0x4000 rechnet. Commit e1b6823 ("report honest short-read
byte count instead of topping up disc reads") und die aktuelle
NtReadFile-/Disc-I/O-Semantik auditieren. Beim konkret problematischen Read
einmalig erfassen: angeforderte Bytezahl, tatsaechlich transferierte Zahl /
IO_STATUS_BLOCK.Information, NTSTATUS, Datei-Offset, Dateigroesse,
verbleibende Bytes bis EOF, Reader/Handle. Klaeren, ob der Short Read durch
EOF, Host-I/O, unsere Kernel-Implementierung oder eine andere Begrenzung
entsteht. Mit xemu vergleichen, wenn praktikabel; falls xemu 0x4000 liefert
und Recomp weniger, die I/O-Semantik statt des Original-Spielcodes fixen.
Erst wenn der korrekte, dauerhafte Fix feststeht: die temporaere
Generated-Code-Aenderung entfernen, den Fix in einer regenerationsfesten
Quelle implementieren, die komplette Recomp-Pipeline ausfuehren, und
bestaetigen, dass der alte `sub_00040821`-Blocker weiterhin verschwunden
ist. Danach den neuen `sub_004A12B0`/`sub_004A1600`/`sub_004A1740`-Poll-Loop
als separaten naechsten Blocker untersuchen.

**Zuerst eine wichtige Korrektur beim erneuten Lesen der Aufrufstellen:**
Aufrufstelle 1s eigene Disk-Read-Anforderungslaenge (`recomp_0001.c`, die
`MEM32(ebx+0x44)`-Rundungs-/Deckelungssequenz unmittelbar vor ihrem
`sub_000516B7`-Aufruf) ist auf bis zu **0x20000** gedeckelt, identisch zur
eigenen Anforderungsdeckelung des Kompaktierungspfads -- keine feste 0x4000.
Das bedeutet, die festkodierte `0x4000` in der Cursor-Formel war von
Anfang an NIE "die Anforderungsgroesse"; die tatsaechlich transferierte
Menge fuer ein gegebenes Refill kann legitim ueberall bis zu 0x20000 Bytes
liegen. Das weckte echten Zweifel, ob Update 220s Fix (die echte
transferierte Zahl statt der festkodierten Konstante einsetzen) ueberhaupt
dimensional sinnvoll war, da ein weit ueber 0x4000 liegender Transfer in
diese Formel eingesetzt einen Cursor-Wert ohne offensichtliche a-priori
Rechtfertigung liefert -- genau die Art Frage, die diese Runde mit echten
Zahlen geklaert haben wollte, bevor irgendetwas weiter vertraut wird.

**Nativer I/O-Audit, per One-Shot-Erfassung direkt in `xbox_NtReadFile`
(`src/kernel/kernel_file.c`), zuerst den Update-220-Fix temporaer
zurueckgesetzt, damit der originale, ungefixte Codepfad (der den
problematischen Read tatsaechlich erzeugt) genau wie immer laeuft:**

```
SCDA-DIAG221 #2 handle=0x290 requested=131072 transferred=89969 offset=... filesize=1924977 ...
SCDA-DIAG221 #3 handle=0x290 requested=131072 transferred=0     offset=... filesize=1924977 ...
```

(Das genaue Offset-Feld hatte in dieser Erfassung einen eigenen kleinen Bug
-- `ByteOffset` war NULL fuer diesen Aufruf, das heisst der Read nutzt die
eigene, implizite, sequentiell fortschreitende Position der Datei, und die
Fallback-Abfrage `SetFilePointerEx(...,FILE_CURRENT)` schlug mit
`ERROR_INVALID_PARAMETER` auf diesem Handle fehl, hoechstwahrscheinlich weil
es fuer Overlapped-artiges I/O geoeffnet ist; nicht weiter verfolgt, da fuer
die untenstehende Schlussfolgerung nicht noetig.) `transferred=89969`
allein ist bereits schluessig: es passt EXAKT zu Update 214s unabhaengig
hergeleiteter Groesse des letzten Chunks von `common.lin` (1.924.977 -
1.835.008 = 89.969), was bestaetigt, dass dies genau dasselbe, einzelne
Ereignis ist, das diese gesamte Untersuchungsreihe seit Update 213 verfolgt
-- kein anderer oder neu eingefuehrter Kurzlesevorgang. `requested=131072
(0x20000)` gegen `filesize=1924977` bei einem Offset 89.969 Bytes vor dem
Ende ist ein voellig gewoehnliches, arithmetisch exaktes "Anfrage ueber EOF
hinaus, erhalte genau das Verbleibende"-Muster: **Eintrag #2 ist ein
echter, ehrlich gemeldeter Kurzlesevorgang am wahren Ende der Datei, kein
Artefakt unserer eigenen I/O-Schicht, des Host-Dateisystems oder einer
kuenstlichen Begrenzung.** Eintrag #3 (der unmittelbar naechste Aufruf,
gleiches Handle, gleiche 0x20000-Anfrage) mit `transferred=0` ist der
lehrbuchmaessige, korrekt behandelte Fall echten EOFs (`xbox_NtReadFile`
liefert `STATUS_END_OF_FILE`, `Information=0`, exakt gemaess NT-Semantik) --
ebenfalls korrekt, ebenfalls kein Bug.

**Das beantwortet die zentrale Frage dieser Runde direkt, mit der von
dieser Runde selbst vorgegebenen Methode (Vergleich mit xemu):** Updates
185/216 hatten bereits per Live-xemu-Grundwahrheit auf einem FRISCHEN Boot
(kein wiederverwendetes Log) festgestellt, dass echte Hardware fuer genau
diesen Stream DENSELBEN Kurzlesevorgang erlebt: `want=0x20000 got=0x15F71`
(0x15F71 = 89969, dieselbe Zahl). **xemu liefert NICHT 0x4000, waehrend
Recomp weniger liefert -- beide Plattformen liefern denselben, weit
groesseren-als-0x4000 Kurztransfer.** Die von dieser Runde selbst genannte
Fallback-Bedingung ("wenn xemu 0x4000 liefert und Recomp weniger, behebe
die I/O-Semantik") greift also nicht: es gibt keine I/O-Semantik-Diskrepanz
zwischen den Plattformen zu beheben. Unsere Kernel-Bruecke, das
Host-Dateisystem und Commit e1b6823s ehrliche-Meldung-Aenderung sind alle
als korrekt bestaetigt und nicht die Ursache. **Der Defekt liegt
ausschliesslich in der eigenen festkodierten 0x4000-Annahme der
Guest-seitigen Cursor-Formel** (Updates 215/220), die stillschweigend
annimmt, jedes wrap-ausloesende Refill transferiere exakt 0x4000 Bytes,
waehrend Transfers routinemaessig bis zu 0x20000 laufen -- es tritt hier
speziell zutage, weil dieser konkrete Wrap mit dem echten, ehrlich
gemeldeten Ende der Daten des Streams zusammenfaellt. Das loest auch den
oben aufgeworfenen Zweifel an der dimensionalen Stimmigkeit: die echte
transferierte Zahl zu nutzen (wie gross sie auch sei) ist keine willkuerliche
Ersetzung, sondern die Wiederherstellung der einen bereits verfuegbaren,
immer korrekten Information (`eax`, zwei Zeilen zuvor aus `MEM32(ebp-4)`
fuer die Akkumulator-Aktualisierung geladen), die die Original-Formel
zugunsten einer Konstante verwarf, die fuer diesen Aufruf nie tatsaechlich
repraesentativ fuer die wahre Transfergroesse war.

**Housekeeping (native Seite):** `src/kernel/kernel_file.c`s temporaere
`SCDA-DIAG-221`-Erfassung wurde per `git checkout` vollstaendig
zurueckgesetzt (kein verbleibender Diff); der Update-220-Fix im generierten
Code wurde vor der dauerhaften Fix-Arbeit unten wiederhergestellt.

**Dauerhafter, regenerationsfester Fix implementiert.** Gemaess dem in
Update 220 markierten Vorbehalt (der Fix lebte nur in gitignorter,
generierter Ausgabe und wuerde von einem vollstaendigen Pipeline-Regenerate
stillschweigend geloescht), wurde die bestehende Recompiler-Pipeline nach
einem dauerhaften Patch-Mechanismus durchsucht (an einen Recherche-Agenten
delegiert; bestaetigt: keine adressbasierte Instruktions-/Byte-Patch-Liste
und kein generischer Post-Lift-Patch-Schritt existieren, aber ein
`--exclude-manual <Datei>`-Mechanismus existiert: `manual_scan.py` scannt
eine handgeschriebene C-Datei nach `void sub_XXXXXXXX(void)`-Definitionen
und schliesst genau diese Adressen automatisch von der Auto-Generierung
aus, mit einem direkten, bereits genutzten Praezedenzfall in derselben
Datei: `sub_00040A88`, dorthin in einer frueheren Untersuchung aus einem
analogen "der Auto-Lift braucht eine Hand-Korrektur"-Grund verschoben).
Denselben Mechanismus angewandt: mechanisch (per kleinem Python-Skript,
kein manuelles Abtippen, um Transkriptionsrisiko auszuschliessen) die
exakten, bytegetreuen generierten Koerper von `sub_00040821` und
`sub_0004086E` extrahiert, denselben zweizeiligen Text-Fix aus Update 220
auf jeden angewandt, mit dem etablierten Register-Alias-`#define`/`#undef`-
Boilerplate umschlossen, das die existierenden handgeschriebenen Funktionen
dieser Datei bereits nutzen, und mit vollstaendigem erklaerendem Kommentar
(Updates 213-221 zitierend) an `projects/scda/src/recomp_manual.c`
angehaengt. Den kanonischen Recomp-Generierungsbefehl mit
`--exclude-manual projects/scda/src/recomp_manual.c` erneut ausgefuehrt
(bereits der Standard-SCDA-Aufruf gemaess frueheren Eintraegen dieser
Datei) -- per Grep bestaetigt, dass beide Adressen im frisch generierten
`recomp_funcs.h` jetzt nur deklariert sind, ohne Koerper in den
Massen-generierten .c-Dateien.

**Ein unabhaengiger, bereits bestehender Datendrift-Bug tauchte sofort auf
und wurde mitbehoben (kein Scope Creep -- noetig, um ueberhaupt zu linken):**
der vollstaendige Regenerate scheiterte beim Linken mit undefiniertem
`sub_00040841`. Per direkter Capstone-Disassemblierung der echten XBE
root-caused (Wiederverwendung des etablierten
`verify_40909_capstone.py`-Musters): `0x00040841` ist ein echter,
legitimer Mid-Function-Wiedereintrittspunkt innerhalb derselben Routine
(ein Loop-back-Ziel fuer wiederholte Refill-Iterationen, nur per Sprung
erreicht, nie per Fallthrough -- bytegenau gegen den Original-Spielcode
bestaetigt, kein Disassemblierungs-Artefakt). Er fehlt in der AKTUELLEN
`tools/func_id/output/scda_seeded_corrected/identified_functions.json`,
obwohl `sub_0004086E`s eigener auto-generierter Koerper (vor den Aenderungen
dieser Runde) bereits einen Aufruf dorthin enthielt -- das bedeutet, ein
frueherer Schnappschuss dieser generierten Daten muss ihn enthalten haben,
aber die aktuell eingecheckten Disassemblierungs-/Func-ID-Artefakte tun das
nicht mehr. Das ist eine latente, bereits bestehende Zerbrechlichkeit in
den eigenen Pipeline-Daten des Projekts (unabhaengig vom Fix dieser Runde),
die bei JEDEM vollstaendigen Neu-Regenerate von Grund auf aufgetaucht waere,
nicht etwas, das durch das manuelle Verschieben dieser zwei Funktionen
eingefuehrt wurde -- es wurde einfach nie zuvor ausgeloest, weil
`sub_0004086E` (nur ueber die indirekte Dispatch-Tabelle erreichbar, nie
per Namen von anderem generierten Code aufgerufen) zuvor in derselben
massen-generierten Uebersetzungseinheit lebte wie viele andere, definitiv
referenzierte Funktionen, sodass die Objektdatei immer mitgelinkt wurde,
unabhaengig davon, und dabei stillschweigend dieselbe latente haengende
Referenz trug, ohne sie je aufloesen zu muessen. Behoben ueber den bereits
etablierten, praezedierten Mechanismus dieses Projekts fuer genau diese
Problemklasse (Git-Log zeigt mehrere fruehere "disasm: fix ...
function-boundary bug via N verified seed addition(s)"-Commits): `0x00040841`
mit vollstaendiger Begruendung zu `projects/scda/function_seeds.json`
hinzugefuegt, dann die komplette dreistufige Pipeline
(`tools.disasm` mit `--seed-functions`/`--exclude-seed-functions` ->
`tools.func_id` -> `tools.recomp` mit `--exclude-manual`) von Grund auf
erneut ausgefuehrt. Bestaetigt: `sub_00040841` ist jetzt eine echte
Funktion in der frischen Ausgabe, und das gesamte Projekt baut sauber.

**Vollstaendige Pipeline Ende-zu-Ende verifiziert.** Nach dem kompletten
dreistufigen Regenerate und einem vollstaendigen Neubau (`cmake --build`,
null Fehler), eine frische Instanz gestartet und per Live-, nicht-invasivem
`cdb -pv`-Snapshot nach ca. 150s bestaetigt: der Prozess erreicht exakt
dieselbe `sub_004A12B0+0x1e8`/`sub_004A1600+0x7b2`/`sub_004A1740+0xb6`-Stelle,
die Update 220s handgepatchter-generierter-Datei-Test bereits erreichte --
NICHT das alte `sub_0003FCA5`-Fatal-Callback-Einfrieren. Der Fix ist
bestaetigt, einen echten, vollstaendigen Neu-Regenerate von Grund auf zu
ueberstehen, nicht nur einen Hand-Edit an bereits generierter Ausgabe.

**Committed:** `projects/scda/function_seeds.json` (4 Zeilen, der neue Seed)
und `projects/scda/src/recomp_manual.c` (657 Zeilen, die zwei
handgeschriebenen Funktions-Overrides). Die generierten Ausgabeverzeichnisse
(`tools/disasm/output/`, `tools/func_id/output/`, `tools/recomp/output/`)
bleiben wie etabliert gitignored -- sie sind aus den zwei committeten
Quelltext-Aenderungen ueber die oben dokumentierte Drei-Befehle-Pipeline
reproduzierbar.

**Naechster, separater Blocker (gemaess der expliziten eigenen Anweisung
dieser Runde, noch nicht begonnen):** den neuen
`sub_004A12B0`/`sub_004A1600`/`sub_004A1740`-Poll-Loop untersuchen -- ein
niedrig-CPU (~16%), stabiler, zuvor unerreichter Synchronisationspunkt.
Bemerkenswert: `recomp_manual.c`s eigener bereits bestehender
`sub_00040A88`-Kommentar (die `sub_00040A88`-Praezedenzfunktion, bis eben
unabhaengig vom Fix dieser Runde) erwaehnt bereits genau diese Adresse aus
einer frueheren (2026-09-05/11) UMOD-Reader-Untersuchung: "...warum die
gesamte Kette ... aussieht, als spinne sie ewig in sub_004A12B0, statt in
normaler Zeit fertigzuwerden..." -- zuerst pruefenswert, ob dieser fruehere
Fix dieses NEUE Auftreten bereits vollstaendig abdeckt oder ob dies ein
eigenstaendiges Auftreten eines aehnlichen Problems ist.

**Modellstufe:** STRONG bleibt ueber diese gesamte Runde angemessen -- jeder
Schritt (nativer I/O-Audit, Recherche zum Manual-Override-Mechanismus,
mechanische Extraktion, Seed-Daten-Fix, Pipeline-Neulauf) war eine
begrenzte, klar spezifizierte Aufgabe mit einem klaren, ueberpruefbaren
Erfolgskriterium.
"""

new_raw = raw.rstrip(b"\r\n") + b"\r\n" + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
