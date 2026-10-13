path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

text = """

## Update 216 (2026-09-21): Live-xemu-Grundwahrheit fuer genau diesen Fall erhalten; Rollen der zwei sub_00040821-Aufrufstellen vollstaendig entwirrt; die wahre Root Cause ist eine DRITTE, weiterhin ungeklaerte Frage, die schon vier Tage frueher in Update 177 aufgeworfen wurde -- kein Fix implementiert, bewusst

**Befolgter Auftrag:** den in dieser Runde angeforderten xemu-Abgleich
(Datensatzlaenge, Restplatz, Cursor, Datei-Offset, resultierende memcpy-
Zieladresse) tatsaechlich durchfuehren, damit entscheiden, ob der "Arena-
Allocation"-Zweig der Anweisung zutrifft, und ERST DANACH eine Fix-
Strategie waehlen -- ausdruecklich noch keine implementieren.

**In dieser Runde durchgefuehrter Live-xemu-Lauf (nicht aus Update 185s
altem Log wiederverwendet -- ein frischer Boot, heute):** `xemu.exe -s -S`
gestartet und eine erweiterte Version von Update 185s eigenem
`xemu_object_refill_wantgot.py` laufen lassen
(`tools/debug/xemu_object_refill_wantgot_remaining.py`, neu, liest genau
eine Sache zusaetzlich: `ebx`s eigene `+0x4058`/`+0x4060`-Felder am
`sub_000516B7`-Eintritts-Breakpoint, ergibt `restplatz_bis_pufferende` fuer
genau diesen Aufruf). **Ergebnis: Treffer #16 bei t=28.297s reproduzierte
Update 185s exakten Kurzlesevorgang bytegenau** (`this=0x00879ED0`,
`want=0x00020000 got=0x00015F71`, Ruecksprungadresse `0x00040983` --
also Aufrufstelle 2), mit `restplatz_bis_pufferende=0x00001E22` in genau
diesem Moment. Das bestaetigt unabhaengig und frisch, dass echte Hardware
diesen exakten Kurzlesevorgang mit diesen exakten Zahlen tatsaechlich
erreicht, und beantwortet die bedingte Frage dieser Runde direkt: da xemus
eigene Zieladressen-/Restplatz-Werte an dieser Stelle wiederherstellbar
sind und klar von recomps abweichen (siehe unten), trifft der Zweig "gehoert
der Bereich vor dem Objekt zu einer groesseren Parent-/Arena-Allocation"
nicht zu -- beide Plattformen erreichen ueberhaupt nicht denselben
gefaehrlichen Zustand, es gibt also nichts an der Speicher-Layout-Deutung
neu zu interpretieren.

**Korrektur der eigenen Update-215-Aussage dieser Sitzung: welche
Aufrufstelle macht was.** Ein vollstaendiges Neu-Lesen von `sub_00040821`s
komplettem Koerper in einem Durchgang (nicht Fragmente) plus eine
passende recomp-seitige Erfassung (neue Callsite 18, `recomp_0001.c`,
zeichnet `puffergroesse`/`cursor`/`restplatz_bis_pufferende` unmittelbar
vor Aufrufstelle 2 auf, genau spiegelnd, was das xemu-Skript misst) zeigt
die tatsaechliche Sequenz des fatalen Zyklus, der Reihe nach:
1. Aufrufstelle 1 (`0x408E1`, der "<8 Bytes vor dem physischen Pufferende"-
   Refill) feuert und kehrt mit dem Ringpuffer-Unterobjekt **weiterhin
   vollstaendig valide** zurueck (bestaetigt durch Callsites 11/13 im Ring,
   unveraendert gegenueber Updates 213/214).
2. Der naechste Datensatz-Header-Parse (`loc_00040909`) liest die zwei
   4-Byte-Felder am neuen Cursor -- **an dieser Stelle noch valide Daten**
   (Callsite 14, valide).
3. Dieser Parse stellt fest, dass NICHT genug Restplatz fuer die
   deklarierte Datensatzlaenge vorhanden ist und faellt in den
   Kompaktierungsblock (`loc_00040942`, Callsite 16, **weiterhin valide
   unmittelbar vor dem `rep movsd`/`rep movsb`-Paar**).
4. **Die Kompaktierung's eigenes memcpy ist es, was das Objekt korrumpiert**
   -- Callsite 17, unmittelbar nach Abschluss beider Kopien erfasst, zeigt
   bereits alle Header-Felder genullt. Das bestaetigt Update 213s
   urspruenglichen Fund erneut, jetzt verankert in einer vollstaendig
   verstandenen umgebenden Sequenz.
5. Erst danach feuert Aufrufstelle 2 (`0x40983`) -- mit dem bereits
   genullten Objekt (Callsite 18 unmittelbar davor liest bereits alles
   Null; Callsites 11/12/9/10 fuer genau diese Invocation sind ebenfalls
   alle Null). Dieser Aufruf ist eine FOLGE der Korruption, kein
   unabhaengiger zweiter Kurzlesevorgang -- es gibt keinen Grund
   anzunehmen, dass genau diese Instanz von Aufrufstelle 2 ueberhaupt
   echte Disc-Daten mit einem bedeutungsvollen want/got liest, da ihr
   eigenes `handle`-Feld bereits zerstoert ist.

Das bedeutet: der tatsaechliche common.lin-erschoepfende Kurzlesevorgang
passiert auf **recomp** hoechstwahrscheinlich ueber **Aufrufstelle 1**,
waehrend auf **xemu** der bytegenau identische Kurzlesevorgang ueber
**Aufrufstelle 2** passiert -- nicht zwingend eine tiefe Inkonsistenz,
nur ein Unterschied darin, auf welche genaue Schleifeniteration jede
Plattform ihren (ansonsten identischen) Buchfuehrungsstand fuer den
letzten Teil-Chunk legt. Beide Aufrufstellen teilen denselben strukturellen
Defekt (Update 215), das aendert also nicht, WELCHE Formel fehlerhaft ist,
nur welche konkrete Aufrufinstanz ihn in einem gegebenen Lauf ausloest.

**Die wahre Root Cause ist eine dritte, tiefere Frage -- bereits
aufgeworfen, weiterhin ungeklaert, vier Tage bevor diese gesamte
Untersuchung begann:** weiteres Zurueckblaettern foerderte Updates 176-177
(17.09.2026, derselbe Tag wie Updates 171-175, chronologisch unmittelbar
DAVOR) zutage, die das Datensatzformat bereits vollstaendig entschluesselt
hatten: ein 8-Byte-Header (4-Byte Typ/Tag, 4-Byte Payload-Groesse), gefolgt
von entsprechend vielen Payload-Bytes, wobei das Tag-Feld an
`sub_000503EE` weitergereicht wird, vermutet als Deserializer. Update 177
verfolgte das eine Ebene weiter und fand: `sub_000503EE` ruft tatsaechlich
`sub_0004E275` auf, eine **generische Speicher-Wachstumsroutine**
(Allocator-Callback-Muster, `std::vector`-Grow-artig), die **voellig
unabhaengig vom tatsaechlichen Tag-Wert** ist -- ueberhaupt keine
Terminator-Pruefung. Update 177s eigene Schlussfolgerung, seitdem in
keinem der beiden Untersuchungsstraenge weiterverfolgt: *"Die
tatsaechliche Terminator-/Zaehl-Logik liegt folglich hoeher in der Kette,
vermutlich in ... `sub_0007DE25`, `sub_0007F412` oder `sub_00064B00`"* --
d.h., etwas weiter oben in der Aufrufkette soll ein Terminator-/
Stream-Ende-Tag erkennen und aufhoeren, weitere Chunks anzufordern, und
niemand hat je bestaetigt, ob es das tut, oder warum es das beobachtete
Verhalten nicht verhindert. Spaetere Updates (195/205) haben
`sub_0007DC43`/`sub_0007DE25`/`sub_001CA0A5` und einen Teil von
`sub_00064B00` bereits vollstaendig gelesen, aber **nur auf der Suche nach
einem Zero-Fill-Schreiber** (die "0x8070"-Rahmung) -- keiner dieser
Lesevorgaenge pruefte auf Tag-/Terminator-Behandlung, Update 177s konkrete
Frage bleibt also genauso offen wie am 17.09.2026.

**Warum in dieser Runde bewusst kein Fix implementiert wird:** drei intern
konsistente Erklaerungen fuer "warum zieht recomp weiter an einem
erschoepften Stream, waehrend echte Hardware das nicht tut" bleiben
lebendig, und sie verlangen drei unterschiedliche Fixes:
1. Die hoeherliegende Kette ERKENNT auf echter Hardware tatsaechlich
   korrekt ein Terminator-Tag, und etwas VOR `sub_00040821`
   (Zustand, nicht Code, da der Code bytegenau identisch ist) weicht auf
   recomp genug ab, dass der Terminator ihm nie praesentiert wird (z.B.
   landet echte Hardwares Kurzlesevorgang zufaellig so, dass sein eigener
   letzter echter Datensatz bereits DER Terminator IST, waehrend recomps
   woanders landet) -- behoben durch Finden und Korrigieren dieser
   vorgelagerten Zustandsabweichung, ohne `sub_00040821`/`sub_000516B7`
   ueberhaupt anzufassen.
2. Die hoeherliegende Kette implementiert GAR KEINE Terminator-Erkennung
   (Update 177 fand keinen Hinweis darauf in den vier geprueften
   Funktionen, die drei groessten Kandidaten bleiben aber unbestaetigt) --
   und auf echter Hardware wird `sub_00040821` einfach NIE gebeten, einen
   Chunk zu holen, den es nicht liefern kann, weil das eigene
   Groessen-Tracking des Spiels (passend zur tatsaechlichen Byte-Anzahl
   der echten Datei/ISO) das schlicht nie anfordert -- behoben durch
   Sicherstellen, dass die exponierte Groesse der emulierten Datei und die
   eigenen Groessenerwartungen des Spiels auf genau dieselbe Weise
   uebereinstimmen wie auf echter Hardware (ein Daten-/Umgebungs-Fix, kein
   Code-Fix).
3. Die fehlerhafte Formel in `sub_00040821` (Update 215) ist auf echter
   Hardware aus einem anderen, noch unbekannten Grund tatsaechlich nicht
   latent -- behoben durch Korrektur des Cursor-/Restplatz-Bookkeepings
   selbst.
Eine dieser drei zu implementieren, ohne zuvor die anderen zwei
auszuschliessen, riskiert entweder, ein Symptom zu fixen, waehrend der
echte Defekt bestehen bleibt (Wiederholung von `scda_disc_fill_overrun`s
eigenem Fehler, explizit in Update 185 benannt), oder eine Aenderung an
weitgehend geteiltem Hot-Path-Code (`sub_00040821`/`sub_000516B7` werden
von Dutzenden unabhaengiger Streams quer durchs ganze Spiel benutzt, laut
der Aufrufstellen-Grep in Update 212) einzufuehren, die etwas aktuell
Funktionierendes regressieren koennte, ohne automatisierte Testabdeckung
fuer dieses Modul, die das auffangen wuerde.

**Naechster Test (fuer STRONG, ersetzt Punkte 1-2 dieser Runde):**
1. `sub_0007DE25`, `sub_0007F412` und `sub_00064B00` vollstaendig von
   Grund auf lesen, spezifisch auf Terminator-/Tag-Pruef-Logik (nicht auf
   einen Zero-Fill-Schreiber, wofuer sie in Updates 195/205 ausgeschlossen
   wurden) -- das ist Update 177s eigener, vier Tage alter, nie
   abgeschlossener naechster Schritt.
2. Falls eine Terminator-Pruefung gefunden wird: auf recomp UND xemu den
   exakten Tag-Wert erfassen, der an der Stelle vorliegt, wo jede
   Plattforms eigener letzter Kurzlesevorgang landet, um zu sehen, ob
   recomps zufaellig vom Terminator-Sentinel abweicht, waehrend xemus das
   nicht tut.
3. Falls KEINE Terminator-Pruefung irgendwo in dieser Kette gefunden wird:
   zu Erklaerung 2 oben wechseln -- das eigene interne "erwartete
   Gesamtgroesse fuer diese Datei"-Feld des Spiels (wo auch immer es
   initialisiert wird, vermutlich aus einem frueher im Level-Load
   geparsten `.ini`-/Index-Eintrag) auf beiden Plattformen gegen
   `common.lin`s tatsaechliche extrahierte Groesse vergleichen.
4. Erst wenn eine dieser drei Erklaerungen bestaetigt ist: den
   entsprechenden Fix implementieren, mit einem vollstaendigen Lauf
   verifizieren, dann jeden `SCDA-DISC-ERR-INSTRUMENTATION`-Marker und die
   `scda_capture_wraparound_once`-/vergroesserte-Ringpuffer-Infrastruktur
   dieser Sitzung (Updates 210-216) zuruecksetzen, den Standard-
   inkrementellen Recomp-Build durchfuehren (eine vollstaendige Pipeline-
   Regenerierung ist nicht noetig -- das sind Hand-Edits an bereits
   generierten Dateien, ihr Zuruecksetzen ergibt exakt das, was eine
   Regenerierung ohnehin produzieren wuerde), und bis zum naechsten
   Blocker weiterarbeiten.

**Aktion dieser Runde:** gemaess obigem wird die temporaere
Instrumentierung jetzt zurueckgesetzt (ihr Zweck fuer den memcpy-/Cursor-
Mechanismus ist vollstaendig erfuellt; die naechste Phase braucht andere
instrumentierte Funktionen, nicht mehr vom Gleichen), statt 18
Aufrufstellen Diagnosecode unbegrenzt bestehen zu lassen. Siehe die
Housekeeping-Notiz im naechsten Commit.

**Modell-Stufe:** STRONG bleibt angemessen -- die verbleibende Arbeit ist
eine begrenzte, spezifische Funktions-Lese-Aufgabe (drei benannte
Funktionen) mit klarem Erfolgskriterium (Terminator-Pruef-Logik finden
oder ausschliessen), keine ergebnisoffene Suche.
"""

with open(path, "wb") as f:
    f.write(raw + text.replace("\n", "\r\n").encode("utf-8"))

print("done, new size", len(raw) + len(text.replace("\n", "\r\n").encode("utf-8")))
