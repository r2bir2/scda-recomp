# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 229 (2026-09-22): FIX IMPLEMENTIERT fuer die 0x4AB668-Korruption -- ein echter physischer Speicher-Alias fuer dynamisch bump-allozierten Contiguous-Speicher, per Startup-Selbsttest verifiziert und in einem laengeren Lauf ohne jede Wiederholung der alten Korruptions-/Crash-Signatur bestaetigt

Auf explizite Nutzer-Anweisung wurde eine reine Workaround-Loesung
abgelehnt zugunsten eines Fixes der von Update 228 identifizierten
architektonischen Luecke: das Contiguous-Memory-Fenster (0x80000000+) war
mit Speicher ohne physische Identitaet zum Basis-RAM hinterlegt, sodass
der Tiled-Alias einer Contig-Oberflaeche still auf Basis-RAM-Bytes am
selben physischen Offset zugriff statt auf die eigenen Bytes der
Oberflaeche.

### Der Fix: ein zweiter, echt aliasierter View fuer dynamische Contig-Allokationen

`src/kernel/xbox_memory_layout.c`:

- **Neue Globals** (nahe `g_contig_memory` deklariert, vor der Stelle wo
  `xbox_ContiguousAlloc` sie braucht): `g_contig_alias_view` (das neue
  Mapping), `g_contig_alias_base` (dessen Guest-VA), `g_contig_alias_need`
  (wie weit die Tiled Aperture reichen muss, um es abzudecken), sowie
  `g_contig_next`/`g_contig_top`, nach oben verschoben von ihrer alten
  Stelle neben `xbox_ContiguousAlloc`, damit `xbox_MemoryLayoutInit` sie
  setzen kann, bevor diese Funktion je aufgerufen wird.
- **Wahl des physischen Offsets**: `contig_alias_phys_off =
  max(g_memory_size, XBOX_CONTIG_SIZE)` -- der erste physische Offset, der
  garantiert sowohl hinter dem vollstaendig beanspruchten eigenen Layout
  des Basis-RAM (Image + Stack + Heap, das exakt bis `g_memory_size` reicht,
  via `XBOX_HEAP_TOP`) als auch hinter der eigenen `VirtualAlloc`-Spanne
  des Legacy-Contig-Fensters liegt, sodass der neue Bereich keines von
  beiden je ueberlappen kann. Berechnet und gespeichert
  (`g_contig_alias_base`, `g_contig_alias_need`) direkt nach `g_memory_size`
  selbst, noch bevor `CreateFileMappingA` ueberhaupt aufgerufen wird.
- **File-Mapping vergroessert**: dieselbe `g_mapping_handle`-Section, die
  fuer Basis-RAM, dessen Mirrors und die Tiled Aperture verwendet wird, ist
  jetzt `g_contig_alias_need` Bytes gross (`g_memory_size` plus ein
  `XBOX_CONTIG_SIZE`-Schwanz) statt exakt `g_memory_size`. Der Basis-View
  und jeder RAM-Mirror sind unveraendert -- sie mappen weiterhin nur
  File-Offset `[0, g_memory_size)` -- das kostet also nichts fuer bereits
  funktionierendes und kann die Lage der 64-MB-Adressbus-Wraparound-Mirrors
  nicht verschieben (ein echtes, tragendes Verhalten fuer andere Titel;
  siehe Kommentar am Mirror-Setup).
- **Neuer View direkt nach dem Legacy-Contig-Fenster gemappt, *vor* der
  RAM-Mirror-Schleife**: `g_contig_alias_view = MapViewOfFileEx(
  g_mapping_handle, ..., contig_alias_phys_off, XBOX_CONTIG_SIZE,
  XBOX_CONTIG_BASE + contig_alias_phys_off)`. Diese Reihenfolge hat waehrend
  der eigenen Verifikation dieser Sitzung einen echten Bug aufgedeckt
  (siehe unten) -- der View muss vor der Mirror-Schleife committen, nicht
  danach, sonst beansprucht ein generischer Wraparound-Mirror dieselbe
  Adresse zuerst.
- **`xbox_TiledApertureSize()` erweitert**, um zusaetzlich
  `g_contig_alias_need` abzudecken (weiterhin begrenzt durch dieselben zwei
  harten Obergrenzen -- 0xFD000000 und den 4-GB-Wrap -- die die Funktion
  bereits kodierte), damit das eigene `MapViewOfFileEx` der Tiled Aperture
  auch den physischen Offset des neuen Bereichs erreicht.
