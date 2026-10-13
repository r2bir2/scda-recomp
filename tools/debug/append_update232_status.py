# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 232 (2026-09-22): URSACHE GEFUNDEN UND BEHOBEN fuer den Menu-Tree-Node-Pointer-Bug (Updates 230-231) -- ein Codegenerator-Bug in `_fixup_icall_esp_save`s FPO-Prolog-Erkennung, kein Daten-/Pointer-Problem

Direkte Fortsetzung von Update 231s Live-Bisektions-Plan, nach derselben
stehenden Anweisung (den genauen Call-Uebergang eingrenzen, an dem `ebx`
von `0` auf den beobachteten Wert `0x00390037` kippt, Checkpoint fuer
Checkpoint live, kein reines statisches Raten mehr, sobald Live-Bisektion
moeglich ist).

### Bisektion: von sub_00048B15 hinunter bis sub_0026940F

Temporaere Ein-Schuss-Instrumentierung (in den gitignoreten generierten
`.c`-Dateien, durch eine vollstaendige Pipeline-Regenerierung danach
komplett zurueckgesetzt) setzte aufeinanderfolgende Checkpoints entlang
der in Update 231 identifizierten Aufrufkette, mit jeder Runde enger:

1. `sub_00048B15`s eigener Koerper (CP0-CP6): `ebx` bleibt an jeder
   geprueften Stelle `0`, auch am Tail-Call-Uebergang zu `sub_0004934E`
   -- bestaetigt sauber, passend zu Update 231s statischer Lesart.
2. Erkannt: `sub_0004934E` hat **zwei unabhaengige Eintrittspfade** zur
   Array-Loop-Call-Stelle, die `sub_00062237` erreicht: den Tail-Call von
   `sub_00048B15` (selten -- genau einmal pro Prozesslauf beobachtet, ein
   einmaliger Init-Pfad), und eine Fallthrough-/Tail-Call-Kette durch
   `sub_00048F1D` -> `sub_000491BA` -> `sub_000492FA` -> `sub_00049336`,
   die tatsaechlich der Pro-Frame-Update-Pfad des Spiels ist (diese
   Funktionsfamilie hat im Original-Binary gar keine echte
   Call/Return-Grenze -- `sub_00048B15` bis `sub_0004934E` sind eine
   einzige zusammenhaengende Prozedur, rein aus
   Uebersetzungseinheit-Groessengruenden in separate C-Funktionen
   aufgeteilt). `ebx` war live an jedem dieser Einstiegspunkte `0`.
3. `sub_0004934E`s eigenen ~3000-Zeilen-Koerper bisektiert (Viertel-/
   Halb-/Dreiviertel-Checkpoints, dann zunehmend feiner) und den Umkipp
   auf zwei benachbarte Checkpoints bei Adressen `0x00049FC7` (noch `0`)
   und `0x0004A0E9` (bereits `0x00390037`) eingegrenzt -- ein Fenster von
   ~190 Zeilen.
4. Innerhalb dieses Fensters grenzten Pro-Callee-Checkpoints weiter ein:
   `ebx` ist noch `0` nach Rueckkehr von `sub_004528E7`, und bereits
   `0x00390037` nach Rueckkehr von `sub_001D8842`. `sub_001D8842` ist der
   direkte Verursacher-Call.

### Innerhalb von sub_001D8842: eine echte 8-Byte-Stack-Imbalance, empirisch gemessen

`sub_001D8842`s eigenen Prolog und Epilog instrumentiert, um `esp` und
die rohen Stack-Inhalte an dessen gesicherten Register-Slots
aufzuzeichnen. Prolog: `esp=0x00F7B710`, Slot-Werte `edi=0,
esi=0x00F7B798, ebx=0` (korrekt). Epilog: `esp=0x00F7B718` -- **8 Byte
hoeher als das esp des Prologs**, eine direkt gemessene, eindeutige
Stack-Imbalance, kein Raten. Bisektion mit Viertel-Punkt- und dann
feineren esp-Delta-Checkpoints (alle lesen `esp - <Prolog-esp>`, erwartet
`0` durchgehend bei einer balancierten Funktion) grenzte die Einfuehrung
dieses `+8` auf genau einen Aufruf ein: `sub_0026940F`. Checkpoints
unmittelbar vor und nach diesem spezifischen Aufruf (in `sub_001D8842`)
bestaetigten, dass das gesamte `+8` innerhalb von `sub_0026940F` selbst
entsteht -- nicht davor oder danach.

