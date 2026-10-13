# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 225 (2026-09-21): KONKRETER WRITER GEFUNDEN -- die 0x4AB668-Korruption ist eine Tiled-Aperture-Oberflaechen-/Zeilenkopie in sub_004AED5D, deren Zielbereich sich physisch mit dem D3D-Device-Pointer ueberlappt; eine cdb-`.frame N`-Methodik-Falle (zurueckgezogen) und eine Runde echter, minimaler One-Shot-Quellcode-Instrumentierung bestaetigen das praezise

**Zwei falsche Spuren diese Runde untersucht und im Sinne eines ehrlichen
Protokolls ausdruecklich zurueckgezogen:**

1. **Host-Speicher-Aliasing ueber RAM-Mirrors wurde geprueft und trifft auf
   diese Schleife nicht direkt zu.** `XBOX_PTR(addr) =
   (uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset`
   (`templates/runtime/recomp_types.h:460`) macht kein Wraparound/Masking,
   sodass eine kleine Guest-Adresse wie das zuvor beobachtete `0x394` (aus
   einem *anderen*, unabhaengigen, harmlosen Aufruf derselben gemeinsamen
   Kopierroutine) selbst keinen 64-MB-RAM-Mirror erreichen kann. Der
   RAM-Mirror-Mechanismus in `xbox_memory_layout.c` (dort ausfuehrlich
   dokumentiert) ist real, war aber hier nicht der wirkende Mechanismus.
2. **Eine vermutete Stack-Pointer-Korruption (`ebp` liest sich als `0x3C0`
   in drei verschiedenen `cdb`-Stack-Frames via `.frame N; ? ebp`) wurde
   untersucht und ist ein Debugging-Methodik-Artefakt, kein echter Fund.**
   Erneute Pruefung mit dem frame-bewussten `dv`-Befehl (statt `? name`,
   das bei einem `.frame`-Wechsel fuer einen auch in aeusseren Frames
   existierenden Variablennamen still auf ein Pseudoregister/falschen
   Scope zurueckfallen kann) zeigt vollkommen normale, unterschiedliche,
   korrekt verschachtelte Stack-Adressen pro Frame (`ebp=0xf7b0cc` in
   `sub_004AED5D`, `ebp=0xf7b188` in `sub_004ADE9D`, zwei Frames hoeher) --
   der Stack ist gesund. Lektion fuer kuenftige `cdb`-Arbeit in diesem
   Projekt: niemals `? name` ueber einen `.frame`-Wechsel fuer einen auch
   in aeusseren Frames existierenden Variablennamen vertrauen; stattdessen
   `dv` (frame-gescopte Locals-Auflistung) verwenden.

### Der echte Mechanismus, direkt gefangen mit einer Runde Quellcode-Instrumentierung

