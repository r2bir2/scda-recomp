# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 220 (2026-09-21): Kette direkt mit exakten Zahlen bestaetigt -- KORRIGIERT die stale==-1-Vermutung zu stale==7 (ein legitimer Wert); der wahre Defekt liegt in einer festkodierten 0x4000-Annahme statt der bereits verfuegbaren echten Transferzahl; minimaler, registerlebensdauer-sicherer Fix implementiert und verifiziert, dass er das Laufzeitverhalten aendert (Prozess kommt jetzt ueber den alten Einfrierpunkt hinaus in neues Terrain)

**Verfolgte Aufgabe:** gemaess expliziter Anweisung die Kette
`stale_value -> Cursor-Formel -> remaining_space -> memcpy-Unterlauf` direkt
bestaetigen, mit minimaler One-Shot-Instrumentierung, einem Host-seitigen
Erfolgsmarker, frischen Prozessen pro Versuch, maximal ca. 3 Minuten
gebundenem Warten, einem einzelnen `cdb`-Snapshot statt langer stiller
Wartezeiten, und sofortigem Neustart falls ein Lauf bereits im bekannten,
unbeteiligten `sub_0003FCA5`-Einfrieren auf einem anderen Stream steckt.
Nach Bestaetigung entweder die Herkunft von `stale_value` zurueckverfolgen
oder den korrekten Fix bestimmen; kein Clamp auf Cursor/remaining_space, bis
das geklaert ist.

**Orchestrierung exakt wie spezifiziert gebaut und genutzt**
(`tools/debug/diag219_orchestrate.sh`, als wiederverwendbares Werkzeug
belassen): startet einen frischen `scda_recomp.exe`, prueft alle paar
Sekunden auf eine Marker-Datei, macht nach 180s ohne Marker genau einen
`cdb -pv -p <pid> -c "r eip; kb 3; q"`-Snapshot, killt und startet sofort
neu, falls dieser Snapshot das bekannte `sub_0003FCA5`-Einfrieren zeigt,
sonst eine weitere Zeitscheibe. Das vermeidet direkt die langen, blinden
Wartezeiten aus Update 218.

**Die ersten zwei Gate-Versuche trafen daneben (beide guenstig diagnostiziert,
keine Symptome eines echten Problems):**
1. Eine erste One-Shot-Erfassung, an dieselbe bereits an der KORRUMPIERENDEN
   Aufrufstelle bewiesene Bedingung gebunden (`remaining_space>0x4000`),
   aber an die Cursor-Formel-Stelle selbst gesetzt, feuerte nach 34s -- aber
   beim ERSTEN, voellig gewoehnlichen Zyklus fuer dieses Objekt, da
   `remaining_space>0x4000` sich als bei fast jedem normalen Zyklus dieses
   Puffers wahr herausstellt (`bufsize=0x24000`, Formel-Ausgabe immer in
   `[0,0x4000]`), kein Zeichen von irgendetwas Falschem.
2. Das Gate auf `stale>0x4000` verschaerft (in Erwartung eines
   umgeschlagenen/negativ aussehenden Sentinels) feuerte ueber 8 frische
   Versuche hintereinander nie -- alle 8 landeten unabhaengig voneinander im
   bereits dokumentierten `sub_0003FCA5`-Einfrieren auf einem ANDEREN
   Stream, bevor sie diesen je erreichten. Diagnostiziert ueber die eigenen
   `cdb`-Snapshots der Orchestrierung (jeder zeigte
   `sub_0003FCA5+0x17b0`s `jmp $`, erreicht ueber eine ANDERE
   `sub_00040821`-Aufrufstelle als die von dieser Instrumentierung
   beobachtete) -- bestaetigt Update 218s eigenen Fund zur
   Einfrier-Reihenfolge-Nichtdeterminiertheit, kein Fehler in der neuen
   Instrumentierung.

**Finales, erfolgreiches Design: die bereits bewaehrte Ausloese-Bedingung
wiederverwenden, keine neue Vermutung.** Statt eine NEUE Bedingung an der
Cursor-Formel-Stelle zu raten, wurde ein kleiner, guenstiger,
bedingungsloser Ringpuffer (4 Eintraege) hinzugefuegt, der jede
Cursor-Formel-Aktualisierung fuer dieses Objekt aufzeichnet (kein I/O im
Hot Path), und dessen Inhalt nur ausgegeben, wenn `sub_00040821`s bereits
validierte gefaehrliche Kompaktierungs-Bedingung (exakt die, auf die sich
Updates 217/218 stuetzten) feuert. Das reproduzierte zuverlaessig in 36
Sekunden beim allerersten Versuch -- zweimal.