### Innerhalb von sub_0026940F: Byte-genauer Disassemblierungs-Abgleich, dann eine zweite empirische Bisektion

`sub_0026940F` (301 Bytes, `capstone`, Byte-genau gegen `default.xbe`)
komplett disassembliert und jede Instruktion gegen den generierten
C-Code verglichen -- strukturell identisch, einschliesslich des
abschliessenden `pop ecx; pop edi; mov [...],eax; pop esi; ret`
(entspricht exakt `POP32(ecx); POP32(edi); ...; POP32(esi); esp+=4;
return;`). Die Funktion fuehrt 10 identisch geformte indirekte Aufrufe
aus (`ecx = MEM32(g); eax = MEM32(ecx); ...; call [eax+0x14]`) gegen
dasselbe Objekt, 9 davon gefolgt von `push eax; mov ecx,<const>; call
sub_000D28EC` und der 10. von `push eax; call sub_000678A4` stattdessen.
Ein erster Durchgang verdaechtigte `sub_000678A4` (das zweimal per
Tail-Jump durch `sub_00368B0E` in `sub_00368AAF` springt, eine kleine
Ganzzahl-Parsing-Routine) oder `sub_000D28EC` (9x aufgerufen) -- beide
wurden direkt instrumentiert (Ein-/Austritts-esp, Pro-Aufruf-Zaehler) und
erwiesen sich als **bei jedem einzelnen Aufruf perfekt balanciert**,
einschliesslich aller 9 echten Aufrufe von `sub_000D28EC` aus genau
dieser Schleife (identifiziert anhand ihrer unverwechselbaren
ecx-Konstanten, herausgefiltert aus 11154 Gesamtaufrufen an diesen
gemeinsam genutzten, im gesamten Lauf breit wiederverwendeten Helfer).
Damit blieben nur die 10 indirekten Aufrufe selbst; das Ausgeben jedes
aufgeloesten Ziels bestaetigte, dass **alle 10 denselben vtable-Slot,
dasselbe Objekt, denselben Wert lesen** (`0x000460C5`) -- schloss "je
nach Aufruf verhaelt sich derselbe Callee anders" aus und deutete auf den
Aufloesungsmechanismus selbst.

### Die tatsaechliche Ursache: ein nicht registriertes indirektes Call-Ziel loest korrekt den RECOMP_ICALL_SAFE-Fallback aus, aber dessen gemerkter Ruecksetzpunkt wurde zu frueh erfasst -- vom Code-GENERATOR, nicht von handgeschriebenem Code

`0x000460C5` ist in dieser Rekompilierung kein registrierter
Funktionseinstieg -- die Adresse faellt mitten in `sub_000460BB`s
eigenen Adressbereich (eine andere, separat benannte Funktion, die ein
paar Bytes frueher beginnt), sodass `recomp_lookup()` sie nicht findet.
Direktes Anfragen aus generiertem Code (`recomp_lookup_manual` /
`recomp_lookup` / `recomp_lookup_kernel`, alle liefern null) bestaetigte
es: `RECOMP_ICALL_SAFE` nimmt seinen vorgesehenen Sicherheits-Fallback
fuer ein nicht aufgeloestes, aber im Code-Bereich liegendes Ziel --
`g_esp = saved_esp; eax = 0;` -- bei allen 10 Aufrufen, jedem Lauf.
Dieser Fallback ist grundsaetzlich korrekt; `saved_esp` ist genau der
Wert `_icall_esp`, den der Codegenerator ueber `_fixup_icall_esp_save()`
in `tools/recomp/translator.py` einfuegt, gedacht, um unmittelbar vor den
eigenen Argument-Pushes des Aufrufs erfasst zu werden, damit eine
fehlgeschlagene Aufloesung sauber *nur* diese Argumente zurueckrollen
kann.