- **Bump-Pointer von `xbox_ContiguousAlloc` umgeleitet**: bei Erfolg
  wandern `g_contig_next`/`g_contig_top` in den Alias-Bereich (kein
  `XBOX_GPU_INSTANCE_DEFAULT`-Abzug dort noetig --
  `bridge_MmClaimGpuInstanceMemory` berechnet seine eigene Adresse immer
  direkt aus dem Legacy-`XBOX_CONTIG_BASE`-Fenster und konsultiert diesen
  Bump-Pointer nie, die beiden Mechanismen bleiben also unabhaengig wie
  zuvor). Faellt bei jedem Fehlschlag (Mapping-Fehler, Tiled Aperture
  reicht nicht weit genug, oder der Selbsttest selbst schlaegt fehl) auf
  das Legacy-, nicht-aliasierte Fenster zurueck (heutiges altes Verhalten,
  bekannte Luecke), statt den Start zu verweigern -- passend zur
  Fehlschlag-Policy jeder anderen Apertur in dieser Funktion.
- **`fence_readable()` erweitert**, um auch den Alias-Bereich als
  hinterlegt zu erkennen, damit der NV2A-Fence-Polling-Thread weiterhin
  einer Kette in ein GPU-geschriebenes Semaphor folgt, das in einem
  dynamisch allozierten Contig-Puffer lebt.
- **Das Legacy-Fenster (`g_contig_memory`, standardmaessig
  0x80000000-0x84000000) bleibt komplett unangetastet**: weiterhin sein
  eigenes unabhaengiges `VirtualAlloc`, weiterhin das, was Halos feste
  Low-Address-Pins und die fake Kernel-Seite bei 0x80010000 nutzen. Nichts
  davon ist ueber die Tiled Aperture erreichbar, daher war der fehlende
  Drei-Wege-Alias, den dieser Fix adressiert, dafuer nie beobachtbar --
  siehe den langen Kommentar an diesem Block fuer die volle Begruendung.

### Startup-Selbsttest

Laeuft, sobald die Tiled Aperture gemappt ist (das Mapping des Alias-Views
selbst passiert frueher -- siehe die Mirror-Kollisions-Notiz unten --, aber
der Beweis des Round-Trips braucht das bereits existierende Tiled-Fenster):
schreibt einen Marker ueber den Contig-Alias, liest ihn ueber die Tiled
Aperture am passenden physischen Offset zurueck, schreibt dann einen
anderen Marker ueber Tiled und liest ihn ueber Contig zurueck. Beide
Richtungen muessen uebereinstimmen, sonst faellt der Fix auf das Legacy-
Fenster zurueck und gibt eine WARNING aus statt einem unbewiesenen Mapping
stillschweigend zu vertrauen. In den Testlaeufen dieser Sitzung bestaetigt
bestanden (`Contig alias verified (both directions, contig<->tiled)`).

**Kein Drei-Wege-Test.** Das Basis-RAM selbst erreicht diesen Bereich
absichtlich nicht (siehe den langen Kommentar an `g_contig_alias_view` im
Quelltext) -- die eigene Reichweite des Basis-RAM dorthin zu erweitern
wurde verworfen, weil es entweder die Mirror-Wraparound-Grenze verschieben
wuerde (was das dokumentierte 64-MB-Wrap-Verhalten des RenderWare-Memory-
Walkers fuer andere Titel bricht) oder eine Verkleinerung der eigenen
Heap-Obergrenze noetig machen wuerde, um im selben 64-MB-Bereich echt
freien Platz zu schaffen (eine echte Reduktion dessen, was ein Titel
allozieren kann). Der Zwei-Wege-Alias Contig<->Tiled, den dieser Fix
liefert, reicht aus: die Bytes einer dynamischen Contig-Allokation koennen
physisch nicht mehr mit irgendeinem Basis-RAM-Global zusammenfallen, weil
die beiden ueberhaupt keinen physischen Offset mehr teilen -- es gibt
nichts mehr, was ein Drei-Wege-Test noch zusaetzlich beweisen muesste.

### Ein echter Bug, waehrend der eigenen Verifikation dieser Sitzung gefunden: Mirror-/Alias-Adresskollision

Der erste Build dieses Fixes mappte `g_contig_alias_view` *nach* der Tiled
Aperture (damit der Selbsttest sofort laufen konnte), was ihn auch nach
die RAM-Mirror-Schleife stellte. Die Mirror-Schleife kachelt den gesamten
4-GB-Bereich oberhalb von `g_memory_base` in 64-MB-Schritten, ohne zu
wissen, was ein bestimmter Abschnitt konzeptionell bedeutet, und Mirror-
Index 32 (0-basiert) deckt genau Guest `[0x84000000, 0x88000000)` ab --
die von diesem Fix gewaehlte Alias-Adresse. Dieser Mirror mappte zuerst
erfolgreich (noch nichts belegte die Adresse), und das eigene
`MapViewOfFileEx` von `g_contig_alias_view` schlug daraufhin mit
`ERROR_INVALID_ADDRESS` (Fehler 487) gegen eine Adresse fehl, die frei
aussah, es aber nicht war:

