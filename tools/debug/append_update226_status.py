# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 226 (2026-09-21): KORREKTUR zu Update 225s eigener Diagnose -- die Zeilenkopie ist KEIN Nullpitch-/Overflow-Bug; es ist eine vollstaendige, korrekt begrenzte ~640x480-Oberflaechenkopie, deren gesamter Zielbereich legitim einen physischen Bereich ueberspannt, der zufaellig die Adresse des Device-Pointers enthaelt; die eigentliche Frage ist jetzt, woher diese Zielbasisadresse stammt

**Selbstkorrektur, im Sinne eines ehrlichen Protokolls:** Update 225s
Einzel-Checkpoint-Erfassung las `dst_pitch`/`src_pitch` aus
`dst_desc/src_desc + 0x104C` und fand beide als Null, was zur
(nachvollziehbaren, aber falschen) Hypothese fuehrte, die Kopie sei eine
degenerierte, nicht fortschreitende Schleife, die moeglicherweise ihren
eigenen Deskriptor durch einen Out-of-Bounds-Schreibzugriff selbst
korrumpiert. **Das war ein Bug in der Diagnose selbst, kein Fund ueber
das Spiel:** `+0x104C` ist ein anderes Feld, das nur einmal pro *Ebene*
gelesen wird (multipliziert mit einem Zeilenindex, der beim ersten
Gebrauch `0` ist, sodass sein Wert dort irrelevant ist) -- das Feld, das
tatsaechlich zur Fortschreibung der Pro-Zeile-Zeiger verwendet wird, ist
`+0x1048` (`recomp_0027.c`: `edx = edx + MEM32(ecx + 0x1048); eax = eax +
MEM32(ebx + 0x1048);`), das Update 225s Erfassung nicht ausgegeben hat.

### Korrigierte, zeilenweise Erfassung

Dieselbe Schleife erneut instrumentiert (dasselbe temporaere,
One-Shot-Muster, danach vollstaendig zurueckgesetzt), um jede Zeile bis
zu einer Obergrenze von 130 zu protokollieren, direkt `esi`/`edi` (die
tatsaechlich verwendeten Adressen) erfassend. Ergebnis, erste und letzte
paar Zeilen vor dem zuvor gefundenen Uebergang:

```
row=1   esi=F020CA00 edi=F0464A00
row=2   esi=F020D400 edi=F0465400
row=3   esi=F020DE00 edi=F0465E00
...
row=114 esi=F0253400 edi=F04AB400
row=115 esi=F0253E00 edi=F04ABE00   <- cur=7400087D (Update 225s Uebergang)
```

**Die Zieladresse schreitet bei jeder einzelnen Zeile um exakt `0xA00`
(2560 Bytes) fort, ohne Luecken oder Spruenge, ueber mindestens 115
aufeinanderfolgende Zeilen hinweg.** Das ist eine vollkommen gesunde,
korrekt begrenzte Kopie -- 2560 Bytes/Zeile passt exakt zu einer
640-Pixel-breiten, 4-Byte-pro-Pixel-(32bpp)-Oberflaeche, und mit
`dst_rows=0x1E0` (480) aus Update 225s eigener Erfassung ist die volle
Zielspanne `[0xF0464000, 0xF0464000 + 480*0xA00) = [0xF0464000,
0xF0590000)` -- eine schlichte, zusammenhaengende, ~1,2-MB-framebuffer-
grosse Oberflaeche in der Tiled Aperture, kein Overrun ueber irgendeine
Grenze hinaus.

**`0x004AB668` (physisch, nach Subtraktion der `0xF0000000`-Tiled-Basis)
liegt innerhalb der normalen, beabsichtigten Spanne dieses Ziels**
(`0x00464000 <= 0x004AB668 < 0x00590000`), nicht an irgendeiner Kante,
die nur durch einen Out-of-Bounds-Schreibzugriff erreicht wird. Es gibt
hier ueberhaupt keinen Kopierschleifen-Bug: gegeben diese exakte Quelle,
Ziel-Basis, Pitch und Zeilenzahl tut die Kopie genau das, was ihr
aufgetragen wurde.

### Worauf das tatsaechlich hindeutet

