# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 224 (2026-09-21): Minimal-invasive GOOD->BAD-Bisektion entlang des tatsaechlich ausgefuehrten Pfads grenzt die 0x4AB668-Korruption auf eine konkrete Zeilenkopier-Schleife in sub_004AED5D ein; deren eigener Zielbereich ueberlappt 0x4AB668 nicht, kein anderer Thread ist zum Korruptionszeitpunkt aktiv, und ein praezise eingegrenzter Hardware-Watchpoint loest trotzdem nicht aus -- deutet auf eine Host-Speicher-Aliasing-Luecke statt auf einen gewoehnlichen Guest-Store hin

**Methode, laut expliziter Anweisung dieser Runde:** hierarchische
One-Shot-Checkpoint-Bisektion entlang des echten Call-Pfads zwischen dem
letzten bestaetigten GUTEN Device-Pointer-Lesevorgang (direkt nach
Rueckkehr aus `sub_0049D210`) und dem ersten bestaetigten SCHLECHTEN
Lesevorgang (in `sub_004A12B0`), wobei bei jedem Checkpoint nur
`MEM32(0x4AB668)` (plus wenige Register) per Live-`cdb`-Breakpoints
gelesen wird -- keine Quellcode-Instrumentierung, keine
Hot-Path-Ausgabe, frischer Prozess pro Lauf. Hierarchisch eingegrenzt:

```
sub_0004934E (Icall +0x3263f, GUT) -> sub_002E75CE -> sub_002E2BB1 ->
sub_0049D210 [[Geraeteerstellung erfolgreich, schreibt 0x004AB670]]
    ... (sub_0004934Es eigener Koerper, ~60 Zeilen generierter C-Code,
         ein Kandidaten-Store bei recomp_0002.c:8023-aehnlich direkt
         ausgeschlossen) ...
sub_0004934E (Icall +0x34b3e, direkt davor noch GUT) -> sub_00037274 ->
sub_002E5F95 -> sub_0049B0C0 [[bereits bei Eintritt SCHLECHT]]
```

Erneute Bisektion innerhalb von `sub_002E5F95` (714 Zeilen generierter
C-Code) grenzte den Uebergang auf ein 150-Zeilen-Fenster ein, dann auf
einen einzigen Aufruf: `sub_004AE0A7` (davor gut, danach schlecht).
Erneute Bisektion innerhalb von `sub_004AE0A7` grenzte auf einen
einzigen Aufruf ein: `sub_004ADE9D` (davor gut, danach schlecht). Erneute
Bisektion innerhalb von `sub_004ADE9D` (eigene ~260 Zeilen, keine
direkten Nicht-Lokal-Stores per statischem Grep) grenzte auf einen
einzigen Aufruf ein: `sub_004B0F92` (gut vor Zeile 14747, schlecht bei
eigenem Ausgang Zeile 14967). Erneute Bisektion der neun verketteten
Setup-Aufrufe von `sub_004B0F92` fand: nur einer (`sub_004AED5D`) wird in
diesem Lauf ueberhaupt erreicht -- die anderen acht werden ueber einen
Fruehausstieg bei Erfolg uebersprungen. **Endgueltige Lokalisierung: die
Korruption geschieht irgendwo innerhalb von `sub_004AED5D`
(`recomp_0027.c:7881-8080`), konkret nach Beginn seiner
Zeilenkopier-Schleife (`recomp_0027.c:8016-8041`, live bestaetigt noch
GUT bei Zeile 8023) und vor seinem eigenen Ausgang (SCHLECHT bei
Zeile 14967s aeusserem Aufrufer-Checkpoint).**

### Ein echter, unabhaengiger Defekt nebenbei gefunden: die Zeilenkopier-Schleife schreitet nie fort

