# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 228 (2026-09-21/22): GRUNDURSACHE BESTAETIGT, noch kein Fix -- die 0x4AB668-Korruption ist eine echte architektonische Luecke im Speicher-Aperture-Modell dieser Rekompilierung: das Contiguous-Memory-Fenster (0x80000000+) ist absichtlich NICHT mit demselben physischen Speicher wie das Basis-RAM hinterlegt, aber die Tiled Aperture (0xF0000000+) ist NUR ein Alias des Basis-RAM -- eine ueber ihren Tiled-Alias angesprochene Contig-allozierte Oberflaeche landet daher still auf unzusammenhaengenden Basis-RAM-Bytes statt auf ihrem eigenen Speicher

**Laut expliziter Nutzer-Anweisung ist dieser Eintrag reines Audit: kein
Patch angewendet.** Beauftragt, `xbox_ContiguousAlloc`/das Tiled-Memory-
Modell direkt zu auditieren (Freibereichs-Verfolgung, ob `0xF0464000`
ueberhaupt allozierbar sein sollte nach dem eigenen Speicher-Layout
dieses Projekts, Grenzen/Reservierungen/Ausschluesse gegen die echten
NV2A-/MMIO-/Framebuffer-Aperturen, und explizit 32-Bit-Overflow/
Wraparound, inclusive/exclusive Endadressen, Alignment und
Groessenarithmetik) statt den mechanischen Rueckwaerts-Call-Chain-Trace
aus den Updates 223-227 fortzusetzen.

### Die drei Aperturen und wie jede in dieser Rekompilierung tatsaechlich hinterlegt ist

`src/kernel/xbox_memory_layout.c`s eigenes Setup und die eigenen
Kommentare fuer alle drei relevanten Guest-Adressfenster gelesen:

