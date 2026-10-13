# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 222 (2026-09-21): Neuer Blocker diagnostiziert -- sub_004A12B0s Poll-Loop dreht sich endlos, wartend auf ein PFIFO/PGRAPH-artiges Hardware-Abschluss-Bit ueber einen D3D-/NV2A-Geraetezeiger, der nie ordentlich erstellt wurde -- bestaetigt und praezisiert die bereits bekannte "keine Video-Backend verdrahtet"-Luecke

**Verfolgte Aufgabe:** gemaess der eigenen letzten Anweisung dieser Runde
den neuen `sub_004A12B0`/`sub_004A1600`/`sub_004A1740`-Poll-Loop als
separaten naechsten Blocker untersuchen, zuerst pruefen, ob
`recomp_manual.c`s bestehender `sub_00040A88`-Kommentar (eine fruehere,
2026-09-05/11-UMOD-Reader-Untersuchung, die dieselbe Adresse bereits
erwaehnt) das bereits abdeckt.

**`sub_004A12B0` gelesen** (`recomp_0026.c:133445`, Kategorie
`game_render`, Original 0x004A12B0-0x004A1360, 176 Bytes/47 Instruktionen).
Die Live-`cdb`-Snapshots aus Updates 220/221 (abwechselnd `+0x1e8`/`+0x1d0`,
eine enge 2-Instruktionen-Selbstschleife im kompilierten x64-Output)
passen exakt zu dieser Quellstelle:

```
loc_004A12C9: ;
    eax = MEM32(0x4AB668);        // der bekannte D3D-/NV2A-Geraetezeiger
    eax = MEM32(eax + 0x1C28);    // -> PFIFO/PGRAPH-artiger Registerblock
    ecx = MEM32(eax + 0x100410);
    ecx = ecx | 0x10000;
    MEM32(eax + 0x100410) = ecx;  // ein "Kick"-Bit setzen

loc_004A12F0: ;
    if (TEST_NZ(MEM32(eax + 0x100410), 0x10000)) goto loc_004A12F0; // Spin
```

Das ist ein klassisches Hardware-Register-Polling: ein Steuer-Bit setzen,
um eine GPU-Operation anzustossen (Offset `0x100410` sieht stark nach
einem NV2A-PFIFO-/Cache-Steuerregister aus), dann warten, bis die Hardware
es beim Abschluss zurueckgesetzt hat.

**Live bestaetigt, dass der Geraetezeiger ungueltig ist.** `MEM32(0x4AB668)`
auf einer frischen, live in genau dieser Schleife feststeckenden Instanz
gelesen (per `cdb`, uebersetzt via `g_xbox_mem_offset`): **`0x7400087D`** --
eine Adresse weit ausserhalb des 64-MB-Guest-RAM-Bereichs dieses Recompilers
(`XBOX_TOTAL_RAM = 0x04000000`), das heisst kein legitimer, initialisierter
Guest-Heap-Zeiger auf eine echte Geraetestruktur. Das Dereferenzieren davon
(`+0x1C28`, dann `+0x100410`) landet auf was auch immer die
Guest-Speicherzugriffs-Implementierung fuer eine ausserhalb-des-Bereichs
liegende Guest-Adresse aufloest (kein Absturz -- der Prozess dreht sich
nur, stabil bei ~16% CPU, ohne je zu faulten) -- fast sicher
gewrappt/maskiert in irgendeinen unbeteiligten Bereich der 64-MB-Guest-RAM,
der keinen Grund hat, sein `0x10000`-Bit je zu loeschen, da nichts in
diesem Recompiler echte NV2A-/GPU-Hardware-Abschluesse dafuer emuliert.

**Das ist die konkrete Bestaetigung einer bereits bekannten Luecke, kein
neuer, unabhaengiger Bug.** Das Gedaechtnis (die eigenen Standing Notes
dieser Sitzung, "SCDA no video pipeline") hatte bereits festgehalten:
"D3D-Geraet (0x4AB668) nie erstellt und scda_recomp hat null Video-Backend
verdrahtet, also kann noch nichts gerendert werden; naechster Schritt ist
ein guenstiger cdb-Erreichbarkeits-Check, nicht mehr Soak oder ein
Input-Framework." Dieses Update IST genau dieser Erreichbarkeits-Check, und
die Antwort ist jetzt endgueltig: nachdem der Update-220/221-Cursor-Formel-
Fix die fruehere, unabhaengige `sub_00040821`/`sub_0003FCA5`-Korruption
beseitigt hat, schreitet die Boot-Sequenz jetzt weit genug fort, um
erstmals TATSAECHLICH zu versuchen, das Grafikgeraet zu nutzen -- und haengt
sofort hier, weil hinter `0x4AB668` immer noch kein echtes Geraet steckt,
mit dem es sprechen koennte.

**`sub_00040A88`s eigener Kommentar deckt dieses Auftreten nicht ab.** Jene
2026-09-05/11-Untersuchung erwaehnte `sub_004A12B0` ("...warum die gesamte
Kette ... aussieht, als spinne sie ewig in sub_004A12B0...") wegen eines
voellig ANDEREN Symptoms -- ein UMOD-Archiv-Reader, der wegen einer
falschen Seek-Position an einer voellig falschen Datei-Anzahl haengen blieb,
was jener Fix (der bereits in `recomp_manual.c` vorhandene
`sub_00040A88`-Override) bereits geloest hat. Das ist ein strukturell
ANDERES Haengenbleiben in DERSELBEN Funktion (ein echtes Hardware-Register-
Polling ohne Backing-Geraet, keine Software-Parsing-Schleife mit falscher
Iterationszahl) -- das Vorhandensein des frueheren Fixes bestaetigt, dass
diese Adresse ein wiederkehrender Interessenpunkt in dieser Codebasis ist,
nicht dass sie fuer DIESE Ursache bereits geloest ist.

**Umfangs-Einschaetzung, nicht begonnen:** dies richtig zu loesen bedeutet,
ein echtes NV2A-/D3D-Geraet zu verdrahten (Erstellung der
`0x4AB668`-Struktur, und mindestens ein Stub-PFIFO-/PGRAPH-Register-Modell,
das Kicks quittiert) -- ein substantielles neues Subsystem, kein begrenzter
Bugfix, und genau der bereits im Gedaechtnis markierte "kein Video-Backend
verdrahtet"-Meilenstein, kein schneller Patch. Kein Fix diese Runde
versucht; das ist eine Diagnose- und Umfangs-Entscheidung, passend zum
"guenstigen Erreichbarkeits-Check", den die Standing-Gedaechtnis-Notiz
verlangte.

**Modellstufe:** die Diagnose selbst war STRONG-angemessen (begrenzt, per
einem Live-Read bestaetigt). Ein Video-Backend tatsaechlich zu
implementieren ist eine andere, viel groessere Arbeitsklasse und sollte
eine eigene Scoping-Diskussion bekommen, bevor begonnen wird.
"""

new_raw = raw.rstrip(b"\r\n") + b"\r\n" + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
