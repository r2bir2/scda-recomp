# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 217 (2026-09-21): Den tatsaechlichen Aufrufer/die Schleife hinter sub_00040821 gefunden -- kein Terminator-Tag irgendwo, bestaetigt Erklaerung 2, findet aber auch, dass der Ringpuffer stack-alloziert ist, und liefert eine konkrete, testbare vereinheitlichte Hypothese aus Erklaerung 2 und 3

**Verfolgte Aufgabe:** Update 216s eigenen naechsten Schritt fortsetzen
(`sub_0007DE25`, `sub_0007F412`, `sub_00064B00` auf Terminator-/Tag-Logik
lesen). `sub_0007DE25` vollstaendig sofort gelesen; vor Investition in die
anderen zwei (714 bzw. 656 Zeilen) auf eine direktere Methode umgeschwenkt --
den gesamten generierten Output nach jeder Aufrufstelle von `sub_00040821`
durchsuchen und deren TATSAECHLICHEN Aufrufer lesen, da dieser garantiert
enthaelt, was die Leseschleife steuert, statt weiter unter Update 177s drei
Kandidaten zu raten.

**`sub_0007DE25` ausgeschlossen:** vollstaendiges Lesen (98 Zeilen) zeigt
eine kleine, generische vtable-Dispatch-Objekt-Abbauroutine -- gibt drei
Unterobjekte per indirektem Aufruf ueber `MEM32(eax+0x20)` frei, ruft
bedingt `sub_00036582`, beruehrt refcount-foermige Felder bei
`+0x14/+0x18/+0x1C`. Keine Terminator-Logik in irgendeiner Form.

**Der echte Aufrufer gefunden: `sub_00042F00`/`sub_000430A5`.** Beide
Aufrufstellen von `sub_00040821` (`recomp_0001.c:80465` und `:80760`)
kehren zum selben Label `0x431C9` zurueck, und beide liegen in EINER
logischen Funktion, aufgeteilt auf zwei `void sub_...(void)`-Bloecke auf
C-Ebene (`sub_000430A5` ist als `frameless: inherit caller's frame`
markiert -- ein Fallthrough-/Sprungziel von `sub_00042F00`, kein
eigenstaendiger Aufruf). Diese Funktion ist der Resource-Loader, der die
Datei oeffnet, das Ringpuffer-Objekt aufsetzt und eine Schleife faehrt, die
`sub_00040821` (Refill) und danach `sub_000517A4` (Parsen/Konsumieren)
wiederholt aufruft.