**Erste erfolgreiche Erfassung (Ring zeichnete nur eine der zwei
Textstellen der Formel auf) zeigte eine Luecke:** der juengste Eintrag des
Rings (`cursor_after=0x21DE`) passte nicht zur eigenen Lesung des
korrumpierenden Aufrufs (`cursor=0x4001`) -- ein fehlendes Bindeglied.
Ursache: Update 215 hatte bereits festgestellt, dass die Formel an ZWEI
separaten Textstellen existiert (`loc_000408F0`, nach Aufrufstelle 1, und
`loc_00040992`, nach Aufrufstelle 2); die erste Erfassung instrumentierte
nur die zweite. Denselben guenstigen Rekorder auch bei `loc_000408F0`
hinzugefuegt, erneut ausgefuehrt -- **luckenloses Ergebnis:**

```
SCDA-DIAG220 CORRUPTING-CALL this=01135928 bufsize=00024000 cursor=00004001 remaining_space=0001FFFF ring_count=4
SCDA-DIAG220 ring[3] this=01135928 stale=00000007 cursor_before=00023FF9 cursor_after=00003FF9
```

`cursor_after=0x3FF9` plus die zwei 4-Byte-Record-Header-Feldlesungen bei
`loc_00040909` (jede erhoeht den Cursor um 4, +8 insgesamt) landet exakt bei
`0x3FF9 + 8 = 0x4001` -- passt bitgenau zur eigenen Lesung des
korrumpierenden Aufrufs. **Die Kette ist jetzt vollstaendig, numerisch,
luckenlos bestaetigt:** `stale=7 -> cursor=0x4000-7=0x3FF9 -> +8
(Header-Parse) -> cursor=0x4001 -> remaining_space=bufsize-cursor=0x1FFFF
-> unterlaufende memcpy-Zieladresse.`

**Das korrigiert die bisherige `stale==-1`-Hypothese aus Update 217/218.**
`stale=7` ist kein umgeschlagener/Muell-Sentinel -- es ist ein kleiner,
voellig plausibler "verbleibende Bytes bis zum physischen Ringpuffer-Ende"-
Wert. Der eigentliche Defekt liegt nicht in `stale` selbst, sondern darin,
WIE die Formel ihn nutzt: `neuer_cursor = 0x4000 - stale` ist nur korrekt,
wenn der gerade abgeschlossene Lesevorgang EXAKT 0x4000 Bytes uebertragen
hat (die implizite, festkodierte Annahme der Formel -- passt zu Update
215s eigener Beschreibung, "nutzt nie die tatsaechliche Transferzahl").
Entscheidend: **die echte transferierte Bytezahl ist bereits zwei Zeilen
frueher im selben Block in `eax` geladen** (`eax = MEM32(ebp+-4);
MEM32(ebx+0x4064) += eax;`, genutzt um einen Akkumulator zu aktualisieren)
-- die naechsten Zeilen verwerfen sie unnoetig (`eax = 0x4000;` bei
`loc_00040992`, oder `edx = 0x4000; eax = edx;` bei `loc_000408F0`), bevor
der neue Cursor aus der falschen, festen Konstante statt daraus berechnet
wird.

**Fix implementiert (kein Clamp -- eine Korrektur mit bereits vorhandenen
Daten):** genau die eine Instruktion an jeder Stelle entfernt, die die
bereits korrekte Transferzahl verwirft, sodass `eax` (bereits
`MEM32(ebp+-4)` haltend) direkt in `eax = eax - MEM32(ebp+-12)` einfliesst.
Eine Feinheit erforderte Sorgfalt: bei `loc_000408F0` wird `edx = 0x4000`
AUCH noch, unveraendert, weiter unten von der Kompaktierungs-eigenen
Zielformel benoetigt (`edi = edi + edx` innerhalb `loc_00040942`) -- diese
Zuweisung wurde bewusst unangetastet gelassen; nur die nachfolgende
`eax = edx`-Kopie (die keinen anderen Zweck hatte als die Transferzahl zu
verwerfen) wurde entfernt. Bei `loc_00040992` gibt es keine solche zweite
Nutzung, daher wurde die entsprechende `eax = 0x4000`-Zeile schlicht
komplett entfernt. Identisch auf beide Textvorkommen jeder Stelle angewandt
(die `sub_00040821`/`sub_0004086E`-Duplikation, bereits diese Sitzung
etabliert).