Bei 9 der 10 Aufrufe in dieser Funktion passiert genau das. Beim
*ersten* Aufruf speziell platziert `_fixup_icall_esp_save()` die
Sicherung einen Schritt zu frueh -- vor dem einmaligen
Register-Erhaltungspaar `PUSH32(esp, esi); PUSH32(esp, edi);` der
Funktion, nicht danach. Ursache in `_fixup_icall_esp_save()` selbst
gefunden (durch Lesen bestaetigt, nicht geraten): die Funktion sucht
einen fuehrenden FPO-artigen Register-Sicherungslauf, indem sie ab der
allerersten Zeile nach dem `RECOMP_TRACE_PC()` des Eintrittslabels
vorwaerts scannt und dabei eine `PUSH32(esp, ebx|esi|edi)`-Zeile
*unmittelbar* dort verlangt. `sub_0026940F`s Koerper hat zuerst zwei
gewoehnliche Setup-Zeilen (`ecx = MEM32(g); eax = MEM32(ecx);`, den von
allen 10 Aufrufen gemeinsam genutzten vtable-Fetch), bevor die beiden
Sicherungen kommen -- der Scan sieht also zuerst eine Nicht-Push-Zeile
und stoppt sofort, laesst `prologue_end_idx` bei 0 und gibt dem
rueckwaerts scannenden Icall-Sicherungs-Scan freie Bahn, an den
Sicherungen vorbeizulaufen. Die *andere* Schutzmassnahme der Funktion --
einen Push als "Sicherung" zu behandeln, wann immer er nicht oefter
gepusht als gepoppt wird -- uebersieht diesen speziellen Fall ebenfalls,
weil sowohl `esi` als auch `edi` separat als wiederholter
Konstanten-Argumentwert ueber die anderen 9 Aufrufe in derselben Funktion
wiederverwendet werden: jedes wird insgesamt 11-mal gepusht und nur
einmal gepoppt (die echte Epilog-Wiederherstellung), sodass sie allein
nach Zaehlung genau wie Argumente aussehen, nicht wie Sicherungen.
Per Disassemblierung bestaetigt, dass diese beiden Bedingungen (fuehrende
Nicht-Push-Setup-Zeilen, plus ein Callee-Saved-Register, das spaeter in
derselben Funktion vielfach als woertliches Argument wiederverwendet
wird) gemeinsam in echtem, unauffaelligem Spielcode auftreten koennen
und auftreten -- kein hypothetischer Grenzfall.

Da `_icall_esp` vor statt nach den `esi`/`edi`-Sicherungen erfasst wird,
verwirft der Fallback (`g_esp = saved_esp`) bei der Aufloesungs-
fehlschlag des allerersten Aufrufs diese beiden Pushes -- 8 Byte --
dauerhaft aus `sub_0026940F`s Vorstellung von der eigenen Stack-Tiefe
fuer den Rest seiner Ausfuehrung. Jeder weitere Push/Pop in der Funktion
(alle 9 verbleibenden Aufrufe, jeder fuer sich genommen korrekt) laeuft
nun 8 Byte daneben von dort, wo der eigene Epilog der Funktion sie
erwartet, und dessen `pop edi; pop esi;` ganz am Ende liest zwei
Stack-Slots, die nicht mehr diejenigen sind, die er gesichert hat --
Datenmuell wird in das geladen, was das wiederhergestellte `edi`/`esi`
sein sollte, zurueckgegeben an den Aufrufer `sub_001D8842`, dessen
eigenes `pop ebx` im Epilog (an einem nun ebenfalls dauerhaft um dieselben
8 Byte verschobenen Offset) ist, wo `ebx = 0x00390037` tatsaechlich
erzeugt wurde -- viele Aufrufe, bevor es je `sub_00062237` erreicht.

### Der Fix

`tools/recomp/translator.py`, `_fixup_icall_esp_save()`: eine
Ueberspring-Schleife vor der bestehenden FPO-Sicherungs-Erkennungs-
schleife hinzugefuegt, die ueber fuehrende Zeilen hinweggeht, die reines
Register-/Speicher-Lade-Setup ohne Stack- oder Kontrollfluss-Effekt sind
(kein `esp`, `PUSH32`, `POP32`, `RECOMP_`, `goto`, `return`, `if (`, oder
ein Label), und stoppt, sobald sie entweder ein `PUSH32(esp,
ebx|esi|edi)` findet (uebergibt unveraendert an die bestehende
Erkennungsschleife) oder etwas anderes (faellt auf das heutige,
unveraenderte Verhalten zurueck). Dies ist strikt additiv: es erweitert
nur, was als "Beginn des FPO-Prologs" zaehlt, aendert aber nicht, wie
dieser Prolog nach dem Finden validiert wird (der bestehende
Push/Pop-Zaehlabgleich gilt weiterhin).

