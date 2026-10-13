# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 230 (2026-09-22): der Steady-State nach Update 229 als ECHTE neue Divergenz bestaetigt (kein Testumgebungsartefakt, kein Bezug zum Contig-Alias-Fix) -- ein Menue-Baum-Knoten-Pointer, der ein dynamischer Heap-Pointer sein sollte, ist stattdessen eine festverdrahtete, unausgerichtete Adresse im Executable-Image selbst, per xemu-Ground-Truth verifiziert

Auf explizite Nutzer-Anweisung beantwortet dieser Eintrag genau eine
Frage: ist der am Ende von Update 228/229 beobachtete Steady-State-Loop
(dort faelschlich als "Bink-Worker-Critical-Section-Polling"
charakterisiert) ein echter neuer Blocker oder ein Artefakt der
Testumgebung? Der Contig-Alias-Fix aus Update 229 gilt weiterhin als
abgeschlossen und wurde nicht wieder geoeffnet, da nichts hier ihn
betrifft.

### Schritt 1-2: was der Loop tatsaechlich ist

Update 229s Charakterisierung war falsch. Die wiederholten
`[KERNEL] summary`-Zeilen sind generisches RTL-Critical-Section-/
Object-Reference-Rauschen, das zeitlich zusammenfaellt, aber nicht das
eigentliche Symptom ist. Das tatsaechliche Symptom (`[ICALL] target
0x00000000 is not code -- skipped N time(s) ... source=0x0005F701`, N
steigt im Testfenster auf ueber 1000) laeuft auf dem **Hauptthread**, nicht
auf einem Worker-Thread.

Temporaere Ein-Schuss-Instrumentierung (in der gitignoreten
`recomp_0002.c`, vollstaendig zurueckgesetzt, `git status`/`git diff`
sauber) an `sub_0005F6FF` (generischer `this=ecx; call [[ecx]+0x54]`-
Vtable-Slot-Dispatcher) erfasste bei allen 8 Treffern: `ecx(this)=
0x81660000`, alle Felder `[ecx+0/4/8/0xC]` lesen Null, und (durch Lesen
des emulierten Guest-Stacks am exakten Call-Punkt, derselbe Trick wie
den ganzen Rest dieser Sitzung ueber verwendet, um einen Caller ohne
native Backtrace zu identifizieren) eine vollstaendige Aufrufkette:

```
[Caller bei 0x622DA, im rekursiven Baum-Walk von sub_00062237]
  -> sub_00060B2E(manager=ebx, cursor=Stack-lokal)
       cursor.bucket = manager              ; = 0x00390037
       cursor.element = MEM32(manager+0x38) ; = 0x81660000
  -> sub_000600AD(cursor)                    ; laeuft cursor als Liste ab
       ecx = cursor.element                  ; = 0x81660000
       -> sub_0005F6FF(ecx)                  ; this=0x81660000, komplett-Null-Objekt
            eax = MEM32(ecx)      = 0         ; Vtable-Pointer selbst ist 0
            target = MEM32(eax+0x54) = 0      ; -> Null-Indirect-Call, uebersprungen
```

`sub_00062237` ist ein **rekursiver Menue-/Szenen-Knoten-Baum-Walker**
(`ebx` = aktueller Knoten, geprueft via `MEM8(ebx+0x90)&4`; Kindlisten-
Kopf bei `ebx+0x24`; rekursiert in sich selbst fuer Kinder mit gesetztem
Flag-Bit). Erreicht in diesem Lauf ueber dieselbe tiefe Aufrufkette in
den Haupt-Initialisierer wie in frueheren Updates etabliert, direkt
nachdem `Menu.lin.dlf` (die Level-Datei des Hauptmenues) erfolgreich
gelesen wurde.

### Schritt 3-4: die Konstante ist im XBE-Image selbst festverdrahtet -- keine Laufzeitberechnung, keine fehlenden Disc-/Streaming-Daten

