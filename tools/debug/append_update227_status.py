# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 227 (2026-09-21): Die Ziel-Oberflaeche der korrumpierenden Kopie bis zu ihrem Konstruktor und einer Formatcode-Dispatch-Tabelle zurueckverfolgt; bestaetigt, dass dies eine generische Pixelformat-Konvertierungs-Kopie ist (kein "Praesentieren auf den Backbuffer"), was die offene Frage darauf eingrenzt, wo die physische/Tiled-Basisadresse der Ziel-Textur alloziert wird -- noch nicht gefunden; markiert fuer eine Scoping-Entscheidung, wie weit von Hand weiterverfolgt werden soll

**Methode:** zwei weitere Runden derselben temporaeren, One-Shot-,
vollstaendig zurueckgesetzten Quellcode-Instrumentierungstechnik
(`recomp_0027.c`, gitignored), plus eine Live-`cdb`-Stack-Trace-Erfassung,
die den Rueckwaerts-Trace von Update 226s geschaerfter Frage fortsetzt
("woher kommt `dst_desc + 0x18` = `0xF0464000`?").

### Die Deskriptor-Huelle wird aus einer bereits vollstaendigen "Oberflaechen-Info"-Struktur konstruiert

Dieses Projekts eigenen generierten Code nach Schreibzugriffen auf die
seltenen Offsets `0x1048`/`0x104C`/`0x1050`/`0x1054`/`0x1058`/`0x105C`
durchsucht (alle einzigartig fuer `recomp_0027.c`) und den tatsaechlichen
Konstruktor gefunden, `sub_004B1377` (`recomp_0027.c:15583`), der nur
ueber einen indirekten (Vtable-)Dispatch aufgerufen wird -- per `grep`
bestaetigt, dass er nirgendwo im generierten Code direkte Aufrufer hat,
nur einen Eintrag in `recomp_dispatch.c`. Live-One-Shot-Instrumentierung
in seinem Koerper (`ebx` = neues Objekt "this", `eax` = sein eigener
erster Parameter, ein "Info"-Struct-Zeiger) erfasste, fuer die zwei
Konstruktionen, die vor der korrumpierenden Kopie geschehen:

```
n=1 this=00FA0260 info0=F0464000 info4=00000012 info8=00000A00 infoC=00000000   (wird DST)
n=2 this=00FA12E0 info0=F020C000 info4=00000012 info8=00000A00 infoC=00000000   (wird SRC)
```

`info0`/`info8` bilden direkt auf die `+0x18`-(Basisadresse-) und
`+0x1048`-(Pitch-)Felder des konstruierten Objekts ab -- was bestaetigt,
dass `0xF0464000` von nichts in diesem Konstruktor oder seinen zwei
duennen Pro-Format-Unterklassen-Wrappern (`sub_004B36B9`/`sub_004B36D5`/
`sub_004B36F1`, einer pro Bit-Tiefen-Gruppe, jeder leitet nur denselben
"Info"-Zeiger weiter und korrigiert danach eine Vtable) berechnet wird;
es wird woertlich aus einer vom Aufrufer gelieferten "Info"-Struktur
kopiert. `info4 = 0x12` (18) passt zu einem linearen 32-bpp-Pixelformat-
Code.

### Der unmittelbare Aufrufer ist eine 19-Eintraege-Formatcode-Dispatch-Tabelle, kein "Praesentieren"-Pfad

