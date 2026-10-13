# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 218 (2026-09-21): Live-Erfassung WIDERLEGT Update 217s eigene vereinheitlichte Hypothese -- der Zaehler der aeusseren Schleife ist bereits null im Moment der Korruption, also die normale, erwartete letzte Iteration, keine zusaetzliche; ausserdem echte Lauf-zu-Lauf-Nichtdeterminiertheit entdeckt, welcher Stream zuerst das bestehende sub_0003FCA5-Einfrieren ausloest

**Verfolgte Aufgabe:** Update 217 stellte eine konkrete, falsifizierbare
Hypothese auf (die aeussere Schleife in `sub_000430A5` ruft
`sub_00040821` ein zusaetzliches Mal unnoetig nach einem Kurzlesevorgang
auf, weil ihr eigener Bytezaehler noch nicht null erreicht hat) und nannte
den naechsten Test genau: `MEM32(objekt_this + 0x40B0 - 8)` (den eigenen
Restbytezaehler der aeusseren Schleife, erreichbar von innerhalb
`sub_00040821` aus, da `ebp_aussen = this + 0x40B0`) im Moment der bekannten
korrumpierenden Kompaktierung erfassen. Als neue, minimale One-Shot-
Erfassung implementiert (`SCDA-DIAG-217`-Marker, an
`vtable==0x4F6940 && remaining_space>0x4000` gebunden, hoechstens einmal
feuernd) direkt in `recomp_0001.c` bei `loc_00040942` (beide Textstellen,
`sub_00040821` und dessen Duplikat `sub_0004086E`), neu gebaut, ausgefuehrt.

**Ergebnis -- die Hypothese ist WIDERLEGT:**

```
SCDA-DIAG217 this=01135928 vtable=004F6940 bufsize=00024000 cursor=00004001
             remaining_space=0001FFFF outer_ebp=011399D8 outer_remaining=00000000
```

`outer_remaining=00000000`. Der eigene Zaehler der aeusseren Schleife ist
BEREITS null im exakten Moment, in dem die gefaehrliche Kompaktierung
feuert. Gegeben der in Update 217 gefundenen Schleifenstruktur (`ebx =
min(remaining, 0x1000); remaining -= ebx; call sub_00040821(...); call
sub_000517A4(...); if (remaining != 0) goto loop;`), bedeutet ein Zaehler
von exakt null BEIM EINTRITT in den Aufruf von `sub_00040821`, dass dies
KEINE zusaetzliche, fehlerhafte Iteration ist -- es ist die normale, letzte
Iteration der Schleife, die die letzte (moeglicherweise partielle)
`<=0x1000`-Byte-Einheit verbraucht, bevor die eigene `!= 0`-Pruefung sie
beenden wuerde. Die Schleife kann diesen Aufruf nicht ueberspringen; echte
Hardware muss denselben letzten Aufruf machen, da der Code bytegenau
identisch ist. Das schliesst "die aeussere Schleife fragt einmal zu viel"
als eigenstaendige Erklaerung aus.