Laut der eigenen expliziten Fallback-Genehmigung des Nutzers
("instrumentiere ausschliesslich die Stores dieses kleinen Pfads, die die
konkrete Device-Pointer-Adresse ueberlappen") wurde eine minimale,
One-Shot-, static-flag-gegatete Diagnose direkt in das generierte
`sub_004AED5D` in `tools/recomp/output/scda_v060/gen/recomp_0027.c`
eingefuegt (gitignored, temporaer, am Ende dieses Eintrags vollstaendig
zurueckgesetzt): eine kleine statische Hilfsfunktion, die `MEM32(0x4AB668)`
gegen den bekannten guten Wert `0x004AB670` an fuenf Checkpoints rund um
die Dword-Kopie, die Byte-Rest-Kopie, die Pro-Zeile-Zeiger-Neuberechnung,
den aeusseren Schleifentest und den Funktionsausgang der
Zeilenkopier-Schleife vergleicht -- und nur beim ERSTEN Mal ausloest
(Markerdatei schreiben plus ein `fprintf`), wenn der Wert sich aendert,
nachdem er zuvor korrekt beobachtet wurde. Ein frischer Start (Build musste
nur die eine geaenderte Uebersetzungseinheit neu kompilieren) ergab, ueber
das eigene `scda_runtime.log` des Spiels (dessen `stderr` wird von `main.c`
per `_dup2` dorthin umgeleitet -- eine externe `cdb`/Redirect-Erfassung
sieht das nie, weshalb der allererste Startversuch nichts im
Konsolen-umgeleiteten Log zeigte, bis das erkannt und stattdessen
`scda_runtime.log` geprueft wurde):

```
SCDA-DIAG224 cp=after-dwordcopy bad=7400087D esi=F0253E00 edi=F04ABE00
    ecx=00000000 ebx=00000A00 edx=F0253400 eax=F04AB400 ebp=00F7B0CC
```

Dieser Checkpoint loest unmittelbar nach dem Dword-`memcpy` der Schleife
aus (`recomp_0027.c` -- der `if (!g_df) { memcpy(...); esi += ecx*4; edi
+= ecx*4; }`-Zweig bei der vor der Instrumentierung ~Zeile 8047). An
diesem Punkt spiegeln `esi`/`edi` bereits die Post-Kopie-(fortgeschrittenen)
Zeiger wider, und `ebx` haelt noch die Vor-Shift-Bytezahl (`0xA00` = 2560
Bytes), die zwei Zeilen zuvor berechnet wurde -- die soeben ausgefuehrte
Kopie bewegte also 2560 Bytes von Quelle `[esi_vorher, esi_vorher+0xA00) =
[0xF0253400, 0xF0253E00)` nach Ziel `[edi_vorher, edi_vorher+0xA00) =
[0xF04AB400, 0xF04ABE00)`.

**Beide Adressen liegen in der Tiled Aperture** (`XBOX_TILED_BASE =
0xF0000000`, in `xbox_memory_layout.c` dokumentiert als "The NV2A exposes
physical RAM a second time here and titles render through it" -- eine
`View` derselben Datei-Mapping-Section wie die Basis-RAM, bei einem
`+0xF0000000`-Guest-Offset). Subtrahiert man die Tiled-Basis, ergibt sich
der physische Bereich, den das Ziel dieser Kopie tatsaechlich beruehrt:
`[0x004AB400, 0x004ABE00)`.

**`0x004AB668` (die eigene Adresse des Device-Pointers) faellt genau in
diesen Bereich** (`0x004AB400 <= 0x004AB668 < 0x004ABE00`, bei Offset
`0x268` innerhalb der 2560-Byte-Zielspanne). **Dieser eine `memcpy`-Aufruf
ist der direkte Schreiber.** Er ueberschreibt die Device-Pointer-Globale
mit was auch immer am entsprechenden Offset in den QUELL-Oberflaechendaten
stand (`[0xF0253400, 0xF0253E00)`, d.h. physisch `[0x00253400,
0x00253E00)`) -- weshalb der korrumpierte Wert (`0x7400087D`) wie
beliebige Daten aussah statt wie ein Null- oder offensichtlich-ungueltiger
Sentinel-Wert: es sind echte Pixel-/Oberflaechen-Bytes aus einem
unabhaengigen Puffer, kein Muell im ueblichen Sinne.

### Warum der fruehere Hardware-Watchpoint (Update 224) nie ausloeste -- jetzt vollstaendig erklaert

Der Watchpoint war auf `g_xbox_mem_offset + 0x004AB668` scharf gemacht --
die Host-Adresse der **Basis-RAM-View** fuer dieses Guest-Byte. Der
korrumpierende Schreibzugriff laeuft stattdessen ueber `XBOX_PTR(edi)` mit
`edi` in der **Tiled Aperture**, d.h. `g_xbox_mem_offset + 0x004AB668 +
0xF0000000`, uebersetzt ueber `g_tiled_view`s *separates*
`MapViewOfFileEx`-Mapping (eine andere Host-virtuelle-Adresse, die
ueber die gemeinsame, in `xbox_memory_layout.c` dokumentierte
Datei-Mapping-Section dieselben physischen Bytes aliast). Ein
CPU-Daten-Breakpoint vergleicht *lineare Adressen*, nicht physische
Seitenidentitaet, sodass ein auf der Basis-View-Adresse scharf gemachter
Watchpoint niemals fuer einen ueber die Tiled-View-Adresse laufenden
Schreibzugriff ausloesen kann, egal wie eng das Beobachtungsfenster
eingegrenzt ist. Das ist genau die "Host-Speicher-Aliasing"-Arbeitshypothese
aus Update 224, jetzt praezise identifiziert (konkret die Tiled Aperture,
nicht ein 64-MB-RAM-Mirror) und per Live-Erfassung bestaetigt statt nur
durch Ausschlussverfahren erschlossen.

### Erklaert auch die fruehere "Schleife schreitet nie fort"-Beobachtung als separaten, korrekten Aufruf

Update 224s Fund, dass dieselbe Schleifen `esi`/`edi` in einem frueheren
Lauf ueber 480 Iterationen auf winzigen Werten (`0x394`/`0x398`) eingefroren
waren, wird durch diesen Eintrag nicht widerlegt -- `sub_004AED5D` ist
offensichtlich eine **generische, gemeinsam genutzte Zeilenkopier-Routine,
die in dieser Boot-Phase fuer viele verschiedene Oberflaechen aufgerufen
wird** (sowohl der Lauf mit winzigen Adressen als auch dieser
Tiled-Aperture-Lauf teilen exakt dasselbe Call-Chain-Skelett bis hinauf zu
`sub_0004934E`/`sub_00037274`/`sub_002E5F95`/`sub_004AE0A7`/
`sub_004ADE9D`/`sub_004B0F92`, ueber mehrere Erfassungen hinweg identisch
bestaetigt). Das nichtdeterministische Timing verschiedener Laeufe erreicht
offenbar *unterschiedliche einzelne Aufrufe* dieser gemeinsamen Funktion
zuerst -- manchmal einen harmlosen mit degenerierten Nahe-Null-Adressen
(ein separater, echter Defekt: bei jenem Aufruf lesen sich die eigenen
Pitch-/Groessenfelder als Null, eine Nachverfolgung wert, aber nicht die
Ursache dieser Korruption), manchmal -- wie in diesem Eintrag -- der
konkrete Aufruf, dessen Ziel sich mit dem Device-Pointer ueberlappt. Das
bedeutet auch: der in Update 224 gefundene "Schleife schreitet nie
fort"-Defekt ist bestaetigt unabhaengig von diesem hier; beide sind echt,
beide leben in Kopien, die von derselben gemeinsamen Funktion fuer
unterschiedliche Aufrufer/Oberflaechen gemacht werden.

### Noch nicht getan: die Zieladresse bis zu ihrer letztendlichen Quelle zurueckverfolgen und gegen das Original-x86 vergleichen

Laut der eigenen naechsten-Schritt-Anweisung des Nutzers besteht die
verbleibende Arbeit darin, zurueckzuverfolgen, woher `edi`s
Tiled-Zieladresse (und die Bytezahl in `ebx`) tatsaechlich stammen -- d.h.
welcher Oberflaechen-/Textur-Deskriptor `sub_004AED5D` bei diesem konkreten
Aufruf seine Parameter liefert, ob die Basisadresse oder der Pitch dieses
Deskriptors relativ zum Original-x86 korrekt berechnet wird, und ob der
wahre Bug (a) darin liegt, dass die Basisadresse/Groesse der Ziel-
Oberflaeche falsch berechnet wird (ein Overrun, der in irgendeiner Form
auch auf echter Hardware auftreten wuerde, nur nicht notwendigerweise
genau diese Globale trifft), oder (b) eine Diskrepanz spezifisch fuer die
Platzierung des Device-Pointer-Globals dieser Rekompilierung relativ dazu
ist, wo der Original-XBE-Linker den kollidierenden Oberflaechenpuffer
tatsaechlich platziert hat. Das wurde diese Runde nicht begonnen -- als
klarer naechster Schritt markiert, passend zur eigenen expliziten
Anweisung des Nutzers, und genau eine Frage zur getreuen Uebersetzung, fuer
die die etablierte Methodik dieses Projekts (Capstone-Byte-Vergleich gegen
`default.xbe`) gut geeignet ist.

### Prozess-Hygiene

Alle fuenf `scda_diag224_check`-Aufrufstellen und die Hilfsfunktion wurden
nach der Erfassung aus `recomp_0027.c` entfernt (visuell gegen die
Vor-Instrumentierungs-Lesungen in diesem und dem vorherigen Eintrag
geprueft, um einen sauberen Revert zu bestaetigen); das Projekt baute
sauber neu und kompilierte nur diese eine Uebersetzungseinheit neu. `git
status`/`git diff` bestaetigen null Aenderungen an getrackten Dateien (die
generierte Datei ist gitignored). Kein Clamp, kein gefaelschter Zeiger,
kein Workaround. Alle `scda_recomp.exe`/`cdb.exe`-Instanzen und die
zurueckgebliebene `scda_diag224_success.marker`-Datei am Ende bereinigt.
"""

new_raw = raw.rstrip(b"\r\n") + b"\r\n" + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