```
WARNING: contig alias view at 0x84000000 failed (error 487); dynamic
contiguous allocations will use the legacy window and remain subject to
the tiled-aperture aliasing gap (Update 228)
```

Behoben durch Verschieben des Alias-View-Mappings an die Stelle direkt
nach dem Legacy-Contig-Fenster, *vor* die Mirror-Schleife -- dieselbe
Position, die das Legacy-Fenster und die Kernel-Seite bereits einnehmen.
Der eigene `VirtualQuery`-basierte Fallback der Mirror-Schleife fuer
belegte Bereiche (bereits vorhanden, genau fuer diese Situation -- siehe
deren eigenen Kommentar zum Legacy-Contig-Fenster, NV2A, MCPX und Flash)
erkennt den nun bereits committeten Alias-Bereich daraufhin korrekt und
faellt darueber weg/ueberspringt ihn:

```
Mirror 32: FAILED at 0000700080000000 (error 487)   <- Legacy-Contig-Fenster (unveraendert, bereits vorher so)
Mirror 33: FAILED at 0000700084000000 (error 487)   <- neuer Alias-Bereich (erwartet, jetzt korrekt erkannt)
```

Der Selbsttest selbst bleibt an seiner Stelle (nach dem Mapping der Tiled
Aperture), da er nur braucht, dass der View bereits existiert, nicht dass
er genau an dieser Stelle in der Funktion gemappt wird.

### Verifikation

Vollstaendiger Pipeline-Rebuild (`cmake --build build/scda_upgrade --config
RelWithDebInfo --target scda_recomp`), sauber bis auf bereits vor dieser
Sitzung vorhandene Warnungen (implizite `getenv`/`strtoul`/`atoi`/`_exit`-
Deklarationen, eine `old_protect`-Shadow-Warnung).

Zwei unabhaengige Testlaeufe (`scda_recomp.exe`, `build/scda_upgrade/bin/
RelWithDebInfo`), ca. 20 s bzw. ca. 100 s, beide bei ueber 180.000
Kernel-Calls und 5.000+ Runtime-Log-Zeilen:

- `Contig alias verified (both directions, contig<->tiled)` in beiden
  ausgegeben.
- Null Vorkommen, in beiden vollstaendigen Logs, des alten Korruptionswerts
  (`7400087D`), der alten Crash-Stelle (`004A12B0`), einer
  `[CONTIG] arena exhausted`-Meldung, oder jeglichen Access-Violation-/
  Exception-Texts.
- Beide Laeufe stabilisieren sich unabhaengig von der Laufzeit auf denselben
  Steady-State-Loop (wiederholte `[KERNEL] summary`-Zeilen, Ordinale
  277/246/294 -- `RtlEnterCriticalSection`/`RtlLeaveCriticalSection`/
  `ObReferenceObjectByHandle`, konsistent mit dem eigenen Critical-Section-
  Polling eines Bink-Worker-Threads, kein neuer Fehler oder Hang durch
  diese Aenderung), es handelt sich also um ein Plateau, nicht um einen
  abgeschnittenen Lauf. Ob dieser Steady State selbst der naechste echte
  Blocker ist, oder ein Artefakt fehlender echter Disc-Daten in dieser
  Testumgebung, ist **noch nicht geklaert** -- fuer den naechsten Eintrag
  vorgemerkt statt in eine Richtung angenommen.

Nicht in dieser Runde erledigt: die genaue ehemalige Korruptionsstelle
(`sub_004AED5D`s Zeilenkopie / `sub_004B1377`s Konstruktionspfad) live
unter `cdb` einzeln durchzuschreiten, um zuzusehen, wie sie 0x004AB668
*nicht* mehr korrumpiert -- der Selbsttest beweist den zugrundeliegenden
Aliasing-Mechanismus direkt, und der laengere Lauf zeigt keine
Wiederholung, das ist die Evidenz, auf die sich dieser Eintrag stuetzt;
eine Live-Rueckbestaetigung genau an der urspruenglichen Stelle waere
strikt gruendlicher, war aber nicht noetig, um zu diesem Schluss zu
kommen.
"""

new_raw = raw.rstrip(b"\r\n") + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