**Verifiziert, dass der Fix das Laufzeitverhalten aendert, nicht nur die
Formel isoliert.** Sauber neu gebaut, frische Instanz mit derselben
gebundenen-Warten-Disziplin ausgefuehrt: nach ca. 4 Minuten wurde der
Prozess (per `cdb`-Snapshot, kein Kill) bei
`sub_004A12B0`/`sub_004A1600`/`sub_004A1740` ausfuehrend gefunden -- drei
Adressen, die in der gesamten Untersuchung dieser Sitzung nie zuvor
aufgetaucht sind -- mit CPU-Auslastung um 16% (eine echte, guenstige
Poll-/Wartschleife, nicht das ~100%-auslastende harte `jmp $`-Spinnen des
alten Einfrierens) und einem stabilen, unveraenderten Call-Stack ueber eine
weitere 2-Minuten-Nachpruefung. **Der Prozess erreicht den alten
Korruptions-/Einfrierpunkt ueberhaupt nicht mehr** -- er ist tatsaechlich
weiter fortgeschritten als jeder Lauf in dieser gesamten Untersuchungsreihe,
und ist jetzt an einem neuen, spaeteren, zuvor unerreichbaren
Synchronisationspunkt blockiert. Das ist der naechste Blocker aus der
Standing-Instruction-Sequenz ("...weiter bis zum naechsten Blocker"), noch
nicht untersucht.

**Kritischer, noch ungeloester Vorbehalt: dieser Fix hat aktuell kein
dauerhaftes Zuhause.** `recomp_0001.c` ist gitignorte, generierte
Ausgabe; ein vollstaendiger Pipeline-Regenerate aus der Original-XBE (der
"vollstaendige Recomp-Pipeline"-Schritt der Standing-Instruction-Sequenz)
wuerde stillschweigend die originalen, bytegenau-mit-der-XBE-identischen
fehlerhaften Instruktionen reproduzieren und diesen Fix ohne Warnung
loeschen, da der Lifter ueber keinen Pro-Funktion-Override-/Patch-
Mechanismus in dieser Codebasis verfuegt (geprueft: `tools/recomp/config.py`,
`__main__.py`, sowie nach einer bestehenden adressbasierten
Override-Liste -- keine gefunden, weder fuer diese noch fuer irgendeine
andere Adresse). **Keinen vollstaendigen Pipeline-Regenerate ausfuehren, bis
dieser Fix ein dauerhaftes Zuhause hat** (eine Lifter-Ebene-Patch-Liste nach
Adresse, ein Nachgenerierungs-Patch-Anwendungsschritt, oder diese eine
Funktion in das handgepflegte `recomp_manual.c` als vollstaendigen Ersatz
verschieben) -- das ist jetzt die Top-Prioritaet fuer die Nachbereitung,
vor der Untersuchung des neuen `sub_004A12B0`-Blockers.

**Housekeeping:** alle `SCDA-DIAG-219`/`SCDA-DIAG-220`-Diagnosecode wurde
wieder vollstaendig aus `recomp_0001.c` entfernt (per Grep verifiziert), nur
der Fix selbst bleibt (markiert `SCDA-FIX-215`, vier Vorkommen: zwei Stellen
x zwei duplizierte Funktionen).

**Modellstufe:** STRONG bleibt fuer den unmittelbar naechsten Schritt
angemessen (einen dauerhaften Patch-Mechanismus zu finden ist eine
begrenzte Infrastrukturaufgabe); der `sub_004A12B0`-Blocker selbst ist noch
nicht bewertet und braucht ggf. eine eigene Stufen-Einschaetzung, sobald er
untersucht wird.
"""

new_raw = raw.rstrip(b"\r\n") + b"\r\n" + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