Das Durchlaufen jeder Iteration der Schleife bei `recomp_0027.c:8023`
(480 Iterationen in einem Lauf erfasst) zeigt `esi`/`edi` **eingefroren
auf denselben Werten (`0x00000394`/`0x00000398`) bei JEDER einzelnen
Iteration** -- der pro Zeile am Ende jeder Iteration addierte Pitch-Wert
(`recomp_0027.c:8034-8035`, `MEM32(ecx+0x1048)`/`MEM32(ebx+0x1048)`) muss
sich als Null lesen. Das ist ein echter, reproduzierbarer Defekt (ein
"kopiere jede Zeile fuer immer an dieselbe Stelle"-Bug), unabhaengig von
der 0x4AB668-Frage, noch nicht root-caused oder behoben -- fuer
Nachverfolgung markiert, diese Runde laut Anweisung (erst bisektieren)
nicht weiter verfolgt.

### Die von dieser Schleife kopierten Adressen ueberlappen 0x4AB668 ueberhaupt nicht

`esi=0x394, edi=0x398, ecx(dwords)=0x3a4` (Zielbereich
`[0x398, 0x1228]`) liegt nirgendwo in der Naehe von `0x4AB668`. Live
bestaetigt: der Geraete-Zeiger kippt in einem Lauf exakt zwischen
Schleifen-Iteration 115 und 116 von GUT auf SCHLECHT (`dd
0x7000004ab668` bei jeder Iteration geprueft) -- aber die Adressen der
Kopie selbst aendern sich ueber keine dieser Iterationen. **Dieses
konkrete memcpy kann laut seinem eigenen Quell-/Zielbereich nicht der
direkte Schreiber von 0x4AB668 sein.**

### Thread-Race direkt ausgeschlossen

Im exakten Moment des GUT->SCHLECHT-Uebergangs wurden alle 5 laufenden
Threads geloggt (`~*kb 3`): Thread 0 (unserer) sitzt am Checkpoint
innerhalb von `sub_004AED5D`/`sub_004B0F92`/`sub_004ADE9D`; Threads 1-3
sind untaetige Threadpool-Worker in `NtWaitForWorkViaWorkerFactory`;
Thread 4 ist dieses Projekts eigener `nv2a_ack_thread`, der in `SleepEx`
sitzt (mitten im Schlaf, fasst keinen Guest-Speicher an). **Kein anderer
Thread ist aktiv oder koennte in diesem Moment Guest-Speicher
schreiben** -- die Korruption ist single-threaded auf Thread 0, was die
anfangs vermutete Race-Condition-Hypothese ausschliesst.

### Plan B (Hardware-Schreib-Watchpoint), diesmal praezise eingegrenzt: loest trotzdem nicht aus

Laut Anweisung ("Hardware-Watchpoint nur als Plan B") wurde
`ba w4 <Host-Adresse von 0x4AB668>` scharf gemacht, aber *deaktiviert*
gehalten, bis der Schleifenzaehler bei `recomp_0027.c:8023` 100 erreichte
(komfortabel vor dem oben beobachteten 115-116-Uebergang), dann aktiviert
(`bd`/`be` auf die Breakpoint-ID, um die verschachtelte Anfuehrungszeichen-
Problematik eines dynamischen `ba` innerhalb einer anderen
Breakpoint-Aktion zu vermeiden) und den Prozess von dort an frei laufen
lassen. Per `cdb -pv`-Schnappschuss danach bestaetigt: der Prozess hatte
bereits den finalen `sub_004A12B0`-Spin erreicht (d.h. die Korruption war
bereits geschehen) -- **der Watchpoint hat nie ausgeloest**, obwohl er
nur fuer das ~15-Iterationen-Fenster scharf war, in dem der Uebergang
bekanntermassen stattfindet. Das ist ein deutlich engeres, schluessigeres
Negativergebnis als der breite, verrauschte Versuch in Update 223 (dort
musste der Watchpoint Minuten unabhaengiger Ausfuehrung ueberstehen; hier
musste er nur ein paar Dutzend Schleifen-Iterationen bereits isolierten
Codes ueberstehen, und hat trotzdem versagt).