Einen vierten Regressionstest zu `tools/recomp/test_icall_esp_save.py`
hinzugefuegt, `test_fpo_save_after_vtable_fetch_setup`, direkt nach
`sub_0026940F`s echter Struktur modelliert (zwei Setup-Zeilen, zwei
FPO-Sicherungen, zehn Aufrufe mit zwei der gepushten Register als
wiederholten Argumenten wiederverwendet) -- prueft, dass die
`_icall_esp`-Sicherung nach beiden Setup-Zeilen UND beiden FPO-
Sicherungen landet, nicht vor einer von beiden. Alle vier Tests in der
Datei bestehen (`py -3 tools/recomp/test_icall_esp_save.py`).

### Verifiziert: vollstaendige Pipeline-Regenerierung, sauberer Rebuild und ein Live-Retest

Die gesamte SCDA-Ausgabe ueber den kanonischen Recomp-only-Befehl
regeneriert (bestehendes `scda_v060`-Disassembly, `--exclude-manual
projects/scda/src/recomp_manual.c`) -- 28159/28162 Funktionen, 0
fehlgeschlagen, identischer Umfang wie die Vor-Fix-Generierung.
Bestaetigt in der frischen `sub_0026940F`, dass `_icall_esp = g_esp;`
jetzt direkt nach `PUSH32(esp, esi); PUSH32(esp, edi);` und vor dem
ersten Aufruf-spezifischen Argument-Push landet, fuer alle 10 indirekten
Call-Stellen. Vollstaendiger sauberer Rebuild erfolgreich (keine neuen
Warnungen ausser bereits bestehenden, unabhaengigen
Unreferenzierte-Label-Hinweisen in `recomp_manual.c`).

Das neu gebaute `scda_recomp.exe` ~90 Sekunden laufen lassen (179.000+
Kernel-Aufrufe). Ergebnisse:
- `0x00390037` erscheint nirgends in `scda_runtime.log` -- null
  Vorkommen, gegenueber der vorherigen Steady-State-Wiederholung bei
  jedem Frame.
- Die spezifische Null-Vtable-Call-Signatur, die diese gesamte
  Untersuchung verfolgt hat (`[ICALL] ... target=0x00000000 ...
  source=0x0005F701`), ist verschwunden; die einzigen verbleibenden
  `source=0x0005F701`-Treffer (10 ueber den gesamten Lauf) zeigen jetzt
  echte, nicht-null Zieladressen und legitime Heap-Bereich-`ecx`/`edi`-
  Werte -- ein anderer, deutlich niedrigerprioritaerer Fall von "nicht
  aufgeloestes indirektes Call-Ziel" (derselbe generelle
  RECOMP_ICALL_SAFE-Fallback, korrekt ausgeloest an einem anderen, noch
  nicht identifizierten vtable-Slot; diese Runde nicht weiter verfolgt,
  da keine Evidenz vorliegt, dass er ein erhaltenes Register verwirft
  wie im vorliegenden Fall) statt dem korrupten Muell-Pointer-Muster,
  das diese Untersuchung zum Ziel hatte.
- Der Lauf erreicht einen **neuen, anderen Steady-State** (ein
  wiederholter Kernel-Aufruf, der durch eine kleine Menge von
  Ordinalen/Slots zyklisiert) -- ein eigenstaendiger, noch nicht
  untersuchter Blocker, als naechster Punkt vorgemerkt.

### Nicht in diesem Eintrag

Der neue Post-Fix-Steady-State (Kernel-Aufruf-Zyklus ueber Ordinale
99/234/246/277/294) ist unerforscht -- als naechster konkreter Blocker
vorgemerkt gemaess der stehenden Anweisung, nach Dokumentation eines
relevanten Ergebnisses autonom weiterzuarbeiten. Die 10 verbleibenden,
harmlos aussehenden unresolved-icall-Treffer bei `source=0x0005F701`
(anderes Ziel, nicht null) sind vermerkt, aber nicht weiterverfolgt --
keine Evidenz, dass sie ein erhaltenes Register verwerfen wie im
`0x0026940F`-Fall, und die stehende Anweisung fuer diese Bisektion war
speziell die `0x00390037`-Korruption, die nun geschlossen ist.

Kein Stub, kein Fake-Signal, kein Clamp, und kein Hand-Edit an einer
generierten `.c`-Datei ueberlebt -- der Fix lebt vollstaendig im
Codegenerator (`tools/recomp/translator.py`) plus dessen Testsuite,
verifiziert durch eine vollstaendige, echte Pipeline-Regenerierung, kein
zurechtgeflicktes Symptom.
"""

new_raw = raw.rstrip(b"\r\n") + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
