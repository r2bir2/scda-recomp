# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 231 (2026-09-22): den falschen Wert 0x00390037 eine Aufrufebene weiter zurueckverfolgt -- er ist ein durchgereichtes, geerbtes `ebx`, nichts, was sub_00062237s eigener Caller frisch berechnet; der Erzeuger ist auf sub_00048B15s eigenen Funktionskoerper eingegrenzt, aber noch nicht punktgenau gefunden

Direkte Fortsetzung von Update 230, nach derselben stehenden Anweisung
(den ersten falschen Wert finden, keine Abkuerzungen). Dieser Eintrag
aendert Update 230s Schlussfolgerung nicht (weiterhin eine echte,
bestaetigte Divergenz, weiterhin ohne Bezug zum Contig-Alias-Fix aus
Update 229) -- er grenzt nur ein, woher der falsche Wert kommt.

### Der externe Caller von sub_00062237, und dessen eigenes Dreifach-Push

Temporaere Instrumentierung (in der gitignoreten `recomp_0002.c`,
zurueckgesetzt und sauber neu gebaut, per `git status` bestaetigt) am
eigenen Entry-Punkt von `sub_00062237`, die den rohen Stack vor dessen
Prolog liest, fing alle 5 Treffer mit `incoming_ebx_param == 0x00390037`
bei der **identischen Caller-Rueckkehradresse `0x0004A31D`** ein, jedes
Mal. Diese Call-Stelle, innerhalb von `sub_0004934E`
(`recomp_0002.c:19467-19472`), lautet:

```c
ecx = MEM32(0x6012D4);
ecx = MEM32(ecx + eax * 4);   /* ecx (das neue "this"/edi) = ein echter Array-Lookup */
PUSH32(esp, ebx);
PUSH32(esp, ebx);
PUSH32(esp, ebx);              /* alle drei Argumente sind dasselbe ebx */
call sub_00062237
```

Das ist Byte-getreu zum Original-x86 (dreimal "push ebx" ist genau, was
die Disassemblierung sagt) -- kein Uebersetzungsfehler an dieser Stelle.
`ecx` (die Wurzel/"this" des Baums) ist ein legitimer Array-indizierter
Lookup, ohne Bezug zum falschen Wert. Der falsche Wert ist rein `ebx`,
unveraendert geerbt von dem, womit `sub_0004934E` selbst aufgerufen wurde.

### ebx wird bis zu zwei weiteren Funktionen einfach durchgereicht

Statische Pruefung: weder `sub_0004934E` noch `sub_00048F1D` (deren
komplette Koerper nach irgendeiner `ebx = ...`-Zuweisung durchsucht)
schreiben je auf `ebx` -- genau passend zur echten x86-cdecl-Callee-
Saved-Semantik fuer eine Funktion, die dieses Register schlicht nicht
nutzt. Wenn das Original-x86 dieser beiden Funktionen `ebx` ebenfalls nie
anfasst (passend zu sauberer Rekompilierung, kein anderes Bug-Muster in
beiden Funktionen bei dieser Pruefung gefunden), muss `ebx` beim Erreichen
von `sub_0004934E` **das sein, was deren gemeinsamer Caller uebergeben/
hinterlassen hat**.

Die Kette weiter nach oben verfolgt: `sub_0004934E` wird von
`sub_00048B15` per **Tail-Call** erreicht (`recomp_0002.c:15529`,
`sub_0004934E(); return; /* tail jmp 0x0004934E */`) -- kein normaler
`RECOMP_ABI_CALL`, weshalb ein frueherer Grep nach direkten Callern leer
blieb und korrigiert werden musste. Statische Suche im kompletten
~550-Zeilen-Koerper von `sub_00048B15` findet genau **eine**
`ebx`-Zuweisung: `ebx = 0` direkt nach dessen Stack-Probe-Prolog
(`recomp_0002.c:15014`), und nichts weiteres weist ihm vor dem Tail-Call
bei Zeile 15529 einen neuen Wert zu.

### Der offene Widerspruch

Woertlich genommen sagt das: `ebx` sollte beim Erreichen von
`sub_0004934E` (und damit `sub_00062237`) `0` sein -- nicht `0x00390037`.
Da die Live-Diagnose beweist, dass es tatsaechlich `0x00390037` ist, gilt
eine der folgenden Moeglichkeiten, noch nicht unterschieden:

1. **Ein Register-Erhaltungs-Bug in einem der ~25 direkten Callees von
   `sub_00048B15`** (diese Runde aufgelistet: `sub_00035F65`,
   `sub_00036116`, `sub_000370F1`, `sub_000371A6`, `sub_0003E9D9`,
   `sub_0003EAE3`, `sub_00042082`, `sub_00043455`, `sub_00044C15`,
   `sub_00045584`, `sub_00045647`, `sub_00048856`, `sub_00067886`,
   `sub_000679C1`, `sub_00070854`, `sub_00070B06`, `sub_00070BDA`,
   `sub_00070CC6`, `sub_000715C7`, `sub_00071B49`, `sub_001CAD08`,
   `sub_001CB603`, `sub_001D47CF`, `sub_001D589D`, `sub_0026AF03`) -- wenn
   das Original-x86 dort `ebx` als Callee-Saved behandelt und die
   Uebersetzung eines davon es nicht korrekt wiederherstellt, wuerde
   `ebx` stillschweigend einen unrelated Wert uebernehmen, den dieser
   Callee hinterlassen hat.
2. **Ein anderer Codepfad wird tatsaechlich genommen**, als das statische
   Lesen von `sub_00048B15` nahelegt -- z.B. ein Zweig, den dieses
   statische Lesen nicht beruecksichtigt hat und der die `ebx = 0`-Zeile
   ganz ueberspringt, wodurch die Suche eine Ebene hoeher weitergeht.

### Noch nicht erledigt

Die Unterscheidung zwischen diesen beiden erfordert entweder Live-
Bisektion (Pruefen von `ebx`s Wert an aufeinanderfolgenden Stellen
innerhalb von `sub_00048B15`, Eingrenzung auf den genauen verantwortlichen
Callee oder Zweig -- dieselbe Ein-Schuss-Instrumentierungstechnik wie die
ganze Sitzung ueber verwendet) oder ein Byte-genaues Register-Erhaltungs-
Audit der aufgelisteten Callees gegen das Original-x86. Beides diese
Runde nicht erledigt; als naechster konkreter Schritt vorgemerkt. Update
230s Schlussfolgerung bleibt unveraendert: per xemu-Ground-Truth
bestaetigt ein echter Bug, kein Testumgebungsartefakt, und ohne Bezug zum
Fix aus Update 229 (der weiterhin abgeschlossen bleibt).

Kein Stub, kein Fake-Signal, kein Clamp. Alle Instrumentierung dieser
Runde war temporaer, in der gitignoreten `recomp_0002.c`, vollstaendig
zurueckgesetzt, und die saubere Baseline wurde vor diesem Eintrag neu
gebaut; `git status`/`git diff` zeigen keine getrackten Aenderungen.
"""

new_raw = raw.rstrip(b"\r\n") + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