**Neuer, unerwarteter Fund: das Ringpuffer-Objekt ist gar keine
Heap-/Arena-Allokation -- es ist direkt auf dem Stack-Frame dieser
Loader-Funktion per `alloca` reserviert.** Ganz am Anfang von `sub_00042F00`:
`eax = 0x40B0; call sub_00365C30;` -- `0x40B0` = 16560 dezimal, und jede
spaetere Referenz auf das Ringpuffer-Objekt innerhalb dieser Funktion nutzt
`ecx = ebp + -16560` als `this`-Zeiger. `sub_00365C30` wird mit der
Reservierungsgroesse in `eax` VOR jedem eigenen Feld-Schreibvorgang des
Objekts aufgerufen -- entspricht exakt der Standard-MSVC-
`_alloca_probe`/`_chkstk`-Aufrufkonvention. Das bedeutet: die urspruengliche
Frage dieser Runde ("gehoert der Bereich vor dem Objekt zu einer groesseren
Parent-/Arena-Allokation") hat fuer dieses konkrete Objekt eine klare
Antwort: es gibt hier ueberhaupt keine Heap-Arena -- "vor dem Objekt" (in
Underflow-Richtung) bedeutet niedrigere Adressen INNERHALB desselben
Stack-Frames dieser Loader-Funktion, also ihre eigenen anderen lokalen
Variablen und, jenseits der alloca-Basis, ihre gesicherten
`ebx/esi/edi`-Register und schliesslich der Frame ihres eigenen Aufrufers.

**Die tatsaechliche Lese-/Parse-Schleife, vollstaendig gelesen
(`recomp_0001.c:80746-80793`):**

```
loc_000431AE:
    ebx = min(MEM32(ebp-8), edi)      // edi = feste 0x1000-Byte-Chunkgroesse
    MEM32(ebp-8) -= ebx
    call sub_00040821(this=ebp-16560, ..., ebx, MEM32(ebp+8))   // Refill
loc_000431C9:
    call sub_000517A4(...)             // Konsumieren/Parsen
loc_000431DA:
    if (MEM32(ebp-8) != 0) goto loc_000431AE;   // Schleife solange != 0
```

Das ist eine simple, bedingungslose bytegezaehlte Schleife. Es gibt
NIRGENDS einen Tag- oder Terminator-Vergleich -- sie laeuft, bis
`MEM32(ebp-8)` null erreicht, Punkt. Das bestaetigt Erklaerung 2 aus Update
216 in konkreter Form, eine Ebene ueber `sub_00040821` selbst, und macht es
sehr unwahrscheinlich, dass `sub_0007F412`/`sub_00064B00` (noch nicht
vollstaendig gelesen, aber anhand von Teil-Lesevorgaengen bereits als
grosser Konstruktor bzw. Datei-Oeffnen-/Pfad-Formatierungsroutine erkannt)
ueberhaupt eine Terminator-Pruefung enthalten -- sie liegen noch weiter von
dieser Schleife entfernt, nicht darin.

**Woher `MEM32(ebp-8)` (die Gesamtsumme der Schleife) stammt -- vollstaendig
zurueckverfolgt:** unmittelbar vor der Schleife macht `sub_000430A5`, in
dieser Reihenfolge: `SetFilePointer(handle, -4, NULL, FILE_END)` (4 Bytes
vor das tatsaechliche physische Dateiende springen) -> `sub_000516B7(handle,
&MEM32(ebp-8), length=4, &bytesRead)` (ein echter Disk-Read von genau 4
Bytes in `MEM32(ebp-8)`, geprueft `bytesRead==4`) -> `SetFilePointer(handle,
0, NULL, FILE_BEGIN)` (zurueck an den Anfang springen). **`MEM32(ebp-8)` ist
also der eigene, eingebettete 4-Byte-Trailer-Wert der Datei, direkt aus den
letzten 4 Bytes von `common.lin` gelesen, und wird bedingungslos als
Gesamt-Byte-Budget fuer die komplette Schleife verwendet.**

**Ein zweiter, unabhaengiger Groessenwert existiert daneben.** Direkt nach
dem Zurueckspringen ruft dieselbe Funktion zusaetzlich `sub_00051B8B` ->
`sub_00051B4D` auf, was ein echter `NtQueryInformationFile`-Wrapper ist: die
Argumente passen exakt (`FileHandle`, `IoStatusBlock=ebp-8`,
`FileInformation=ebp-64`, `Length=0x38`,
`FileInformationClass=0x22`=`FileNetworkOpenInformation`), und die zwei
herauskopierten Dwords (`MEM32(ebp-64+40)`/`+44`) landen exakt am
`EndOfFile`-Feld-Offset dieser Struktur (0x28). Das ist eine **echte, live
Abfrage der tatsaechlichen physischen Dateigroesse**, kein abgeleiteter oder
zwischengespeicherter Wert. `(PhysicalSize - 4)` wird dann an
`sub_00040766(this=Ringpuffer-Objekt, 0, 0, PhysicalSize-4)` uebergeben --
sehr wahrscheinlich, um dem Ringpuffer-Objekt mitzuteilen, wie viele Bytes
ihm tatsaechlich auf der Platte zur Verfuegung stehen, unabhaengig davon,
was der Trailer-abgeleitete Zaehler der aeusseren Schleife noch anfordern
will.

**`kernel_file.c`s eigene `NtQueryInformationFile`-Implementierung auf einen
statischen Bug geprueft -- keinen gefunden.** Sowohl der Windows-Pfad
(`GetFileInformationByHandle`, `xbox_NtQueryInformationFile` um Zeile
575/587) als auch der POSIX-Pfad (`stat()`, um Zeile 1164/1170) fuehren eine
echte, live Abfrage gegen das tatsaechlich geoeffnete Host-Datei-Handle fuer
`FileNetworkOpenInformation`s `EndOfFile` durch, ohne festkodierten oder
abgeleiteten Wert. Das schliesst einen trivialen "falsche Konstante"-Bug in
unserer eigenen Kernel-Bruecke fuer diesen konkreten Aufruf aus.

**Die physische Bytezahl jeder `common.lin`-Kopie auf der Platte geprueft --
alle stimmen exakt mit Retail ueberein.** Alle drei Build-Output-Baeume
(`build/scda/bin/{Release,RelWithDebInfo}`,
`build/scda_upgrade/bin/RelWithDebInfo`) melden `Default/DataXb/LMaps/common.lin`
mit exakt 1.924.977 Bytes, identisch zur Retail-ISO-Extraktion
(`SC4_debug_compare/unpacked iso/Retail/...`), bytegenau uebereinstimmend
mit dem bereits in Update 173 festgestellten vollstaendigen Inhaltsabgleich.
(Nebenbefund: eine voellig unabhaengige, viel groessere
`adv/System/common.lin`, 7.758.556 Bytes, existiert ebenfalls im
Disc-Baum -- andere Datei, gleicher Name/Endung, hier nicht relevant. Eine
separate "Proto"-Extraktion unter `SC4_debug_compare` hat eine
UNTERSCHIEDLICH GROSSE `LMaps/common.lin`, 1.922.128 Bytes -- fast sicher
ein nicht verwendetes Prototyp-/Beta-Build-Artefakt, von keinem unserer
Build-Outputs genutzt, nur notiert, damit es spaeter nicht mit einer echten
Diskrepanz verwechselt wird.) Das schliesst die naive Erklaerung "recomp
liest eine falsch grosse Datei" aus -- unsere Datei ist auf jedem
geprueften Build-Output die korrekte, Retail-uebereinstimmende.

**Verfeinerte, vereinheitlichte Hypothese (noch nicht empirisch
bestaetigt):** da der eigene Trailer-Wert der Datei (`MEM32(ebp-8)`s
Anfangswert) direkt aus Dateibytes gelesen wird, die bereits als bytegenau
identisch zu Retail bewiesen sind, muss er auf recomp und auf echter
Hardware numerisch identisch sein -- beide Plattformen starten diese
Schleife mit demselben Gesamtbudget. Update 185/216 zeigte bereits, dass
xemu denselben Kurzlesevorgang (`want=0x20000 got=0x15F71`) erlebt wie
recomp. Der in Update 216 festgestellte konkrete Unterschied ist nur, dass
xemu DANACH keine weiteren `sub_000516B7`-Aufrufe mehr macht, waehrend
recomp das tut. Kombiniert mit dem Fund dieser Runde: **wenn der aeussere
Schleifenzaehler echter Hardware (`MEM32(ebp-8)`) in genau der Iteration
null erreicht, in der dieser Kurzlesevorgang auftritt, verlaesst die
Schleife sich selbst sauber, BEVOR sie `sub_00040821` je um ein weiteres
Refill bittet -- und der bereits bestaetigte Cursor-Formel-Defekt (Update
215, Erklaerung 3) wird nie ausgeloest, weil ihn niemand erneut aufruft.**
Wenn recomps eigene Zaehlung von "effektiv verbrauchten Bytes pro Iteration"
auch nur geringfuegig abweicht (plausibel, da Update 216 bereits
feststellte, dass recomp und xemu den aequivalenten Zyklus auf
unterschiedlichen internen Aufrufstellen von `sub_00040821` landen lassen),
waere recomps Zaehler nach demselben Kurzlesevorgang immer noch ungleich
null, wodurch die Schleife `sub_00040821` ein weiteres Mal aufruft -- genau
der Aufruf, der in den bereits korrumpierten/veralteten Ringpuffer-Zustand
laeuft und die Kompaktierungs-memcpy ausloest. Das wuerde bedeuten, dass
Erklaerung 2 und 3 keine konkurrierenden Alternativen sind, sondern zwei
Haelften eines Mechanismus: es gibt keine Terminator-Pruefung (2), und der
seit der Original-XBE latente, bytegenau identische Cursor-Defekt (3) ist
harmlos, solange die aeussere trailer-gesteuerte Schleife nach einem
Kurzlesevorgang nie um ein Refill bittet -- eine Frage exakter
Byte-Buchhaltungs-Paritaet zwischen den Schleifeniterationen beider
Plattformen, nicht unterschiedlichen Codes auf beiden Plattformen.

**Noch nicht erledigt -- konkreter naechster Test:** auf einem Live-recomp-Lauf
den Wert von `MEM32(ebp_aussen - 8)` erfassen (der aeussere
Restbytezaehler, erreichbar vom bereits existierenden Korruptions-memcpy-
Erfassungspunkt aus als `MEM32(ebx_objekt_this + 0x40B0 - 8)`, da
`ebp_aussen = this + 0x40B0`) im exakten Moment, in dem das
Ringpuffer-Objekt korrumpiert wird, um zu sehen, ob er bereits null ist (die
Schleife haette stoppen sollen -- deutet auf einen anderen Bug) oder noch
ungleich null (bestaetigt obige vereinheitlichte Hypothese). Das erfordert
eine neue, kleine One-Shot-Erfassung (noch nicht hinzugefuegt), statt etwas
bereits Zurueckgesetztes wiederzuverwenden.

**Auch diese Runde kein Fix implementiert** -- gemaess der Standing
Instruction, weiterhin am Klaeren. **Modellstufe:** STRONG bleibt
angemessen; der naechste Schritt ist ein einzelner, klar spezifizierter
Speicher-Read an einer bekannten Stelle.
"""

new_raw = raw.rstrip(b"\r\n") + b"\r\n" + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