**Verfeinertes mechanistisches Verstaendnis.** `cursor=00004001` ist selbst
hochgradig verdaechtig: es ist exakt das, was die bereits bekannte
fehlerhafte Formel aus Update 215 (`MEM32(ebx+0x4060) = 0x4000 -
MEM32(ebp-12)`) liefern wuerde, wenn der Eingabewert "veralteter,
vorab-gelesener Restplatz" `0xFFFFFFFF` (-1) waere, da `0x4000 - (-1) =
0x4001` unter 32-Bit-Ueberlauf-Arithmetik. Das bedeutet, der Muellwert des
Cursors wurde hoechstwahrscheinlich von der VORHERIGEN Invocation durch
genau den bereits in Update 215 gefundenen und gegen die Original-XBE
bytegenau verifizierten Defekt geschrieben -- und dieser Muell-Cursor ist
es, der dann `remaining_space = bufsize - cursor = 0x24000 - 0x4001 =
0x1FFFF` zu einer riesigen, gefaehrlichen Zahl macht, beim naechsten Besuch
des Kompaktierungspfads. Unter dieser Lesart ist **der Cursor-Formel-Defekt
(Update 215, Erklaerung 3) allein ausreichend, um die Korruption auf
dieser, der ganz normalen letzten Schleifeniteration zu erklaeren -- weder
die Terminator-/Tag-Frage noch eine Diskrepanz in der Iterationsanzahl der
aeusseren Schleife (Update 217s eigene Hypothese) sind als zusaetzliche
Mechanismen noetig.** Die verbleibende offene Frage verengt sich sauber
auf: warum liefert die VORHERIGE Invocations Kurzlesevorgang auf recomp
einen Eingabewert von exakt -1 (oder etwas, das auf einen gefaehrlichen
Cursor abbildet), waehrend der analoge Moment auf echter Hardware das
offenbar nicht tut.

**Ein direkter Folge-Erfassungsversuch unternommen, konnte nicht
abgeschlossen werden -- stattdessen eine neue, wichtige Umgebungs-Tatsache
entdeckt.** Um die "Eingabewert = -1"-Arithmetik direkt zu verifizieren,
wurde eine zweite One-Shot-Erfassung (`SCDA-DIAG-218`) bei `loc_00040992`
(die Zeile, die den Cursor schreibt) hinzugefuegt, die jede Aktualisierung
fuer die konkrete Objektadresse `0x01135928` von oben protokolliert. Zwei
Dinge liefen nacheinander schief, und deren Entwirrung lieferte einen
nuetzlichen neuen Befund:
1. Zunaechst wurde vermutet, dass die neue Erfassung selbst ein Haengen
   verursacht (zwei lange, stille, 100%-CPU-auslastende Laeufe
   hintereinander mit dem neuen Code, nachdem zwei fruehere Laeufe mit nur
   der ersten Erfassung schnell zurueckgekehrt waren). Die zweite Erfassung
   zurueckgesetzt und erneut ausgefuehrt, um sie zu isolieren.
2. Dasselbe stille, CPU-auslastende Verhalten trat dann erneut auf, nur
   mit der bereits bewaehrten ersten Erfassung -- das schliesst die zweite
   Erfassung als Ursache aus. Ein Live-, nicht-invasiver Snapshot (`cdb -pv
   -p <pid> -c "r eip; kb 3; q"`, exakt die sichere Methode, die Update 210
   bereits etabliert hatte) an einem Lauf, der 64+ Minuten still einen Kern
   auslastete, zeigte: der Prozess war NICHT haengengeblieben oder langsam
   -- er hatte bereits dasselbe, bereits dokumentierte, aus Update 210
   bekannte Einfrieren erreicht (`jmp $`-Selbstschleife bei
   `sub_0003FCA5+0x17b0`, diesmal erreicht ueber einen ANDEREN Aufruf in
   `sub_00040821+0x11cd`, also eine andere gestreamte Ressource als das
   `common.lin`, das diese Runden-Instrumentierung beobachtet). Update 210
   selbst hatte genau dieses Symptom bereits einmal gesehen (36 Minuten
   Stille, bevor ein aequivalenter Snapshot denselben eingefrorenen Zustand
   fand) und es als normale Varianz charakterisiert, kein Haengen.

**Neuer Befund: welcher Stream zuerst das bestehende
`sub_0003FCA5`-Einfrieren ausloest, ist NICHT lauf-zu-lauf deterministisch**,
obwohl die Guest-Adressen und berechneten Werte INNERHALB der Verarbeitung
eines gegebenen Streams hochgradig reproduzierbar sind (dasselbe
`this=0x01135928` trat ueber mehrere, unabhaengig gestartete Laeufe dieser
Sitzung hinweg wiederholt auf). Das passt zur asynchronen Natur von
`sub_000516B7`s zugrundeliegendem Disk-I/O (`STATUS_PENDING`/
Callback-Abschluss, gemaess Update 213s eigener Lesung) -- die tatsaechliche
Reihenfolge des OS-Ebene-I/O-Abschlusses kann lauf-zu-lauf variieren, selbst
fuer scheinbar einzelthreadigen Guest-Code, sodass welcher unbeteiligte
Stream zuerst den fatalen `MEM32(0x5E6E0C)`-Callback-Pfad erreicht,
zwischen ansonsten identischen Starts variieren kann. Zwei von vier
Laeufen dieser Runde erreichten den `common.lin`-Diagnosepunkt produktiv;
die anderen zwei froren zufaellig zuerst auf einem anderen Stream ein und
erreichten ihn nie. Das ist eine nuetzliche, dauerhafte Umgebungs-Tatsache
fuer kuenftiges Debugging dieses gesamten Bereichs: ein einzelner
nicht-reagierender Lauf ist fuer sich genommen kein Beweis fuer eine
Regression, und ein Live-`cdb -pv`-Snapshot (nie ein Kill) ist die
korrekte, guenstige Methode, um "laeuft noch normal" von "bereits am
bekannten Einfrierpunkt" zu unterscheiden, bevor entschieden wird, ob
laenger gewartet oder neu gestartet wird.

**Housekeeping:** beide One-Shot-Erfassungen (`SCDA-DIAG-217`, die nie
abgeschlossene `SCDA-DIAG-218`, sowie das dafuer noetige
`#include <stdio.h>`) wurden erneut vollstaendig aus `recomp_0001.c`
entfernt, per Grep verifiziert (keine verbleibenden `SCDA-DIAG`-Referenzen)
und sauber neu gebaut.

**Kein Fix implementiert.** Die Root Cause ist jetzt eine Ebene praeziser
verstanden als Update 217 sie hinterliess: der Cursor-Formel-Defekt aus
Update 215 ist sehr wahrscheinlich allein ausreichend, um die Korruption
auf der eigenen letzten Schleifeniteration dieses Streams zu erklaeren,
ohne eine zusaetzliche Diskrepanz in der Iterationsanzahl der aeusseren
Schleife zu benoetigen. Offen bleibt, WARUM die vorherige Invocations
Kurzlesevorgang auf recomp konkret einen gefaehrlichen veralteten
Eingabewert erzeugt -- das wurde noch nicht direkt erfasst (der
`SCDA-DIAG-218`-Versuch dazu wurde durch die oben beschriebene
Einfrier-Reihenfolge-Nichtdeterminiertheit blockiert, nicht widerlegt).

**Naechster Test:** die `SCDA-DIAG-218`-artige Erfassung erneut versuchen
(vollstaendige Sequenz der Cursor-Formel-Aktualisierungen fuer das
konkrete Objekt, nicht nur die letzte), im Wissen, dass es wegen der neu
verstandenen Einfrier-Reihenfolge-Nichtdeterminiertheit 2-4 Versuche
brauchen kann; einen Live-`cdb -pv`-Snapshot statt eines festen
Wall-Clock-Timeouts nutzen, um "laeuft noch Richtung common.lin" von
"bereits auf einem anderen Stream eingefroren" zu unterscheiden, ohne einen
Lauf zu killen, der noch kurz vor dem Erfolg stehen koennte.

**Modellstufe:** STRONG bleibt angemessen -- das ist eine begrenzte, klar
spezifizierte Erfassungsaufgabe mit einer verstandenen, charakterisierten
Quelle fuer Lauf-Varianz, kein offenes Suchen.
"""

new_raw = raw.rstrip(b"\r\n") + b"\r\n" + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