### Angeforderte Pruefung: kommt 0x7400087D anderswo im Guest-Speicher vor?

Der gesamte 64-MB-Guest-RAM wurde durchsucht (`s -d` ueber
`0x7000000000000` L?`0x4000000`) nach dem Dword-Muster `0x7400087D` auf
dem bereits korrumpierten, weiterlaufenden Prozess: **null Treffer
irgendwo sonst** (ein Sanity-Check mit kleinem Suchbereich um `0x4AB668`
selbst bestaetigte, dass der Suchbefehl korrekt funktioniert). Fuer sich
genommen nicht schluessig -- die Quelldaten, falls vorhanden, koennten
durch weitere Ausfuehrung seit dem korrumpierenden Schreibzugriff bereits
ueberschrieben worden sein -- stuetzt aber auch keine einfache "Streu-
Kopie aus einem noch lebendigen, intakten Puffer"-Erklaerung.

### Arbeitshypothese, noch nicht bestaetigt: Host-seitiges Speicher-Aliasing

Die Kombination aus (a) der identifizierten Kopie, deren eigene Adressen
0x4AB668 nie ueberlappen, (b) keinem anderen aktiven Thread, und (c)
einem praezise eingegrenzten Hardware-Watchpoint, der trotzdem nicht
ausloest, deutet am plausibelsten auf etwas hin, das schon viel frueher
in der Geschichte dieses Projekts als offene architektonische Luecke
markiert wurde (Doku, "continued 53", 2026-09-06): Guest-Speicher koennte
ueber mehr als eine Host-virtuelle-Adresse erreichbar sein (z.B. eine
separate Mapping/View, die von einem Bulk-Copy-Fast-Path genutzt wird),
sodass ein auf der "kanonischen" `g_xbox_mem_offset`-uebersetzten Adresse
scharf gemachter Watchpoint einen Schreibzugriff, der ueber eine andere,
dieselbe Guest-Seite hinterlegende Host-VA laeuft, nie sehen wuerde. Das
wurde fuer DIESE konkrete Korruption noch nicht bestaetigt -- es ist die
konsistenteste Erklaerung der in diesem Eintrag gesammelten Evidenz,
keine bewiesene Grundursache.

### Explizit nicht getan, laut Anweisung

Kein Clamp, kein gefaelschter/Stub-Device-Pointer, kein anderer
Workaround. Keine Quelldateien diese Runde geaendert -- `git
status`/`git diff` bestaetigen reine Doku-Aenderungen (plus neue
Debug-Skripte in `tools/debug/`). Alle
`scda_recomp.exe`/`cdb.exe`-Instanzen am Ende beendet.

### Empfehlung fuer den naechsten Schritt

Das hier vollstaendig root-zu-causen braucht jetzt eine andere Art von
Untersuchung als die bisherige Bisektion: ein Audit des
Guest-Speicher-Allokations-/Mapping-Codes dieses Projekts
(`src/kernel/xbox_memory_layout.c`, `kernel_bridge.c`s
`NtAllocateVirtualMemory`-Pfad, schon einmal frueher markiert) auf jeden
Fall, in dem der Adressbereich um `0x4AB668` (oder der Zielbereich der
Schleife selbst) ueber mehr als eine Host-VA erreichbar ist. Falls kein
solches Aliasing existiert, ist die Alternative eine
Page-Guard-basierte (nicht Debug-Register-basierte) Speicherfalle, die
jeden Zugriff unabhaengig von Host-Adress-Aliasing faengt, auf Kosten
von deutlich mehr Overhead und Invasivitaet -- eine grundlegend andere
Technik als alles bisher in dieser Sitzung Versuchte.
"""

new_raw = raw.rstrip(b"\r\n") + b"\r\n" + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