Der Bug liegt nicht in `sub_004AED5D`s Kopierschleife und nicht in einem
"Null-Pitch"-Defekt (Update 224s echter, eingefrorener Fall mit winziger
Adresse ist ein separater, echter Defekt in einem *anderen* Aufruf
derselben gemeinsamen Routine, wie Update 225 bereits schloss --
unberuehrt von dieser Korrektur). Die eigentliche Frage ist jetzt
eindeutig: **Woher bekommt dieser konkrete Aufruf die physische/Tiled-
Basisadresse seiner Ziel-Oberflaeche (`dst_desc + 0x18 = 0xF0464000`),
und ist diese Adresse korrekt?**

Zwei lebende Moeglichkeiten, noch nicht unterschieden:

1. **Ein echter Platzierungs-Bug im Oberflaechen-/Textur-Allokator oder
   der physischen-Speicher-Buchhaltung dieser Rekompilierung** -- etwas
   berechnet oder weist `0x00464000` als physische Basis dieser
   Oberflaeche zu, obwohl sie irgendwohin haette gehen sollen, das sich
   nicht mit der eigenen statischen Adresse des D3D-Device-Pointer-
   Globals (`0x004AB668`) ueberlappt, am wahrscheinlichsten weil der
   Contiguous-/Tiled-Memory-Allokator dieses Projekts nicht dieselben
   fixen Niedrig-Speicher-Bereiche reserviert/beruecksichtigt, die der
   Original-Xbox-Kernel/-Linker implizit schuetzt.
2. **Das ist original-treu, und die eigene physische Speicherkarte des
   Original-Titels hat diese Kollision tatsaechlich nicht** -- d.h. echte
   Hardware berechnet ebenfalls `0x00464000` (oder das Aequivalent) fuer
   genau diese Oberflaeche, aber das *Device-Pointer-Global selbst* liegt
   auf echter Hardware schlicht nicht physisch bei `0x004AB668`, und die
   statische-Daten-Platzierung dieser Rekompilierung fuer genau dieses
   eine Global weicht vom eigenen gelinkten Layout der Original-XBE ab.

Diese zu unterscheiden braucht einen Vergleich des tatsaechlichen
Global-Daten-Segment-Layouts dieser Rekompilierung gegen die eigenen
gelinkten Adressen der `default.xbe` fuer genau dieses Global (bereits
als identisch bestaetigt -- `0x004AB668`/`0x004AB670` sind literale
Konstanten, direkt aus den Original-Instruktions-Bytes entnommen, laut
der Byte-Verifikations-Arbeit dieser gesamten Untersuchung), was gegen
Erklaerung 2 auf den ersten Blick spricht: wenn der eigene kompilierte
Code des ORIGINAL-Spiels `mov eax, [0x4AB668]` fuer den Device-Pointer
hardcodiert, hat der Original-Linker dieses Global zwangslaeufig an
genau dieser physischen Adresse platziert, auch auf echter Hardware.
**Das macht Erklaerung 1 (ein Oberflaechen-Allokator-/physischer-
Speicher-Buchhaltungs-Bug spezifisch fuer diese Rekompilierung) zum
wahrscheinlicheren Kandidaten**, aber das wurde noch nicht bestaetigt,
indem der tatsaechliche Allokations-Aufruf und seine Eingaben gefunden
wurden.

### Explizit nicht getan diese Runde

Die Herkunft von `dst_desc + 0x18` (welcher Allokations- oder
Textur-Erstellungs-Aufruf es befuellt, und mit welchen Eingaben) wurde
nicht zurueckverfolgt -- das ist der klare naechste Schritt. Kein
Vergleich mit xemu-Ground-Truth fuer die physische Platzierung dieser
konkreten Oberflaeche auf echter Hardware, wozu die eigene fruehere
Standing-Anweisung des Nutzers (mit xemu vergleichen, wenn praktikabel)
aufrufen wuerde, sobald eine konkrete Allokationsstelle zum Vergleich
gefunden ist. Kein Clamp, kein gefaelschter Zeiger, kein Workaround.
Instrumentierung diese Runde erneut hinzugefuegt und vollstaendig
zurueckgesetzt (zweite Runde dieses Update-Paars, beide Male sauber per
`git status`/`git diff` bestaetigt -- die generierte Datei ist
gitignored, getrackte Dateien unangetastet).
"""

new_raw = raw.rstrip(b"\r\n") + b"\r\n" + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