1. **Basis-RAM** (`XBOX_MAP_START = 0x00000000`, `XBOX_TOTAL_RAM` = 64
   MB): hinterlegt durch `g_memory_base`, eine `CreateFileMappingA` +
   `MapViewOfFileEx`-Section -- absichtlich ein *File-Mapping* (nicht
   simples `VirtualAlloc`), speziell damit andere Aperturen dieselben
   physischen Bytes ueber zusaetzliche Views derselben Section teilen
   koennen (der eigene Kommentar der Datei: "Using file mapping instead
   of VirtualAlloc allows us to map the same physical pages at multiple
   virtual addresses via MapViewOfFileEx").
2. **Tiled Aperture** (`XBOX_TILED_BASE = 0xF0000000`): `g_tiled_view =
   MapViewOfFileEx(g_mapping_handle, ..., tiled_native, ...)` -- eine
   echte *View derselben File-Mapping-Section* wie das Basis-RAM. Der
   eigene Kommentar bestaetigt die Absicht praezise: "A view of the same
   section rather than fresh storage: the title writes a surface through
   the tiled address and reads it back through the normal one, so the
   two have to be the same bytes." **Diese Apertur aliast nur, und
   korrekt, das Basis-RAM.**
3. **Contiguous-Fenster** (`XBOX_CONTIG_BASE = 0x80000000`,
   `XBOX_CONTIG_SIZE` = 64 MB, beide aus `src/kernel/kernel.h`):
   hinterlegt durch `g_contig_memory`, ein **separates, simples
   `VirtualAlloc`** -- ausdruecklich und absichtlich *keine* View
   derselben File-Mapping-Section. Der Code-Kommentar selbst nennt die
   Begruendung direkt: "Deliberately NOT a view of the 64 MB RAM
   mapping. On hardware this window aliases physical RAM, but we load
   the XBE image into the low addresses of that same region, so
   aliasing would put a title's pinned pools on top of its own code.
   Separate storage costs an extra mapping and behaves correctly;
   nothing here depends on the aliasing."

### Wo die Annahme bricht

Genau diese letzte Kommentar-Praemisse -- "nothing here depends on the
aliasing" -- ist der Kern, und diese Untersuchung hat jetzt einen
konkreten Fall gefunden, in dem sie nicht zutrifft. Dieses Projekts
eigene weitere Kommentare dokumentieren den *beabsichtigten* echten
Hardware-Rundlauf praezise: "`MmAllocateContiguousMemory` hands back
physical memory, and on Xbox physical page P is visible at `0x80000000 +
P`" (`xbox_memory_layout.c`, oberhalb von `xbox_ContiguousAlloc`), und
separat "Xbox D3D writes its pushbuffer position to the NV2A as `VA &
0x0FFFFFFF` and reads the GPU's position back as `GET | 0x80000000`" --
d.h. **dieselbe physische Seitennummer `P` soll auf echter Hardware ueber
*drei* Aperturen erreichbar sein: Basis-RAM (`0x00000000 + P`), das
Contiguous-Fenster (`0x80000000 + P`), und die Tiled Aperture
(`0xF0000000 + P`), alle als Views eines einzigen, vereinheitlichten
physischen Adressraums.**

In dieser Rekompilierung wird `P` von **zwei unabhaengigen Zaehlern
verfolgt, die beide bei Null anfangen und nichts voneinander wissen**:
der eigene belegte Bereich des Basis-RAM (ausfuehrbares Abbild, statische
Daten -- einschliesslich des Device-Pointer-Globals bei Guest-VA
`0x004AB668`, was eine *physische* Adresse im `0x00000000+`-Sinne ist)
und `xbox_ContiguousAlloc`s Bump-Allokator `g_contig_next` (beginnend bei
`XBOX_CONTIG_BASE`, hochzaehlend ab seiner eigenen Null). **Eine
Contig-Allokation, die `0x464000` Bytes in das 64-MB-Contig-Fenster
faellt, bekommt `P = 0x464000`.** Wenn der eigene (getreu uebersetzte,
byte-verifizierte) D3D8-/Treiber-Code des Spiels die Adresse dieser
Oberflaeche in ihre Tiled-Aperture-Form ueber genau die dokumentierte
Formel umwandelt (`0xF0000000 | (VA & 0x0FFFFFFF)`), berechnet er
`0xF0464000` -- und die Tiled Aperture dieser Rekompilierung, die *nur*
mit dem File-Mapping des Basis-RAM verdrahtet ist, loest diese Adresse zu
**dem eigenen physischen Offset `0x464000` des Basis-RAM** auf, nicht zu
`g_contig_memory`s Speicher ueberhaupt. Da `0x004AB668` (der
Device-Pointer) eine *Basis-RAM*-Adresse ist, deren eigener physischer
Offset (`0x4AB668`) zufaellig innerhalb der Zeilenkopie-Zielspanne von
2560 Bytes pro Zeile, 480 Zeilen, beginnend bei physischem Offset
`0x464000` liegt (Spanne `[0x464000, 0x590000)`, laut Update 226),
ueberschreibt die Kopie ihn still -- **keine Grenzverletzung durch die
eigene Logik der Kopie, und kein Overflow/Off-by-One in
`xbox_ContiguousAlloc`s eigener Arithmetik (dessen Erschoepfungs-Pruefung
verwendet bereits korrekt `uint64_t`, um 32-Bit-Wraparound zu vermeiden,
und dessen Alignment-/Groessen-Handling ist unauffaellig), sondern eine
strukturelle Konsequenz davon, dass zwei unabhaengig nummerierte
"physische" Raeume durch eine Aliasing-Apertur geleitet werden, die nur
mit einem von beiden geteilt ist.**

### Beantwortung der spezifischen Audit-Fragen

- **Freibereichs-Verfolgung:** `xbox_ContiguousAlloc` ist ein reiner
  Bump-Allokator (`g_contig_next`, nie freigegeben -- "contiguous blocks
  are framebuffers and pushbuffers, which a title allocates once", laut
  eigenem Kommentar), begrenzt nur durch `XBOX_CONTIG_BASE +
  XBOX_CONTIG_SIZE - XBOX_GPU_INSTANCE_DEFAULT`. Er hat ueberhaupt kein
  Konzept von, und keine Interaktion mit, dem eigenen belegten Bereich
  des Basis-RAM -- per Design, da beide unabhaengiger Speicher sein
  sollen (und es fuer jeden Zugriffspfad ausser der Tiled Aperture auch
  erfolgreich sind).
- **Ist `0xF0464000` "erlaubt" nach dem eigenen Speicher-Layout dieses
  Projekts?** Ja, bedingungslos -- `xbox_ContiguousAlloc` hat ueberhaupt
  keine Ausschluss-Logik, die auf die Tiled Aperture, die
  NV2A-MMIO-Apertur, oder das eigene Layout des Basis-RAM verweist, und
  keine der drei ist sich der belegten Bereiche der anderen beiden
  ueberhaupt bewusst.
- **Vergleich gegen die tatsaechlichen NV2A-/MMIO-/Framebuffer-Mappings:**
  die NV2A-Register-Apertur (`XBOX_NV2A_BASE = 0xFD000000`, 16 MB,
  hinterlegt durch ihr eigenes separates `g_nv2a_memory`-`VirtualAlloc`)
  und die MCPX-Apertur sind beide unabhaengig von dieser Frage -- sie
  ueberlappen weder `0xF0000000-0xF4000000` (die eigene Spanne der Tiled
  Aperture, begrenzt durch `xbox_TiledApertureSize()`) noch
  `0x80000000-0x84000000` (Contiguous-Fenster). Die Kollision ist
  spezifisch zwischen dem Tiled-*Alias des Basis-RAM* und
  *Contiguous-Fenster*-Adressen, nicht mit einer der beiden
  Hardware-MMIO-Aperturen.
- **32-Bit-Overflow/Wraparound, inclusive/exclusive Grenzen, Alignment,
  Groessenarithmetik:** alle einzeln geprueft und sauber.
  `xbox_ContiguousAlloc`s Erschoepfungs-Pruefung castet zu `uint64_t` vor
  dem Vergleich, kann also nicht wrappen. Alignment wird vor der
  Bump-Pointer-Arithmetik auf ein 4096-Byte-Minimum erzwungen, was fuer
  keine realistische Groesse ueberlaufen kann. Der RAM-Mirror-Code an
  anderer Stelle in derselben Datei (fruehere Lesung aus Update 224/225)
  behandelt bereits 64-MB-Vielfaches-Wraparound absichtlich und korrekt,
  aus einem *anderen* Grund (der dokumentierte Adressbus-Wrap. **Keiner
  davon ist hier der Bug** -- der Bug ist die oben beschriebene
  Aliasing-Luecke, die unabhaengig davon existiert, wie sorgfaeltig die
  Arithmetik eines einzelnen Allokators geschrieben ist.

### Warum das vom frueheren (Update 223) Hardware-Watchpoint oder der RAM-Mirror-Hypothese nicht gefangen wurde

Das ist dieselbe grundlegende Erklaerungs-Kategorie, die in Update 224
als "Arbeitshypothese" (Host-Speicher-Aliasing) vorgeschlagen und in
Update 225 auf "die Tiled Aperture, konkret" verfeinert wurde -- dieser
Eintrag vervollstaendigt diesen Faden, indem er identifiziert, *welche*
zwei Guest-Adressbereiche faelschlich als aliasend behandelt werden
(Basis-RAM und das Contiguous-Fenster, ueber die Tiled Aperture) und
*warum* (eine absichtliche, dokumentierte, aber -- fuer dieses eine
Zugriffsmuster -- falsche Vereinfachung), statt eines Mirror-Wraps oder
eines Kopierschleifen-Bugs.

### Noch nicht getan

Kein Fix implementiert, laut Anweisung. Das ist jetzt eine echte
Design-Entscheidung, keine Ein-Zeilen-Korrektur: moegliche Richtungen
umfassen (a) die Tiled Aperture korrekt sowohl vom Basis-RAM als auch vom
Contiguous-Fenster bei ihren jeweiligen physischen Offsets erreichbar zu
machen (erfordert zu wissen, zu welcher Apertur ein gegebener physischer
Offset "gehoert", was das aktuelle flache File-Mapping-Design nicht
verfolgt), oder (b) dem Contig-Allokator seine eigene, dedizierte, nicht
ueberlappende Scheibe des Tiled-Aperture-Adressraums zu geben, sodass
seine Offsets nie mit dem eigenen Layout des Basis-RAM zusammenfallen,
oder (c) etwas anderes -- diese Runde nicht entschieden oder weiter
gescoped. Kein Clamp, kein gefaelschter Zeiger, kein Workaround. Dieser
Eintrag hat keinerlei Quelltextaenderungen vorgenommen (reines Lesen/
Analysieren bereits committeten, getrackten Codes); `git status`/`git
diff` sind sauber.
"""

new_raw = raw.rstrip(b"\r\n") + b"\r\n" + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