Ein Live-Stack-Trace beim ersten Treffer des Konstruktors (`cdb`, `kb 15`,
einmalig) ergibt die volle Kette: `sub_004B1377 <- sub_004B36B9
(recomp_0027.c:21138) <- sub_004B38F7 (recomp_0027.c:21778) <-
sub_004B0F92 (recomp_0027.c:14791) <- sub_004ADE9D
(recomp_0027.c:6061) <- sub_004AE0A7 (recomp_0027.c:6357) <-
sub_002E5F95 <- sub_00037274 <- sub_0004934E <- ... <- main` -- dasselbe
Gesamt-Skelett, das diese ganze Untersuchung seit Update 223
zurueckverfolgt, jetzt drei Ebenen tiefer erweitert. `sub_004B38F7`
(`recomp_0027.c:21708`) liest ein Formatcode-Byte ueber `MEM32(eax +
0x4B3C00)` aus seinem eigenen "Info"-Argument und leitet ueber eine
**19-Eintraege-Sprungtabelle** an einen von mehreren Pro-Bit-Tiefe-
Wrapper-Konstruktoren weiter, nachdem zuerst das Wrapper-Objekt selbst
ueber `sub_00055936` (ein generischer Allokator, `0x1064` Bytes anfordernd
-- die Groesse des Wrapper-Structs selbst, nicht der Pixeldaten)
alloziert wurde. Das ist eine generische **Pixelformat-zu-Deskriptor-
Wrapper-Fabrik**, im ganzen Spiel fuer viele verschiedene Oberflaechen-/
Textur-Operationen genutzt, nichts spezifisch fuer das Anzeigen eines
Frames.

**Das schliesst "Praesentieren auf den Backbuffer" als Identitaet der
Operation aus:** geprueft, ob `AvSetDisplayMode` (dieses Projekts eigener
Display-Modus-Setz-Einstiegspunkt, zuvor beobachtet, einmal erreicht
`fb=...` zu loggen) in genau diesem Lauf ueberhaupt schon aufgerufen
wurde, bevor die Korruption ausloest -- war es nicht (`grep` nach
`SetDisplayMode` in `scda_runtime.log` bis zum Kill-Zeitpunkt: null
Treffer). Da die Anzeige-/Backbuffer-Adresse an diesem Punkt im Boot noch
nicht einmal konfiguriert ist, kann die Ziel-Oberflaeche hier nicht der
primaere Backbuffer sein; dies ist eine andere, bereits frueher erstellte
Textur oder ein Render-Target, das eine Formatkonvertierung durchlaeuft.

### Wo das den Trace zuruecklaesst, und die Scoping-Frage

`0xF0464000` wird als Parameter die ganze Kette hinab geliefert, von was
auch immer `sub_004AE0A7` aufruft (drei Funktionsebenen weiter
zurueckverfolgt als Update 226, jede eine duenne Durchleitung desselben
"Info"-Zeigerpaars, keine Berechnung). Die tatsaechliche **Allokation**
der physischen/Tiled-Adresse dieser Ziel-Textur geschieht weiter oben in
dieser Kette, bei einem noch nicht erreichten Textur- oder
Oberflaechen-Erstellungs-Aufruf. Diesen korrekt weiterzuverfolgen braucht
denselben Live-, One-Shot-, Stack-Trace-basierten Rueckwaerts-Gang, der
diese Runde und in den Updates 223-226 erfolgreich war -- er ist
mechanisch und hat einen klaren Praezedenzfall, aber die Anzahl der
verbleibenden Ebenen ist noch nicht bekannt, und er reicht jetzt in
dieses Projekts Textur-/Contiguous-Memory-Allokations-Subsystem hinein,
statt in eine einzelne, in sich geschlossene Kopierroutine.

**Noch nicht getan:** den tatsaechlichen Textur-/Oberflaechen-
Erstellungs-Aufruf zu finden, der `0xF0464000` als physische Basis
dieses Ziels zuweist, und (laut der eigenen Standing-Anweisung des
Nutzers) die Herkunft dieser Adresse gegen xemu-Ground-Truth oder das
Original-x86 zu vergleichen, um zu bestimmen, ob der Contiguous-/Tiled-
Memory-Allokator dieser Rekompilierung diese Oberflaeche irgendwohin
platziert, wo der eigene Allokator des Original-Titels es nicht getan
haette (die Arbeitshypothese aus Update 226, noch nicht bestaetigt). Kein
Fix versucht. Kein Clamp, kein gefaelschter Zeiger, kein Workaround. Alle
diese Runde hinzugefuegte temporaere Instrumentierung (zwei weitere
Runden, dasselbe Muster wie Updates 225/226) vollstaendig zurueckgesetzt;
`git status`/`git diff` bestaetigen keine Aenderungen an getrackten
Dateien (die generierte Datei ist gitignored). Alle
`scda_recomp.exe`/`cdb.exe`-Instanzen am Ende bereinigt.
"""

new_raw = raw.rstrip(b"\r\n") + b"\r\n" + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