`ebx = 0x00390037` wird als Objekt benutzt: `MEM8(ebx+0x90)` (ein
Flag-Byte) und `MEM32(ebx+0x38)` (das "erster registrierter Listener/
Kind"-Feld) werden direkt gelesen. Das Parsen der eigenen Section-Tabelle
von `default.xbe` und Lesen der **statischen, on-disk Image-Bytes** an
Datei-Offset fuer Guest-VA `0x00390037+0x38` (`0x0039006F`) ergibt
`00 00 66 81 ...` -- d.h. der zur Laufzeit beobachtete Wert `0x81660000`
ist **im Executable-Image selbst festverdrahtet** (Section 0, `vaddr=
0x11000`, `vsize=0x454aa8`, die zusammengelegte Code+Daten-Section,
`flags=0x36`), nicht berechnet, nicht durch irgendeinen Allokator
korrumpiert, und nicht von irgendeiner externen Disc-/Streaming-Datei
abhaengig. Sowohl `lipsyncxbox.umd` als auch `splintercell4xboxretail.umd`
sind in der Testumgebung vorhanden und werden im selben Log erfolgreich
geoeffnet/gelesen (`[SCDA-UMD-OPEN]`, `[SCDA-UMD-READ]` beide
erfolgreich) -- **die Testumgebung fehlt hierfuer keine Daten.** Das
schliesst auch unabhaengig einen Bezug zum Contig-Alias-Fix aus Update
229 aus: `0x00390037` ist eine Basis-RAM-Image-Adresse, keine
Contig-Fenster-Adresse, und die Bytes von `0x81660000` kommen aus dem
statischen Image, nicht aus `xbox_ContiguousAlloc`s Bump-Pointer.

`0x00390037` ist fuer sich genommen schon verdaechtig: nicht 4-Byte-
ausgerichtet, ungewoehnlich fuer eine echte C++-Objektadresse. Genau das
motivierte die xemu-Ground-Truth-Pruefung unten, statt "das Image enthaelt
eben diese Konstante" als Endantwort zu akzeptieren -- die offene Frage
war, ob `ebx` hier auf echter Hardware auch legitim `0x00390037` wird
(dann waere die Konstante ein absichtliches, harmloses "noch nichts
registriert"-Sentinel-Design), oder ob `ebx` selbst in dieser
Rekompilierung falsch ist.

### Schritt 5-6: xemu-Ground-Truth -- eindeutig, keine Mehrdeutigkeit

Eine Live-GDB-Remote-Sitzung gegen den xemu-Build des Nutzers (echtes
MCPX/BIOS/Disc-Image, `-s -S`, das etablierte Protokoll aus den
bestehenden `tools/debug/xemu_*.py`-Skripten dieses Projekts) setzte
einen Software-Breakpoint auf Guest-EIP `0x000622D1` -- genau das
"push ebx" unmittelbar vor dem Call zu `sub_00060B2E`, adressidentisch
auf beiden Plattformen, da dieselbe XBE. Keine Quell- oder
Binaeraenderung auf beiden Seiten; reine Beobachtung.

**92 Treffer gesammelt** (geholfen durch den Nutzer, der das Spiel
manuell ueber das passive Hauptmenue hinaus in ein Level vorangetrieben
hat, was deutlich mehr Menue-/HUD-Baum-Aktivitaet erzeugte als passives
Warten):

- `ebx` nahm 40+ unterschiedliche Werte an, **alle im Bereich
  `0x0080xxxx`-`0x00Dxxxxx`** -- gewoehnliche dynamisch allozierte
  Heap-Adressen des echten Konsolen-Allokators, die sich Treffer fuer
  Treffer aendern, wie Objekte erzeugt/zerstoert werden (z.B.
  `0x0081FD60`, `0x0082B420`, `0x00A27470`, `0x00D6BEF0`, ...).
- `[ebx+0x38]` war **entweder legitim `0x00000000`** (leer, noch keine
  Kinder/Listener -- der harmlose Fall, der `sub_000600AD`s Schleifenkoerper
  nie erreicht) **oder ein weiterer lebender Heap-Pointer**
  (`0x0082F1C0`, `0x00822900`, `0x00A99300`, ...), konsistent Treffer fuer
  Treffer fuer dasselbe `ebx`, solange das Objekt ueber Frames hinweg
  bestehen bleibt, und sich aendernd, wie der echte Allokator Speicher
  wiederverwendet/verschiebt (z.B. `0x0083DD70`s `+0x38`-Wert wandert von
  `0x0082D1A0` ueber `0x00A99300` zu `0x00A99550` ueber aufeinanderfolgende
  Treffer -- genau, wie eine live aktualisierte Liste aussieht).
- **`ebx == 0x00390037` trat kein einziges Mal in allen 92 Treffern auf.**
- Kein Treffer zeigte je einen Wert im statischen-Image-Bereich
  (`0x0000xxxx`-`0x00400000`-ish) bei `ebx` oder `[ebx+0x38]`.

**Schlussfolgerung: dies ist eine echte, bestaetigte Divergenz, kein
harmloses Sentinel-Design und kein Testumgebungsartefakt.** Auf echter
Hardware liefert, was auch immer `sub_00062237`s Knoten-Pointer speist,
immer entweder ein gueltiges, dynamisch alloziertes Heap-Objekt oder einen
sauberen leeren (`0`) Zustand. In dieser Rekompilierung produziert
mindestens ein Aufrufpfad stattdessen eine rohe, unausgerichtete Adresse,
die zu einer im Code-/Daten-Bereich der Executable festverdrahteten
Byte-Sequenz aufloest -- fast sicher ein falsch berechneter Pointer (oder
ein falsch genommener Zweig) irgendwo oberhalb von `sub_00062237`, kein
Bug in `sub_00062237`/`sub_000600AD`/`sub_0005F6FF` selbst, die treu tun,
was jede korrekt gespeiste Aufrufkette mit jedem ihr uebergebenen Pointer
tun wuerde.

### Noch nicht erledigt

Der Erzeuger des falschen `0x00390037`-Werts -- d.h. welcher Caller von
`sub_00062237` (oder welcher vorgelagerte Schreibzugriff auf das eigene
`+0x24`-Kind-Pointer-Feld eines Knotens) diese Adresse statt eines echten
Heap-Pointers liefert -- wurde noch nicht zurueckverfolgt. Das ist der
konkrete naechste Schritt: diesen Erzeuger finden, dessen Original-x86
gegen den generierten C-Code an genau dieser Stelle vergleichen, und den
ersten falschen Wert identifizieren, gemaess der stehenden
Nutzer-Anweisung.

Kein Stub, kein Fake-Signal, kein Clamp und kein Bink-spezifischer
Workaround wurden eingefuehrt. Die eine temporaere Diagnose an
`sub_0005F6FF` (gitignorete `recomp_0002.c`) wurde vollstaendig
zurueckgesetzt und die saubere Baseline vor dem xemu-Vergleich neu
gebaut; `git status`/`git diff` zeigen keine getrackten Aenderungen aus
diesem Eintrag. Die xemu-Seite hat ueberhaupt keine Aenderungen
vorgenommen (rein lesende GDB-Remote-Speicher-/Register-Inspektion).
"""

new_raw = raw.rstrip(b"\r\n") + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
